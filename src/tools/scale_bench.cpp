// Full-day scale: where does the time go once 1.7M orders are live?
//
//   scale_bench <file> io     [reps=3] [outdir=results/scale]
//       raw read speed, then floor / store-only / store+books, each with time in read()
//       and time parsing (decode + handler) measured separately
//   scale_bench <file> store  [reps=2] [outdir=results/scale]
//       order-store variants (table size x hash) at full scale, store only, parse time;
//       plus snapshots of load factor and probe distance as the day goes on
//   scale_bench <file> books  [-]      [outdir=results/scale]
//       book depth and update rank (distance from the best price), before and after 09:30
//
// Everything reads the file in 64 MB chunks (constant memory).

#include "parse.h"
#include "itch_io.h"
#include "latency.h"
#include "core/order_store.h"
#include "handlers/book_handler.h"
#include "handlers/discard_handler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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
using clk = std::chrono::steady_clock;

std::filesystem::path g_path;
volatile std::uint64_t g_sink = 0;

double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

struct Run {
    std::uint64_t msgs = 0;
    std::uint64_t wall_ns = 0;
    IoStats io;
    double parse_ns_per_msg() const { return msgs ? static_cast<double>(io.parse_ns) / static_cast<double>(msgs) : 0; }
    double wall_ns_per_msg() const { return msgs ? static_cast<double>(wall_ns) / static_cast<double>(msgs) : 0; }
    double read_mb_s() const { return io.read_ns ? static_cast<double>(io.bytes) / 1e6 / (static_cast<double>(io.read_ns) / 1e9) : 0; }
};

template <typename H>
Run run_chunked(H& h) {
    Run r;
    std::string err;
    const auto t0 = clk::now();
    const ParseResult pr = parse_chunked(g_path, h, 0, err, std::size_t{64} << 20, NoProbe{}, &r.io);
    r.wall_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
    if (!pr.ok) { std::cerr << "parse error: " << err << ' ' << pr.error << '\n'; std::exit(1); }
    r.msgs = pr.processed;
    return r;
}

std::string run_row(const std::string& label, const Run& r) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << "| " << label << " | " << ms(r.wall_ns) / 1000.0 << " s | "
      << ms(r.io.read_ns) / 1000.0 << " s (" << std::setprecision(0) << r.read_mb_s() << " MB/s) | "
      << std::setprecision(1) << ms(r.io.parse_ns) / 1000.0 << " s | " << r.parse_ns_per_msg() << " | "
      << r.wall_ns_per_msg() << " |\n";
    return o.str();
}
const char* kRunHeader = "| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |\n|---|---|---|---|---|---|\n";

// ---------------------------------------------------------------- io
int exp_io(int reps, std::ostream& out) {
    out << "## I/O vs processing\n\nEach run reads the file in 64 MB chunks. `in read()` is time blocked in "
           "`ifstream::read`; `parsing` is framing + decode + handler. Interleaved, " << reps << " reps.\n\n";
    std::vector<std::string> rows;
    for (int rep = 1; rep <= reps; ++rep) {
        {   // read only: how fast can the file be read at all?
            Run r;
            std::ifstream in(g_path, std::ios::binary);
            std::vector<char> buf(std::size_t{64} << 20);
            const auto t0 = clk::now();
            for (;;) {
                const auto t = clk::now();
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const auto got = static_cast<std::uint64_t>(in.gcount());
                r.io.read_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t).count());
                r.io.bytes += got;
                if (got == 0 || !in) break;
                g_sink = g_sink + static_cast<unsigned char>(buf[0]);
            }
            r.wall_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
            std::ostringstream o;
            o << std::fixed << std::setprecision(1) << "| read only, rep " << rep << " | " << ms(r.wall_ns) / 1000.0 << " s | "
              << ms(r.io.read_ns) / 1000.0 << " s (" << std::setprecision(0) << r.read_mb_s() << " MB/s) | - | - | - |\n";
            rows.push_back(o.str());
            std::cout << rows.back();
        }
        {
            DiscardHandler d;
            const Run r = run_chunked(d);
            g_sink = g_sink + d.sum;
            rows.push_back(run_row("floor (parse + decode, discard), rep " + std::to_string(rep), r));
            std::cout << rows.back();
        }
        {
            auto h = std::make_unique<BasicBookHandler<Store, false>>();
            rows.push_back(run_row("store only (default store), rep " + std::to_string(rep), run_chunked(*h)));
            std::cout << rows.back();
        }
        {
            auto h = std::make_unique<BasicBookHandler<Store, true>>();
            rows.push_back(run_row("store + books (default), rep " + std::to_string(rep), run_chunked(*h)));
            std::cout << rows.back();
        }
    }
    out << kRunHeader;
    for (const auto& r : rows) out << r;
    return 0;
}

