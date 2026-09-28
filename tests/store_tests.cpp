// M9: order-store tests. Every store variant runs the same random add/find/remove
// sequence as a std::unordered_map reference and must agree after every operation.
// Small starting tables force wrap-around, long probe runs, growth and tombstone rebuilds.

#include "core/order_store.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>

namespace {

int failures = 0;

bool same(const ::Order* a, const ::Order& b) {
    return a && a->price == b.price && a->shares == b.shares && a->locate == b.locate && a->side == b.side;
}

template <typename StoreT>
void differential(const std::string& name, std::uint64_t key_range, int ops, std::uint32_t seed) {
    StoreT store;
    std::unordered_map<std::uint64_t, ::Order> ref;
    std::mt19937_64 rng(seed);
    int errors = 0;
    for (int i = 0; i < ops && errors < 5; ++i) {
        // keys 1..key_range (0 is reserved), biased to a sliding window like real order refs
        const std::uint64_t base = static_cast<std::uint64_t>(i) / 4;
        const std::uint64_t key = 1 + (base + rng() % key_range) % (key_range * 8);
        const auto op = rng() % 10;
        if (op < 5) {
            const ::Order o{static_cast<std::uint32_t>(rng()), static_cast<std::uint32_t>(rng() % 1000 + 1),
                                static_cast<std::uint16_t>(rng()), rng() % 2 ? Side::Buy : Side::Sell};
            store.add(key, o);
            ref[key] = o;
        } else if (op < 8) {
            store.remove(key);
            ref.erase(key);
        }
        const auto it = ref.find(key);
        const ::Order* got = store.find(key);
        if ((it == ref.end()) != (got == nullptr) || (got && !same(got, it->second))) {
            ++errors;
            std::cout << "FAIL " << name << ": op " << i << " key " << key << " mismatch\n";
        }
        if (store.size() != ref.size()) {
            ++errors;
            std::cout << "FAIL " << name << ": op " << i << " size " << store.size() << " want " << ref.size() << '\n';
        }
    }
    // full sweep: everything in the reference must be found, and iteration must match
    std::size_t seen = 0;
    store.for_each([&](std::uint64_t k, const ::Order& o) {
        ++seen;
        const auto it = ref.find(k);
        if (it == ref.end() || !same(&o, it->second)) ++errors;
    });
    for (const auto& [k, o] : ref)
        if (!same(store.find(k), o)) ++errors;
    if (seen != ref.size()) ++errors;
    if (errors) {
        ++failures;
        std::cout << "FAIL " << name << " (" << errors << " errors)\n";
    }
}

template <typename StoreT>
void run_all(const std::string& name) {
    differential<StoreT>(name + " dense", 500, 200'000, 1);
    differential<StoreT>(name + " sparse", 100'000, 200'000, 2);
}

} // namespace

int main() {
    using namespace itch;
    using enum Layout;
    using enum EmptyMark;
    using enum HashFn;
    using enum Erase;
    run_all<MapStore>("MapStore");
    run_all<DirectStore>("DirectStore");
    run_all<FlatStore<AoS, SentinelKey, Identity, BackwardShift, 16>>("AoS/sentinel/identity/backshift");
    run_all<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, 16>>("AoS/sentinel/fibonacci/backshift");
    run_all<FlatStore<SoA, SentinelKey, Identity, BackwardShift, 16>>("SoA/sentinel/identity/backshift");
    run_all<FlatStore<AoS, MetaByte, Identity, BackwardShift, 16>>("AoS/metabyte/identity/backshift");
    run_all<FlatStore<AoS, SentinelKey, Identity, Tombstone, 16>>("AoS/sentinel/identity/tombstone");
    run_all<FlatStore<SoA, MetaByte, Fibonacci, Tombstone, 16>>("SoA/metabyte/fibonacci/tombstone");
    run_all<Store>("Store (the default)");

    // Backward shift across the end of the table. Identity hash, 16 slots:
    //   14 -> slot 14 (home 14), 30 -> slot 15 (home 14), 16 -> slot 0 (home 0)
    // Deleting 14 must move 30 back into slot 14 but leave 16 at slot 0, its home.
    // Random tests rarely produce this; a wrap-around bug here survived them.
    {
        FlatStore<AoS, SentinelKey, Identity, BackwardShift, 16> w;
        w.add(14, Order{1, 1, 1, Side::Buy});
        w.add(30, Order{2, 2, 2, Side::Buy});
        w.add(16, Order{3, 3, 3, Side::Buy});
        w.remove(14);
        if (!w.find(30) || !w.find(16) || w.find(14) || w.size() != 2 || w.find(16)->price != 3) {
            ++failures;
            std::cout << "FAIL backward shift across the table end\n";
        }
    }

    // reserved keys are refused, not stored
    FlatStore<AoS, SentinelKey, Identity, BackwardShift, 16> s;
    s.add(0, Order{1, 1, 1, Side::Buy});
    s.add(~std::uint64_t{0}, Order{1, 1, 1, Side::Buy});
    if (s.size() != 0 || s.rejected() != 2 || s.find(0) != nullptr) {
        ++failures;
        std::cout << "FAIL reserved keys were stored\n";
    }

    std::cout << (failures ? "FAILED" : "OK") << " store tests\n";
    return failures ? 1 : 0;
}
