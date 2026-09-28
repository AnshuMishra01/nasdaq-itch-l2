#pragma once

#include "core/order.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace itch {

// Every store has the same interface, so BookHandler is a template over it:
//   void    add(ref, order)     insert, or overwrite if ref exists
//   Order*  find(ref)           nullptr if absent; valid until the next add/remove
//   void    remove(ref)         no-op if absent
//   size()  and  for_each(f(ref, const Order&))   (iteration order unspecified)

// ---- M4 baseline: node-based hash map ---------------------------------------------
class MapStore {
private:
    std::unordered_map<std::uint64_t, Order> orders;
public:
    MapStore() { orders.reserve(1000000); }

    void add(std::uint64_t orderRef, Order order) { orders[orderRef] = order; }

    Order* find(std::uint64_t orderRef) {
        auto it = orders.find(orderRef);
        return it != orders.end() ? &it->second : nullptr;
    }

    void remove(std::uint64_t orderRef) { orders.erase(orderRef); }

    template <typename F>
    void for_each(F&& f) const { for (const auto& [ref, o] : orders) f(ref, o); }

    std::size_t size() const { return orders.size(); }
};

// ---- M9: flat open-addressing table, every design choice a template parameter ------
// Linear probing, power-of-two capacity, load factor <= 0.5 (grows by doubling).

enum class Layout : std::uint8_t {
    AoS, // one array of {key, Order}: a probe that hits reads key and value together
    SoA, // keys[] and values[] apart: probing scans 8 keys per cache line
};
enum class EmptyMark : std::uint8_t {
    SentinelKey, // key 0 = empty (refs start at 42), key ~0 = tombstone
    MetaByte,    // separate uint8 per slot: 0 empty, 1 full, 2 tombstone
};
enum class HashFn : std::uint8_t {
    Identity,  // ref & mask
    Fibonacci, // (ref * 2^64/phi) >> (64 - bits)
};
enum class Erase : std::uint8_t {
    BackwardShift, // pull later members of the run back: no tombstones, runs stay tight
    Tombstone,     // mark deleted; rebuild when live + tombstones pass the load limit
};

template <Layout L, EmptyMark M, HashFn H, Erase D, std::size_t InitialCapacity = (std::size_t{1} << 21)>
class FlatStore {
    struct Slot {
        std::uint64_t key;
        Order value;
    };

    static constexpr std::uint64_t kEmptyKey = 0;
    static constexpr std::uint64_t kTombKey = ~std::uint64_t{0};
    static constexpr std::uint8_t kEmpty = 0, kFull = 1, kTomb = 2;

    std::size_t cap_ = 0;
    std::size_t mask_ = 0;
    unsigned shift_ = 0; // 64 - log2(cap)
    std::size_t size_ = 0;
    std::size_t tombs_ = 0;
    std::uint64_t rehashes_ = 0;
    std::uint64_t rejected_ = 0; // adds refused because the key is a reserved sentinel

    std::vector<Slot> slots_;          // AoS
    std::vector<std::uint64_t> keys_;  // SoA
    std::vector<Order> values_;        // SoA
    std::vector<std::uint8_t> meta_;   // MetaByte

    std::size_t home(std::uint64_t key) const {
        if constexpr (H == HashFn::Identity) return static_cast<std::size_t>(key) & mask_;
        else return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    std::uint64_t key_at(std::size_t i) const {
        if constexpr (L == Layout::AoS) return slots_[i].key;
        else return keys_[i];
    }
    Order& value_at(std::size_t i) {
        if constexpr (L == Layout::AoS) return slots_[i].value;
        else return values_[i];
    }
    const Order& value_at(std::size_t i) const {
        if constexpr (L == Layout::AoS) return slots_[i].value;
        else return values_[i];
    }

    bool is_empty(std::size_t i) const {
        if constexpr (M == EmptyMark::MetaByte) return meta_[i] == kEmpty;
        else return key_at(i) == kEmptyKey;
    }
    bool is_tomb(std::size_t i) const {
        if constexpr (D == Erase::BackwardShift) return false;
        else if constexpr (M == EmptyMark::MetaByte) return meta_[i] == kTomb;
        else return key_at(i) == kTombKey;
    }
    bool is_full(std::size_t i) const { return !is_empty(i) && !is_tomb(i); }

    void set_key(std::size_t i, std::uint64_t key) {
        if constexpr (L == Layout::AoS) slots_[i].key = key;
        else keys_[i] = key;
    }
    void put(std::size_t i, std::uint64_t key, const Order& v) {
        set_key(i, key);
        value_at(i) = v;
        if constexpr (M == EmptyMark::MetaByte) meta_[i] = kFull;
    }
    void mark_empty(std::size_t i) {
        if constexpr (M == EmptyMark::MetaByte) meta_[i] = kEmpty;
        else set_key(i, kEmptyKey);
    }
    void mark_tomb(std::size_t i) {
        if constexpr (M == EmptyMark::MetaByte) meta_[i] = kTomb;
        else set_key(i, kTombKey);
    }

    void allocate(std::size_t cap) {
        cap_ = cap;
        mask_ = cap - 1;
        shift_ = static_cast<unsigned>(64 - std::countr_zero(cap));
        if constexpr (L == Layout::AoS) slots_.assign(cap, Slot{kEmptyKey, Order{}});
        else {
            keys_.assign(cap, kEmptyKey);
            values_.assign(cap, Order{});
        }
        if constexpr (M == EmptyMark::MetaByte) meta_.assign(cap, kEmpty);
    }

    // Rebuild into new_cap slots, dropping tombstones.
    void rehash(std::size_t new_cap) {
        ++rehashes_;
        std::vector<std::pair<std::uint64_t, Order>> live;
        live.reserve(size_);
        for (std::size_t i = 0; i < cap_; ++i)
            if (is_full(i)) live.emplace_back(key_at(i), value_at(i));
        allocate(new_cap);
        size_ = 0;
        tombs_ = 0;
        for (const auto& [k, v] : live) {
            std::size_t i = home(k);
            while (!is_empty(i)) i = (i + 1) & mask_;
            put(i, k, v);
            ++size_;
        }
    }

    // Index of key, or cap_ if absent.
    std::size_t index_of(std::uint64_t key) const {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            if (is_empty(i)) return cap_;
            if (key_at(i) == key && !is_tomb(i)) return i;
        }
    }

public:
    FlatStore() {
        static_assert(std::has_single_bit(InitialCapacity) && InitialCapacity >= 16, "capacity must be a power of two >= 16");
        allocate(InitialCapacity);
    }