// ---------------------------------------------------------------- store
struct Snapshot {
    std::uint64_t msgs, live, capacity;
    double load, mean_probe;
    std::size_t max_probe;
};

// Wraps a store-only handler; every `every` messages records load factor and probe distance.
template <typename StoreT>
struct Profiler {
    BasicBookHandler<StoreT, false> h;
    std::uint64_t n = 0, every = 8'000'000;
    std::vector<Snapshot> snaps;
    template <typename M>
    void on(const M& m) {
        h.on(m);
        if (++n % every == 0) {
            const auto [mean, worst] = h.store.displacement();
            snaps.push_back(Snapshot{n, h.store.size(), h.store.capacity(),
                                     static_cast<double>(h.store.size()) / static_cast<double>(h.store.capacity()), mean, worst});
        }
    }
};

struct StoreVariant {
    std::string id, what;
    std::function<Run(std::string&)> run; // also reports the store's state at the end of the run
    std::function<std::vector<Snapshot>()> profile;
    std::vector<Run> runs;
    std::string final_state; // capacity, load, probes, rehashes after the last timed run
};

template <typename StoreT>
std::string describe(const BasicBookHandler<StoreT, false>& h) {
    std::ostringstream o;
    o << std::fixed;
    if constexpr (requires { h.store.capacity(); }) {
        const auto [mean, worst] = h.store.displacement();
        o << "capacity " << h.store.capacity() << ", live " << h.store.size() << ", load " << std::setprecision(2)
          << static_cast<double>(h.store.size()) / static_cast<double>(h.store.capacity()) << ", probe mean "
          << mean << " max " << worst << ", rehashes " << h.store.rehashes() << ", "
          << std::setprecision(0) << static_cast<double>(h.store.memory_bytes()) / (1024.0 * 1024.0) << " MB";
    } else {
        o << "node-based; live " << h.store.size() << ", peak " << h.peak();
    }
    return o.str();
}

template <typename StoreT>
StoreVariant make_store_variant(std::string id, std::string what) {
    StoreVariant v;
    v.id = std::move(id);
    v.what = std::move(what);
    v.run = [](std::string& state) {
        auto h = std::make_unique<BasicBookHandler<StoreT, false>>();
        const Run r = run_chunked(*h); // timed part ends here; describe() is not timed
        state = describe(*h);
        return r;
    };
    v.profile = [] {
        if constexpr (requires(StoreT s) { s.displacement(); }) {
            auto p = std::make_unique<Profiler<StoreT>>();
            run_chunked(*p);
            return p->snaps;
        } else {
            return std::vector<Snapshot>{};
        }
    };
    return v;
}

