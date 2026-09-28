// M9 experiment: order-store variants, one design change at a time.
//
// For every variant: time store + books (the real workload) and store only, `reps` runs
// each, interleaved across variants so laptop drift hits all of them equally. Then check
// correctness: same state hash as the baseline, all anomaly counters zero, and the M7
// store-vs-book cross-check. Writes one metrics file per variant plus summary.md.
//
// usage: store_bench <file> [reps=3] [outdir=results/m9]

#include "parse.h"
#include "core/order_store.h"
#include "handlers/book_handler.h"
#include "handlers/discard_handler.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace itch;

std::vector<unsigned char> g_buf;
volatile std::uint64_t g_sink = 0;
constexpr std::uint64_t kGoldenHash = 0x96fd73902096c354ULL; // M7, unordered_map store

template <typename H>
double time_run(H& h, std::uint64_t& processed) {
    const auto t0 = std::chrono::steady_clock::now();
    const ParseResult r = parse_buffer(g_buf.data(), g_buf.size(), h);
    const auto t1 = std::chrono::steady_clock::now();
    if (!r.ok) { std::cerr << "parse error: " << r.error << '\n'; std::exit(1); }
    processed = r.processed;
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           static_cast<double>(r.processed);
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

struct Checks {
    std::uint64_t combined_hash = 0;
    std::uint64_t store_hash = 0;
    std::uint64_t store_only_store_hash = 0;
    std::uint64_t anomalies = 0; // 7 order-store counters + level_missing + level_underflow
    std::uint64_t crossed = 0;
    std::uint64_t peak_live = 0;
    bool verify_ok = false;
    std::string verify_line;
    std::string store_info; // capacity, memory, rehashes where the store reports them
};

struct Variant {
    std::string id;
    std::string what;
    std::string change; // the one thing that differs from its parent
    std::function<double()> run_full;
    std::function<double()> run_store_only;
    std::function<Checks()> check;
    std::vector<double> full, store_only;
};

template <typename StoreT>
std::string describe_store(const StoreT& s) {
    std::ostringstream o;
    if constexpr (requires { s.capacity(); }) o << "capacity " << s.capacity() << " slots, ";
    if constexpr (requires { s.memory_bytes(); })
        o << std::fixed << std::setprecision(1) << static_cast<double>(s.memory_bytes()) / (1024.0 * 1024.0) << " MB";
    else o << "memory n/a (node-based)";
    if constexpr (requires { s.rehashes(); }) o << ", rehashes " << s.rehashes();
    if constexpr (requires { s.rejected(); }) o << ", rejected refs " << s.rejected();
    if constexpr (requires { s.displacement(); }) {
        const auto [mean, worst] = s.displacement();
        o << ", probe distance mean " << std::setprecision(2) << mean << " max " << worst;
    }
    return o.str();
}

template <typename StoreT>
Variant make_variant(std::string id, std::string what, std::string change) {
    Variant v;
    v.id = std::move(id);
    v.what = std::move(what);
    v.change = std::move(change);
    // handlers are large (3 x 65,536-entry vectors + the store): heap, built outside the timer
    v.run_full = [] {
        auto h = std::make_unique<BasicBookHandler<StoreT, true>>();
        std::uint64_t n = 0;
        return time_run(*h, n);
    };
    v.run_store_only = [] {
        auto h = std::make_unique<BasicBookHandler<StoreT, false>>();
        std::uint64_t n = 0;
        return time_run(*h, n);
    };
    v.check = [] {
        Checks c;
        auto h = std::make_unique<BasicBookHandler<StoreT, true>>();
        std::uint64_t n = 0;
        time_run(*h, n);
        const auto hash = h->state_hash();
        c.combined_hash = hash.combined;
        c.store_hash = hash.store;
        c.anomalies = h->anomaly_exec_unknown + h->anomaly_cancel_unknown + h->anomaly_delete_unknown +
                      h->anomaly_replace_orig_unknown + h->anomaly_add_existing + h->anomaly_over_execution +
                      h->anomaly_over_cancel + h->level_missing + h->level_underflow;
        c.crossed = h->crossed_updates;
        c.peak_live = h->peak();
        const VerifyResult vr = h->verify_levels();
        c.verify_ok = vr.ok();
        std::ostringstream vo;
        BasicBookHandler<StoreT, true>::print_verify(vr, vo);
        c.verify_line = vo.str();
        c.store_info = describe_store(h->store);

        auto s = std::make_unique<BasicBookHandler<StoreT, false>>();
        time_run(*s, n);
        c.store_only_store_hash = s->state_hash().store;
        return c;
    };
    return v;
}

std::string hex(std::uint64_t x) {
    std::ostringstream o;
    o << std::hex << std::setw(16) << std::setfill('0') << x;
    return o.str();
}

std::string now_string() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "usage: " << argv[0] << " <file> [reps=3] [outdir=results/m9]\n";
        return 1;
    }
    const std::string path = argv[1];
    const int reps = argc > 2 ? std::stoi(argv[2]) : 3;
    const std::filesystem::path outdir = argc > 3 ? argv[3] : "results/m9";

    {
        std::ifstream in(path, std::ios::binary);
        if (!in) { std::cout << "cannot open " << path << '\n'; return 1; }
        g_buf.resize(static_cast<std::size_t>(std::filesystem::file_size(path)));
        in.read(reinterpret_cast<char*>(g_buf.data()), static_cast<std::streamsize>(g_buf.size()));
    }

    // Shorthands for the flat-table parameters.
    using enum Layout;
    using enum EmptyMark;
    using enum HashFn;
    using enum Erase;
    constexpr std::size_t k2M = std::size_t{1} << 21;

    // Each flat variant changes exactly ONE parameter of 01_flat_base.
    std::vector<Variant> variants;
    variants.push_back(make_variant<MapStore>(
        "00_unordered_map", "std::unordered_map<uint64, Order>, reserve(1M): the M4/M7 store",
        "baseline"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k2M>>(
        "01_flat_base", "flat table: AoS slots, key 0 = empty, identity hash, backward-shift delete, 2^21 slots",
        "vs 00: node-based map -> flat open addressing"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k2M>>(
        "02_hash_fibonacci", "01 with Fibonacci (multiplicative) hash",
        "vs 01: hash identity -> fibonacci"));
    variants.push_back(make_variant<FlatStore<SoA, SentinelKey, Identity, BackwardShift, k2M>>(
        "03_layout_soa", "01 with keys and values in separate arrays",
        "vs 01: layout AoS -> SoA"));
    variants.push_back(make_variant<FlatStore<AoS, MetaByte, Identity, BackwardShift, k2M>>(
        "04_empty_metabyte", "01 with a separate metadata byte per slot instead of key 0",
        "vs 01: empty marker sentinel key -> metadata byte"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Identity, Tombstone, k2M>>(
        "05_erase_tombstone", "01 with tombstone deletion",
        "vs 01: delete backward-shift -> tombstones"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, 1024>>(
        "06_capacity_grow", "01 starting at 1,024 slots and doubling at load 0.5",
        "vs 01: capacity 2^21 fixed -> 2^10 + growth"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, (std::size_t{1} << 23)>>(
        "07_capacity_2^23", "01 with 2^23 slots",
        "vs 01: capacity 2^21 -> 2^23"));
    variants.push_back(make_variant<DirectStore>(
        "08_direct_index", "vector indexed by order ref (memory grows with the largest ref, not live orders)",
        "reference point: no hashing at all"));
    // Round 2: 06 lost despite a 1.5 MB table. Growth, or the small table itself?
    // Fixed sizes with no rehash separate the two (27k peak live fits 2^16 at load 0.41).
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, (std::size_t{1} << 16)>>(
        "09_capacity_2^16", "01 with 2^16 slots fixed (same final size as 06, no growth)",
        "vs 01: capacity 2^21 -> 2^16; vs 06: growth removed"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, (std::size_t{1} << 18)>>(
        "10_capacity_2^18", "01 with 2^18 slots",
        "vs 01: capacity 2^21 -> 2^18"));
    variants.push_back(make_variant<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, (std::size_t{1} << 16)>>(
        "11_fibonacci_2^16", "02 (Fibonacci) in a 2^16 table: does scattering hurt less when the table fits in cache?",
        "vs 09: hash identity -> fibonacci"));

    // floor: parse + decode + discard, same session
    std::vector<double> floor_runs;

    std::cout << "M9 store bench: " << variants.size() << " variants x " << reps << " reps\n";
    for (int r = 0; r < reps; ++r) {
        DiscardHandler d;
        std::uint64_t n = 0;
        floor_runs.push_back(time_run(d, n));
        g_sink = g_sink + d.sum; // keeps the decode observable
        for (auto& v : variants) {
            v.full.push_back(v.run_full());
            v.store_only.push_back(v.run_store_only());
            std::cout << "  rep " << r + 1 << "  " << std::left << std::setw(20) << v.id << std::right << std::fixed
                      << std::setprecision(1) << std::setw(8) << v.full.back() << " full" << std::setw(8)
                      << v.store_only.back() << " store-only\n";
        }
    }
    const double floor_med = median(floor_runs);

    std::filesystem::create_directories(outdir);
    const Checks base = variants.front().check();
    const double base_full = median(variants.front().full);
    const double base_cost = median(variants.front().store_only) - floor_med;

    std::ostringstream summary;
    summary << "# M9 order-store experiment\n\n"
            << "- date: " << now_string() << "\n"
            << "- file: `" << path << "`, " << reps << " interleaved reps per variant, medians shown\n"
            << "- compiler: g++ " << __VERSION__
#ifdef __OPTIMIZE__
            << ", optimised"
#else
            << ", NOT OPTIMISED (numbers meaningless)"
#endif
#ifdef NDEBUG
            << ", NDEBUG"
#endif
            << "\n- floor (parse + decode + discard): " << std::fixed << std::setprecision(1) << floor_med << " ns/msg\n"
            << "- store cost = store-only median - floor\n"
            << "- correct = state hash equals the golden hash " << hex(kGoldenHash)
            << ", store hash equal with and without books, 0 anomalies, same crossed count as baseline, cross-check PASS\n\n"
            << "| variant | change | store+books ns/msg | store-only ns/msg | store cost | vs baseline cost | store | correct |\n"
            << "|---|---|---|---|---|---|---|---|\n";

    for (auto& v : variants) {
        const Checks c = v.check();
        const double full = median(v.full);
        const double so = median(v.store_only);
        const double cost = so - floor_med;
        const bool correct = c.combined_hash == kGoldenHash && c.store_hash == base.store_hash &&
                             c.store_only_store_hash == c.store_hash && c.anomalies == 0 &&
                             c.crossed == base.crossed && c.verify_ok;

        std::ofstream f(outdir / (v.id + ".txt"));
        f << std::fixed << std::setprecision(1);
        f << "M9 order-store experiment: " << v.id << "\n"
          << "date:     " << now_string() << "\n"
          << "file:     " << path << "\n"
          << "what:     " << v.what << "\n"
          << "change:   " << v.change << "\n"
          << "store:    " << c.store_info << " (at end of run; peak live orders " << c.peak_live << ")\n\n"
          << "Timing (ns/msg, " << reps << " reps interleaved with the other variants)\n"
          << "  store + books:";
        for (double x : v.full) f << ' ' << x;
        f << "   median " << full << "\n  store only:   ";
        for (double x : v.store_only) f << ' ' << x;
        f << "   median " << so << "\n  floor:        ";
        for (double x : floor_runs) f << ' ' << x;
        f << "   median " << floor_med << "\n"
          << "  store cost (store only - floor): " << cost << " ns/msg\n"
          << "  vs 00_unordered_map: store+books " << std::showpos << (full / base_full - 1.0) * 100.0
          << "%, store cost " << (cost / base_cost - 1.0) * 100.0 << std::noshowpos << "%\n\n"
          << "Correctness\n"
          << "  state hash:   " << hex(c.combined_hash) << (c.combined_hash == kGoldenHash ? "  MATCH golden" : "  MISMATCH golden") << '\n'
          << "  store hash:   " << hex(c.store_hash) << (c.store_hash == base.store_hash ? "  same as baseline" : "  DIFFERENT from baseline")
          << (c.store_only_store_hash == c.store_hash ? ", same without books" : ", DIFFERENT without books") << '\n'
          << "  anomalies:    " << c.anomalies << " (7 store counters + level_missing + level_underflow)\n"
          << "  crossed:      " << c.crossed << " (baseline " << base.crossed << ", PNRL halted)\n"
          << "  cross-check:\n" << c.verify_line
          << "  VERDICT:      " << (correct ? "CORRECT" : "WRONG - do not use") << '\n';

        summary << "| " << v.id << " | " << v.change << " | " << std::fixed << std::setprecision(1) << full << " | "
                << so << " | " << cost << " | " << std::showpos << std::setprecision(0)
                << (cost / base_cost - 1.0) * 100.0 << "%" << std::noshowpos << std::setprecision(1) << " | "
                << c.store_info << " | " << (correct ? "yes" : "**NO**") << " |\n";
        std::cout << "  " << std::left << std::setw(20) << v.id << std::right << (correct ? " CORRECT" : " WRONG") << '\n';
    }

    std::ofstream(outdir / "summary.md") << summary.str();
    std::cout << "wrote " << (outdir / "summary.md").string() << " and one .txt per variant\n";
    return 0;
}