    Order* find(std::uint64_t key) {
        const std::size_t i = index_of(key);
        return i == cap_ ? nullptr : &value_at(i);
    }

    void add(std::uint64_t key, const Order& v) {
        if constexpr (M == EmptyMark::SentinelKey) {
            // 0 and ~0 mark empty/deleted slots. Nasdaq refs start at 1, but refuse loudly
            // rather than corrupt the table; the handler then counts later E/X/D as unknown.
            if (key == kEmptyKey || key == kTombKey) { ++rejected_; return; }
        }
        std::size_t first_tomb = cap_;
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (is_empty(i)) break;
            if (is_tomb(i)) {
                if (first_tomb == cap_) first_tomb = i;
                continue;
            }
            if (key_at(i) == key) { value_at(i) = v; return; } // overwrite, like operator[]
        }
        if (first_tomb != cap_) {
            i = first_tomb;
            --tombs_;
        }
        put(i, key, v);
        ++size_;
        if ((size_ + tombs_) * 2 > cap_) {
            // mostly tombstones: clean at the same size; otherwise grow
            rehash(size_ * 4 <= cap_ ? cap_ : cap_ * 2);
        }
    }

    void remove(std::uint64_t key) {
        std::size_t i = index_of(key);
        if (i == cap_) return;
        --size_;
        if constexpr (D == Erase::Tombstone) {
            mark_tomb(i);
            ++tombs_;
        } else {
            // Backward shift: walk the run after i; any entry whose home is not in (i, j]
            // cyclically would become unreachable across the hole, so move it into the hole.
            for (std::size_t j = (i + 1) & mask_; !is_empty(j); j = (j + 1) & mask_) {
                const std::size_t k = home(key_at(j));
                const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
                if (!stays) {
                    put(i, key_at(j), value_at(j));
                    i = j;
                }
            }
            mark_empty(i);
        }
    }

    template <typename F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < cap_; ++i)
            if (is_full(i)) f(key_at(i), value_at(i));
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return cap_; }
    std::uint64_t rehashes() const { return rehashes_; }
    std::uint64_t rejected() const { return rejected_; }

    // Diagnostics, not hot path: how far live entries sit from their home slot
    // (0 = found on the first probe). Mean and max over the current contents.
    std::pair<double, std::size_t> displacement() const {
        std::size_t total = 0, worst = 0, n = 0;
        for (std::size_t i = 0; i < cap_; ++i) {
            if (!is_full(i)) continue;
            const std::size_t d = (i - home(key_at(i))) & mask_;
            total += d;
            worst = d > worst ? d : worst;
            ++n;
        }
        return {n ? static_cast<double>(total) / static_cast<double>(n) : 0.0, worst};
    }

    std::size_t memory_bytes() const {
        std::size_t per_slot = L == Layout::AoS ? sizeof(Slot) : sizeof(std::uint64_t) + sizeof(Order);
        if (M == EmptyMark::MetaByte) per_slot += 1;
        return cap_ * per_slot;
    }
};

// ---- M9: direct indexing by ref (reference point: no hashing, no probing) -----------
// Memory is proportional to the largest ref seen, not to live orders.
class DirectStore {
    std::vector<Order> values_;
    std::vector<std::uint8_t> present_;
    std::size_t size_ = 0;

public:
    explicit DirectStore(std::size_t initial = std::size_t{1} << 23) : values_(initial), present_(initial, 0) {}

    Order* find(std::uint64_t ref) {
        return ref < present_.size() && present_[ref] ? &values_[ref] : nullptr;
    }

    void add(std::uint64_t ref, const Order& v) {
        if (ref >= present_.size()) {
            const std::size_t n = std::bit_ceil(static_cast<std::size_t>(ref) + 1);
            values_.resize(n);
            present_.resize(n, 0);
        }
        if (!present_[ref]) ++size_;
        present_[ref] = 1;
        values_[ref] = v;
    }

    void remove(std::uint64_t ref) {
        if (ref < present_.size() && present_[ref]) {
            present_[ref] = 0;
            --size_;
        }
    }

    template <typename F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < present_.size(); ++i)
            if (present_[i]) f(static_cast<std::uint64_t>(i), values_[i]);
    }

    std::size_t size() const { return size_; }
    std::size_t memory_bytes() const { return values_.size() * sizeof(Order) + present_.size(); }
};

// The store the program uses, chosen from the measurements in results/m9/ (see DECISION.md):
// flat AoS slots, key 0 = empty, identity hash, backward-shift delete, 2^21 slots to start.
using Store = FlatStore<Layout::AoS, EmptyMark::SentinelKey, HashFn::Identity, Erase::BackwardShift>;

} // namespace itch