int exp_store(int reps, std::ostream& out) {
    using enum Layout;
    using enum EmptyMark;
    using enum HashFn;
    using enum Erase;
    constexpr std::size_t k21 = std::size_t{1} << 21, k22 = k21 << 1, k23 = k21 << 2, k24 = k21 << 3;
    std::vector<StoreVariant> vs;
    vs.push_back(make_store_variant<MapStore>("unordered_map", "std::unordered_map, reserve(1M)"));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k21>>("identity 2^21", "current default: starts at 2^21, doubles at load 0.5"));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k22>>("identity 2^22", ""));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k23>>("identity 2^23", ""));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k24>>("identity 2^24", ""));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k21>>("fibonacci 2^21", "starts at 2^21, doubles at load 0.5"));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k22>>("fibonacci 2^22", ""));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k23>>("fibonacci 2^23", ""));
    vs.push_back(make_store_variant<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k24>>("fibonacci 2^24", ""));

    // the floor for the same session, so store cost = parse time - floor
    std::vector<Run> floors;
    for (int rep = 1; rep <= reps; ++rep) {
        DiscardHandler d;
        floors.push_back(run_chunked(d));
        g_sink = g_sink + d.sum;
        std::cout << "  rep " << rep << " floor " << std::fixed << std::setprecision(1) << floors.back().parse_ns_per_msg() << " ns/msg\n";
        for (auto& v : vs) {
            v.runs.push_back(v.run(v.final_state));
            std::cout << "  rep " << rep << ' ' << std::left << std::setw(16) << v.id << std::right << ' '
                      << v.runs.back().parse_ns_per_msg() << " ns/msg parse (" << v.runs.back().wall_ns_per_msg() << " wall)\n";
        }
    }
    auto median = [](std::vector<double> x) { std::sort(x.begin(), x.end()); return x[x.size() / 2]; };
    std::vector<double> fl;
    for (auto& f : floors) fl.push_back(f.parse_ns_per_msg());
    const double floor_med = median(fl);

    out << "## Order store at full scale (store only, no books)\n\n"
        << "Parse time excludes time blocked in read(). Store cost = parse ns/msg - floor (" << std::fixed << std::setprecision(1)
        << floor_med << " ns/msg, median of " << reps << "). " << reps << " interleaved reps; median shown, all runs in brackets.\n\n"
        << "| variant | parse ns/msg | store cost ns/msg | state after the run |\n|---|---|---|---|\n";
    for (auto& v : vs) {
        std::vector<double> p;
        for (auto& r : v.runs) p.push_back(r.parse_ns_per_msg());
        const double med = median(p);
        out << "| " << v.id << " | " << std::setprecision(1) << med << " (";
        for (std::size_t i = 0; i < p.size(); ++i) out << (i ? " / " : "") << p[i];
        out << ") | " << med - floor_med << " | " << v.final_state << " |\n";
    }

    // how load and probes evolve through the day, for the default size, both hashes
    for (std::size_t i : {std::size_t{1}, std::size_t{5}}) {
        const auto snaps = vs[i].profile();
        out << "\n### Through the day: " << vs[i].id << " (" << vs[i].what << ")\n\n"
            << "| messages | live orders | capacity | load | probe mean | probe max |\n|---|---|---|---|---|---|\n";
        for (const auto& s : snaps)
            out << "| " << s.msgs << " | " << s.live << " | " << s.capacity << " | " << std::setprecision(2) << s.load
                << " | " << s.mean_probe << " | " << s.max_probe << " |\n";
    }
    return 0;
}

// ---------------------------------------------------------------- one store variant
// One variant per process, so peak memory belongs to that variant alone. Snapshots of load
// and probe distance every 8M messages; the time spent taking them is measured and
// subtracted, so the reported ns/msg is the store's own work.
template <typename StoreT>
struct TimedProfiler {
    BasicBookHandler<StoreT, false> h;
    std::uint64_t n = 0, every = 8'000'000, snap_ns = 0;
    std::vector<Snapshot> snaps;
    template <typename M>
    void on(const M& m) {
        h.on(m);
        if (++n % every == 0) {
            const auto t = clk::now();
            const auto [mean, worst] = h.store.displacement();
            snaps.push_back(Snapshot{n, h.store.size(), h.store.capacity(),
                                     static_cast<double>(h.store.size()) / static_cast<double>(h.store.capacity()), mean, worst});
            snap_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t).count());
        }
    }
};

template <typename StoreT>
int run_one(const std::string& id, std::ostream& out) {
    auto p = std::make_unique<TimedProfiler<StoreT>>();
    const Run r = run_chunked(*p);
    const std::uint64_t work_ns = r.io.parse_ns - p->snap_ns;
    const double ns_msg = static_cast<double>(work_ns) / static_cast<double>(r.msgs);
    const Snapshot* peak = nullptr;
    std::size_t worst = 0;
    for (const auto& s : p->snaps) {
        if (!peak || s.live > peak->live) peak = &s;
        worst = std::max(worst, s.max_probe);
    }
    const ProcessMemory mem = process_memory();
    out << std::fixed << "## " << id << "\n\n"
        << "| ns/msg (store only, parse time, snapshots excluded) | " << std::setprecision(1) << ns_msg << " |\n|---|---|\n"
        << "| book messages | " << r.msgs << " |\n"
        << "| time in read() / parsing / snapshots | " << ms(r.io.read_ns) / 1000.0 << " s / " << ms(r.io.parse_ns) / 1000.0
        << " s / " << ms(p->snap_ns) / 1000.0 << " s |\n";
    if (peak)
        out << "| at peak live (" << peak->live << " orders, message " << peak->msgs << ") | capacity " << peak->capacity
            << ", load " << std::setprecision(2) << peak->load << ", probe mean " << peak->mean_probe << ", max " << peak->max_probe << " |\n";
    out << "| worst probe distance at any snapshot | " << worst << " |\n"
        << "| final | capacity " << p->h.store.capacity() << ", rehashes " << p->h.store.rehashes() << ", "
        << std::setprecision(0) << static_cast<double>(p->h.store.memory_bytes()) / (1024.0 * 1024.0) << " MB table |\n"
        << "| peak private / working set | " << mem.peak_private / (1024 * 1024) << " MB / " << mem.peak_working_set / (1024 * 1024) << " MB |\n\n"
        << "Snapshots (every " << p->every << " messages):\n\n| messages | live | capacity | load | probe mean | probe max |\n|---|---|---|---|---|---|\n";
    for (const auto& s : p->snaps)
        out << "| " << s.msgs << " | " << s.live << " | " << s.capacity << " | " << std::setprecision(2) << s.load << " | "
            << s.mean_probe << " | " << s.max_probe << " |\n";
    // one machine-readable line for the summary table
    std::cout << std::fixed << "RESULT " << id << ' ' << std::setprecision(1) << ns_msg << ' ' << std::setprecision(2)
              << (peak ? peak->mean_probe : 0.0) << ' ' << worst << ' ' << mem.peak_private / (1024 * 1024) << ' '
              << mem.peak_working_set / (1024 * 1024) << ' ' << (peak ? peak->load : 0.0)
              << " final_capacity=" << p->h.store.capacity() << " rehashes=" << p->h.store.rehashes() << '\n';
    return 0;
}

// Maps a variant name to a store type and calls f.template operator()<StoreT>().
// "...fixed" and the 2^25 variants never grow, so their size is the size measured.
template <typename F>
int with_variant(const std::string& id, F&& f) {
    using enum Layout;
    using enum EmptyMark;
    using enum HashFn;
    using enum Erase;
    constexpr std::size_t k21 = std::size_t{1} << 21, k22 = k21 << 1, k23 = k21 << 2, k24 = k21 << 3, k25 = k21 << 4;
    if (id == "identity21fixed") return f.template operator()<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k21, false>>();
    if (id == "fibonacci21fixed") return f.template operator()<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k21, false>>();
    if (id == "identity23fixed") return f.template operator()<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k23, false>>();
    if (id == "fibonacci23fixed") return f.template operator()<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k23, false>>();
    if (id == "identity24fixed") return f.template operator()<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k24, false>>();
    if (id == "fibonacci24fixed") return f.template operator()<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k24, false>>();
    if (id == "identity25") return f.template operator()<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k25, false>>();
    if (id == "fibonacci25") return f.template operator()<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k25, false>>();
    (void)k22;
    return -1;
}

int exp_store1(const std::string& id, std::ostream& out) {
    const int rc = with_variant(id, [&]<typename S>() { return run_one<S>(id, out); });
    if (rc != -1) return rc;
    using enum Layout;
    using enum EmptyMark;
    using enum HashFn;
    using enum Erase;
    constexpr std::size_t k21 = std::size_t{1} << 21, k22 = k21 << 1, k23 = k21 << 2, k24 = k21 << 3;
    if (id == "identity21") return run_one<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k21>>(id, out);
    if (id == "identity22") return run_one<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k22>>(id, out);
    if (id == "identity23") return run_one<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k23>>(id, out);
    if (id == "identity24") return run_one<FlatStore<AoS, SentinelKey, Identity, BackwardShift, k24>>(id, out);
    if (id == "fibonacci21") return run_one<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k21>>(id, out);
    if (id == "fibonacci22") return run_one<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k22>>(id, out);
    if (id == "fibonacci23") return run_one<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k23>>(id, out);
    if (id == "fibonacci24") return run_one<FlatStore<AoS, SentinelKey, Fibonacci, BackwardShift, k24>>(id, out);
    std::cout << "unknown variant " << id << " (identity21..24, fibonacci21..24)\n";
    return 1;
}

// ---------------------------------------------------------------- latency per variant
// Per-message latency (decode + store + books) on the full file, same method as M8: fenced
// TSC, log-linear histogram, thread pinned to one CPU. Chunked reading; the read() calls
// happen outside the timed dispatch.
struct LatProbe {
    Histogram* book;
    std::uint64_t t0 = 0;
    void before() { t0 = tsc_begin(); }
    void after(unsigned char type, const unsigned char*) {
        const std::uint64_t ticks = tsc_end() - t0;
        if (type == 'A' || type == 'F' || type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U')
            book->record(ticks);
    }
};

template <typename StoreT>
int run_latency(const std::string& id, int runs, std::ostream& out) {
#ifdef _WIN32
    const bool pinned = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << 2) != 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
    const bool pinned = false;
#endif
    const double tpn = tsc_per_ns();
    std::vector<std::uint64_t> ov;
    for (int i = 0; i < 1'000'000; ++i) {
        const auto a = tsc_begin();
        const auto b = tsc_end();
        ov.push_back(b - a);
    }
    std::sort(ov.begin(), ov.end());
    const double overhead_ns = static_cast<double>(ov[ov.size() / 2]) / tpn;
    auto ns = [&](std::uint64_t t) { return static_cast<double>(t) / tpn; };

    out << "## Per-message latency: " << id << "\n\n"
        << "Book messages (A F E C X D U), decode + order store + books, fenced TSC (" << std::fixed << std::setprecision(4) << tpn
        << " ticks/ns), timer cost " << std::setprecision(1) << overhead_ns << " ns (included below; subtract for the work alone), "
        << (pinned ? "pinned to CPU 2" : "not pinned") << ". Values are histogram bucket upper bounds (<= 6.25% high).\n\n"
        << "| run | messages | p50 | p90 | p99 | p99.9 | p99.99 | max | mean | wall ns/msg (probed) | state hash |\n"
        << "|---|---|---|---|---|---|---|---|---|---|---|\n";
    Histogram all;
    for (int r = 1; r <= runs; ++r) {
        Histogram h;
        auto handler = std::make_unique<BasicBookHandler<StoreT, true>>();
        std::string err;
        IoStats io;
        const auto t0 = clk::now();
        const ParseResult pr = parse_chunked(g_path, *handler, 0, err, std::size_t{64} << 20, LatProbe{&h}, &io);
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
        if (!pr.ok) { std::cout << "parse error " << err << '\n'; return 1; }
        const auto hash = handler->state_hash().combined;
        all.merge(h);
        out << "| " << r << " | " << h.count() << std::setprecision(0) << " | " << ns(h.percentile(.5)) << " | "
            << ns(h.percentile(.9)) << " | " << ns(h.percentile(.99)) << " | " << ns(h.percentile(.999)) << " | "
            << ns(h.percentile(.9999)) << " | " << ns(h.max()) << " | " << std::setprecision(1) << h.mean() / tpn << " | "
            << static_cast<double>(wall - static_cast<long long>(io.read_ns)) / static_cast<double>(pr.processed)
            << " | " << std::hex << hash << std::dec << " |\n";
        std::cout << std::fixed << std::setprecision(0) << "LAT " << id << " run " << r << " p50 " << ns(h.percentile(.5))
                  << " p99 " << ns(h.percentile(.99)) << " p99.9 " << ns(h.percentile(.999)) << " p99.99 " << ns(h.percentile(.9999))
                  << " max " << ns(h.max()) << " mean " << std::setprecision(1) << h.mean() / tpn << " hash " << std::hex << hash
                  << std::dec << '\n';
    }
    out << "| **merged** | " << all.count() << std::setprecision(0) << " | " << ns(all.percentile(.5)) << " | "
        << ns(all.percentile(.9)) << " | " << ns(all.percentile(.99)) << " | " << ns(all.percentile(.999)) << " | "
        << ns(all.percentile(.9999)) << " | " << ns(all.max()) << " | " << std::setprecision(1) << all.mean() / tpn << " | | |\n";
    std::cout << std::fixed << std::setprecision(1) << "LATMERGED " << id << " p50 " << ns(all.percentile(.5)) << " p90 "
              << ns(all.percentile(.9)) << " p99 " << ns(all.percentile(.99)) << " p99.9 " << ns(all.percentile(.999))
              << " p99.99 " << ns(all.percentile(.9999)) << " max " << ns(all.max()) << " mean " << all.mean() / tpn
              << " overhead " << overhead_ns << '\n';
    return 0;
}

// ---------------------------------------------------------------- books
// Wraps the real handler: records, for every book update, how far from the best price it
// lands (rank 0 = best level) and how deep that book side is. Analysis only, not timed.
struct BookProfiler {
    BookHandler h;
    static constexpr std::uint64_t kOpen = 34'200'000'000'000ULL; // 09:30:00
    Histogram rank[2], depth[2];    // [0] before 09:30, [1] from 09:30
    std::uint64_t within[2][4] = {}; // rank < 1, < 3, < 10, < 16 counts
    std::uint64_t updates[2] = {};
    std::uint64_t ts = 0;

    static std::size_t rank_of(const std::vector<PriceLevel>& lv, Side side, std::uint32_t price) {
        const std::size_t i = Book::scan(lv, side, price);
        return (i > 0 && lv[i - 1].price == price) ? lv.size() - i : lv.size(); // not found: past the end
    }
    void record(std::uint16_t locate, Side side, std::uint32_t price) {
        const auto& lv = h.books[locate].levels(side);
        const std::size_t r = rank_of(lv, side, price);
        const int ph = ts >= kOpen ? 1 : 0;
        rank[ph].record(r);
        depth[ph].record(lv.size());
        ++updates[ph];
        const std::size_t lim[4] = {1, 3, 10, 16};
        for (int k = 0; k < 4; ++k) within[ph][k] += r < lim[k];
    }
    void before(std::uint64_t ref) {
        if (const Order* o = h.store.find(ref)) record(o->locate, o->side, o->price);
    }

    void on(const StockDirectoryMessage& m) { h.on(m); }
    void on(const AddOrderMessage& m) { ts = m.timeStamp; h.on(m); record(m.stockLocate, m.side, m.price); }
    void on(const AddOrderMPIDMessage& m) { ts = m.timeStamp; h.on(m); record(m.stockLocate, m.side, m.price); }
    void on(const OrderExecutedMessage& m) { ts = m.timeStamp; before(m.orderRef); h.on(m); }
    void on(const OrderExecutedWithPriceMessage& m) { ts = m.timeStamp; before(m.orderRef); h.on(m); }
    void on(const OrderCancelMessage& m) { ts = m.timeStamp; before(m.orderRef); h.on(m); }
    void on(const OrderDeleteMessage& m) { ts = m.timeStamp; before(m.orderRef); h.on(m); }
    void on(const OrderReplaceMessage& m) {
        ts = m.timeStamp;
        before(m.origRef);
        h.on(m);
        if (const Order* o = h.store.find(m.newRef)) record(o->locate, o->side, o->price);
    }
    void on(const TradeMessage& m) { h.on(m); }
    void on(const CrossTradeMessage& m) { h.on(m); }
};

int exp_books(std::ostream& out) {
    auto p = std::make_unique<BookProfiler>();
    run_chunked(*p);
    auto pct = [](const Histogram& x, double q) { return x.percentile(q); };
    out << "## Book shape at full scale\n\n"
        << "Rank = distance of the updated level from the best price (0 = best), measured on every add (after it) and "
           "every execute/cancel/delete/replace (before it). Depth = levels on that side at the time.\n\n"
        << "| phase | updates | rank p50 | p90 | p99 | p99.9 | max | share rank 0 | rank < 3 | rank < 10 | rank < 16 | "
           "depth p50 | p90 | p99 | max |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    const char* names[2] = {"before 09:30", "from 09:30"};
    for (int ph = 0; ph < 2; ++ph) {
        const double u = static_cast<double>(p->updates[ph]);
        out << "| " << names[ph] << " | " << p->updates[ph] << " | " << pct(p->rank[ph], .5) << " | " << pct(p->rank[ph], .9)
            << " | " << pct(p->rank[ph], .99) << " | " << pct(p->rank[ph], .999) << " | " << p->rank[ph].max() << std::fixed
            << std::setprecision(1);
        for (int k = 0; k < 4; ++k) out << " | " << (u > 0 ? 100.0 * static_cast<double>(p->within[ph][k]) / u : 0.0) << "%";
        out << " | " << pct(p->depth[ph], .5) << " | " << pct(p->depth[ph], .9) << " | " << pct(p->depth[ph], .99) << " | "
            << p->depth[ph].max() << " |\n";
    }

    // depth of every non-empty book side at the end of the file
    Histogram end_depth;
    std::size_t over16 = 0, sides = 0;
    for (const Book& b : p->h.books)
        for (const auto* lv : {&b.bids, &b.asks})
            if (!lv->empty()) {
                end_depth.record(lv->size());
                ++sides;
                over16 += lv->size() > 16;
            }
    out << "\nNon-empty book sides at end of file: " << sides << "; depth p50 " << end_depth.percentile(.5) << ", p90 "
        << end_depth.percentile(.9) << ", p99 " << end_depth.percentile(.99) << ", max " << end_depth.max() << "; deeper than 16: "
        << over16 << " (" << std::setprecision(1) << 100.0 * static_cast<double>(over16) / static_cast<double>(sides) << "%)\n"
        << "\n(Histogram values are bucket upper bounds, exact below 16.)\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "usage: " << argv[0] << " <file> io|store|books [reps] [outdir=results/scale]\n"
                  << "       " << argv[0] << " <file> store1 <identity21..24|fibonacci21..24> [outdir]\n"
                  << "       " << argv[0] << " <file> floor [-] [outdir]\n";
        return 1;
    }
    g_path = argv[1];
    const std::string exp = argv[2];
    const bool one = exp == "store1" || exp == "latency1";
    const int reps = !one && argc > 3 && std::string(argv[3]) != "-" ? std::stoi(argv[3]) : (exp == "store" ? 2 : 3);
    const std::filesystem::path outdir = argc > 4 ? argv[4] : "results/scale";
    std::filesystem::create_directories(outdir);
    std::ostringstream out;
    out << "# scale_bench " << exp << (one ? " " + std::string(argv[3]) : "") << "\n\nfile: `" << g_path.string() << "` ("
        << std::filesystem::file_size(g_path) << " bytes)\n\n";
    int rc = 1;
    std::string name = exp;
    if (exp == "io") rc = exp_io(reps, out);
    else if (exp == "store") rc = exp_store(reps, out);
    else if (exp == "books") rc = exp_books(out);
    else if (exp == "store1" && argc > 3) { rc = exp_store1(argv[3], out); name = "store1_" + std::string(argv[3]); }
    else if (exp == "latency1" && argc > 3) {
        const std::string id = argv[3];
        const int lruns = argc > 5 ? std::stoi(argv[5]) : 2;
        rc = with_variant(id, [&]<typename S>() { return run_latency<S>(id, lruns, out); });
        if (rc == -1) { std::cout << "unknown variant " << id << '\n'; return 1; }
        name = "latency1_" + id;
    }
    else if (exp == "floor") {
        DiscardHandler d;
        const Run r = run_chunked(d);
        g_sink = g_sink + d.sum;
        out << kRunHeader << run_row("floor (parse + decode, discard)", r);
        std::cout << "RESULT floor " << std::fixed << std::setprecision(1) << r.parse_ns_per_msg() << '\n';
        rc = 0;
    }
    else { std::cout << "unknown experiment " << exp << '\n'; return 1; }
    const ProcessMemory mem = process_memory();
    out << "\npeak private memory " << mem.peak_private / (1024 * 1024) << " MB, peak working set "
        << mem.peak_working_set / (1024 * 1024) << " MB\n";
    std::ofstream(outdir / (name + ".md")) << out.str();
    std::cout << "wrote " << (outdir / (name + ".md")).string() << '\n';
    return rc;
}
