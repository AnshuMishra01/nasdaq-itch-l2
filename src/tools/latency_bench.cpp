// M8: per-message latency distribution for the book builder.
//
// Times every dispatch (decode + order store + book update) with the TSC, records it in
// a histogram per message type, and keeps the slowest messages of each run so the tail
// can be explained: are the same messages slow every run (our code), or random ones
// (the OS / power management)? Do the slow ones create or delete price levels?
//
// usage: latency_bench <file> [runs=3] [outdir=results/m8] [clock=fenced|plain] [reserve=default 16]

#include "parse.h"
#include "latency.h"
#include "handlers/book_handler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using namespace itch;

constexpr std::size_t kTopK = 4000; // > 0.1% of the 3.2M book messages
const std::string kTypes = "AFECXDURPQ"; // timed separately; everything else is "other"
constexpr std::size_t kOther = 10;

struct Slow {
    std::uint64_t ticks;
    std::uint64_t index;   // message number in the file (all types, from 0)
    std::uint64_t ts;      // ITCH timestamp, ns since midnight
    char type;
    bool level_change;
    bool operator>(const Slow& o) const { return ticks > o.ticks; }
};

struct RunData {
    std::array<Histogram, 11> per_type;
    Histogram book;           // A F E C X D U: the messages the ns/msg figures count
    Histogram book_changed;   // ... that created or erased a price level
    Histogram book_unchanged; // ... that did not
    std::array<Histogram, 11> type_changed;   // per type, created/erased a level
    std::array<Histogram, 11> type_unchanged; // per type, no level change
    std::priority_queue<Slow, std::vector<Slow>, std::greater<>> top; // min-heap of the slowest
    std::uint64_t index = 0;
    std::uint64_t t0 = 0;
    std::uint64_t lc_before = 0;
    const std::uint64_t* level_changes = nullptr;
    double wall_ns_per_msg = 0;
    std::uint64_t book_msgs = 0;
};

std::array<std::size_t, 256> make_slots() {
    std::array<std::size_t, 256> s{};
    s.fill(kOther);
    for (std::size_t i = 0; i < kTypes.size(); ++i) s[static_cast<unsigned char>(kTypes[i])] = i;
    return s;
}
const std::array<std::size_t, 256> kSlot = make_slots();

bool is_book(unsigned char t) { return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U'; }

bool g_plain = false; // clock choice: plain rdtsc instead of the fenced pair
inline std::uint64_t clock_begin() { return g_plain ? tsc_plain() : tsc_begin(); }
inline std::uint64_t clock_end() { return g_plain ? tsc_plain() : tsc_end(); }

struct LatencyProbe {
    RunData* d;
    void before() {
        d->lc_before = *d->level_changes;
        d->t0 = clock_begin();
    }
    void after(unsigned char type, const unsigned char* msg) {
        const std::uint64_t ticks = clock_end() - d->t0;
        const bool changed = *d->level_changes != d->lc_before;
        d->per_type[kSlot[type]].record(ticks);
        if (is_book(type)) {
            d->book.record(ticks);
            (changed ? d->book_changed : d->book_unchanged).record(ticks);
            (changed ? d->type_changed : d->type_unchanged)[kSlot[type]].record(ticks);
            if (d->top.size() < kTopK || ticks > d->top.top().ticks) {
                std::uint64_t ts = 0;
                for (int i = 5; i < 11; ++i) ts = (ts << 8) | msg[i];
                d->top.push(Slow{ticks, d->index, ts, static_cast<char>(type), changed});
                if (d->top.size() > kTopK) d->top.pop();
            }
        }
        ++d->index;
    }
};

std::vector<unsigned char> g_buf;
double g_tpn = 1.0; // TSC ticks per ns

double ns(std::uint64_t ticks) { return static_cast<double>(ticks) / g_tpn; }

struct Pct {
    double p50, p90, p99, p999, p9999, max, mean;
    std::uint64_t n;
};

Pct pct(const Histogram& h) {
    return Pct{ns(h.percentile(0.50)), ns(h.percentile(0.90)), ns(h.percentile(0.99)),
               ns(h.percentile(0.999)), ns(h.percentile(0.9999)), ns(h.max()), h.mean() / g_tpn, h.count()};
}

const char* kHeader = "| | n | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |\n|---|---|---|---|---|---|---|---|---|\n";

std::string row(const std::string& label, const Pct& p) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(0) << "| " << label << " | " << p.n << " | " << p.p50 << " | " << p.p90
      << " | " << p.p99 << " | " << p.p999 << " | " << p.p9999 << " | " << p.max << " | " << std::setprecision(1)
      << p.mean << " |\n";
    return o.str();
}

std::string fmt_tod(std::uint64_t ts) {
    const std::uint64_t s = ts / 1'000'000'000ULL;
    std::ostringstream o;
    o << std::setfill('0') << std::setw(2) << s / 3600 << ':' << std::setw(2) << s / 60 % 60 << ':' << std::setw(2)
      << s % 60 << '.' << std::setw(3) << ts % 1'000'000'000ULL / 1'000'000ULL;
    return o.str();
}

std::vector<Slow> sorted_top(RunData& d) {
    std::vector<Slow> v;
    auto q = d.top;
    while (!q.empty()) { v.push_back(q.top()); q.pop(); }
    std::sort(v.begin(), v.end(), [](const Slow& a, const Slow& b) { return a.ticks > b.ticks; });
    return v;
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
        std::cout << "usage: " << argv[0] << " <file> [runs=3] [outdir=results/m8]\n";
        return 1;
    }
    const std::string path = argv[1];
    const int runs = argc > 2 ? std::stoi(argv[2]) : 3;
    const std::filesystem::path outdir = argc > 3 ? argv[3] : "results/m8";
    g_plain = argc > 4 && std::string(argv[4]) == "plain";
    if (argc > 5) Book::reserve_on_first_use = std::stoul(argv[5]); // otherwise the program default
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) { std::cout << "cannot open " << path << '\n'; return 1; }
        g_buf.resize(static_cast<std::size_t>(std::filesystem::file_size(path)));
        in.read(reinterpret_cast<char*>(g_buf.data()), static_cast<std::streamsize>(g_buf.size()));
    }

    // Pin to one core and raise priority: fewer migrations and preemptions. Not a
    // guarantee; the tail analysis below checks what got through anyway.
    std::string pinning = "not pinned";
#ifdef _WIN32
    const bool pinned = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << 2) != 0;
    const bool prio = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
    pinning = std::string(pinned ? "pinned to logical CPU 2" : "pinning FAILED") +
              (prio ? ", THREAD_PRIORITY_HIGHEST" : ", priority unchanged");
#endif

    // Clock calibration: median of 3 x 200 ms.
    std::vector<double> cal;
    for (int i = 0; i < 3; ++i) cal.push_back(tsc_per_ns());
    std::sort(cal.begin(), cal.end());
    g_tpn = cal[1];

    // Timer overhead: an empty begin/end pair, 1M times. Included in every measurement below.
    Histogram overhead;
    std::vector<std::uint64_t> overhead_raw;
    overhead_raw.reserve(1'000'000);
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t a = clock_begin();
        const std::uint64_t b = clock_end();
        overhead.record(b - a);
        overhead_raw.push_back(b - a);
    }
    std::sort(overhead_raw.begin(), overhead_raw.end());
    const std::uint64_t overhead_med = overhead_raw[overhead_raw.size() / 2]; // exact, not a bucket bound

    // Clock resolution: the smallest step the counter shows (spin until it changes).
    std::uint64_t resolution = ~std::uint64_t{0};
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t a = __rdtsc();
        std::uint64_t b;
        do { b = __rdtsc(); } while (b == a);
        resolution = std::min(resolution, b - a);
    }

    // Throughput without the probe, for comparison (same handler, same file).
    std::vector<double> unprobed;
    for (int r = 0; r < runs; ++r) {
        auto h = std::make_unique<BookHandler>();
        const auto t0 = std::chrono::steady_clock::now();
        const ParseResult pr = parse_buffer(g_buf.data(), g_buf.size(), *h);
        const auto t1 = std::chrono::steady_clock::now();
        unprobed.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
                           static_cast<double>(pr.processed));
    }

    // Probed runs.
    std::size_t book_bytes = 0, sides_touched = 0;
    std::vector<std::unique_ptr<RunData>> data;
    for (int r = 0; r < runs; ++r) {
        auto d = std::make_unique<RunData>();
        auto h = std::make_unique<BookHandler>();
        d->level_changes = &h->level_changes;
        const auto t0 = std::chrono::steady_clock::now();
        const ParseResult pr = parse_buffer(g_buf.data(), g_buf.size(), *h, 0, LatencyProbe{d.get()});
        const auto t1 = std::chrono::steady_clock::now();
        if (!pr.ok) { std::cout << "parse error: " << pr.error << '\n'; return 1; }
        d->book_msgs = pr.processed;
        d->wall_ns_per_msg = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
                             static_cast<double>(pr.processed);
        if (h->state_hash().combined != 0x96fd73902096c354ULL) { std::cout << "WRONG STATE HASH\n"; return 1; }
        // memory held by the book vectors at end of run (capacity, not size)
        book_bytes = 0;
        std::size_t touched = 0;
        for (const Book& b : h->books) {
            book_bytes += (b.bids.capacity() + b.asks.capacity()) * sizeof(PriceLevel);
            touched += static_cast<std::size_t>(b.bids.capacity() != 0) + static_cast<std::size_t>(b.asks.capacity() != 0);
        }
        sides_touched = touched;
        std::cout << "run " << r + 1 << ": p50 " << std::fixed << std::setprecision(0) << ns(d->book.percentile(0.5))
                  << " ns, p99.99 " << ns(d->book.percentile(0.9999)) << " ns, max " << ns(d->book.max()) << " ns\n";
        data.push_back(std::move(d));
    }

    std::filesystem::create_directories(outdir);
    std::vector<std::vector<Slow>> tops;
    for (auto& d : data) tops.push_back(sorted_top(*d));

    // ---- per-run files ----
    for (int r = 0; r < runs; ++r) {
        RunData& d = *data[static_cast<std::size_t>(r)];
        std::ofstream f(outdir / ("run" + std::to_string(r + 1) + ".md"));
        f << "# M8 latency, run " << r + 1 << "\n\nAll values in ns (TSC ticks / " << std::setprecision(4) << g_tpn
          << "), upper bound of a histogram bucket (<= 6.25% wide).\n\n## By message type\n\n" << kHeader;
        f << row("**book (A F E C X D U)**", pct(d.book));
        for (std::size_t i = 0; i < kTypes.size(); ++i)
            if (d.per_type[i].count()) f << row(std::string(1, kTypes[i]), pct(d.per_type[i]));
        if (d.per_type[kOther].count()) f << row("other (S H ...)", pct(d.per_type[kOther]));
        f << "\n## Book messages split by whether a price level was created or erased\n\n" << kHeader
          << row("level created/erased", pct(d.book_changed)) << row("no level change", pct(d.book_unchanged));
        f << "\n## By message type, split by level change\n\n" << kHeader;
        for (char c : std::string("AFECXDU")) {
            const std::size_t i = kSlot[static_cast<unsigned char>(c)];
            if (d.type_changed[i].count()) f << row(std::string(1, c) + ", level created/erased", pct(d.type_changed[i]));
            if (d.type_unchanged[i].count()) f << row(std::string(1, c) + ", no level change", pct(d.type_unchanged[i]));
        }
        f << "\n## 30 slowest book messages\n\n| rank | ns | type | msg # | time of day | level change |\n|---|---|---|---|---|---|\n";
        const auto& t = tops[static_cast<std::size_t>(r)];
        for (std::size_t i = 0; i < 30 && i < t.size(); ++i)
            f << "| " << i + 1 << " | " << std::setprecision(0) << std::fixed << ns(t[i].ticks) << " | " << t[i].type
              << " | " << t[i].index << " | " << fmt_tod(t[i].ts) << " | " << (t[i].level_change ? "yes" : "no") << " |\n";
    }

    // ---- summary ----
    std::ostringstream s;
    s << std::fixed;
    s << "# M8 latency distribution\n\n"
      << "Generated by `latency_bench` on " << now_string() << ". Interpretation is in `ANALYSIS.md`.\n\n"
      << "## Method\n\n"
      << "- file: `" << path << "`; every message timed, nothing excluded (cold start included)\n"
      << "- timed span: one `dispatch()` = decode + order store + book update (framing loop excluded)\n"
      << (g_plain ? "- clock: TSC via plain `rdtsc` at both ends (no fences: reads may reorder with the timed work, blurring single samples); invariant TSC: "
                  : "- clock: TSC via `lfence; rdtsc` ... `rdtscp; lfence`; invariant TSC: ")
      << (invariant_tsc() ? "yes" : "NO") << "\n"
      << "- calibration: " << std::setprecision(4) << g_tpn << " ticks/ns (3 x 200 ms vs steady_clock: "
      << cal[0] << ", " << cal[1] << ", " << cal[2] << ")\n"
      << "- timer overhead (empty begin/end pair, 1M samples): p50 " << std::setprecision(1)
      << ns(overhead.percentile(0.5)) << " ns, p99 " << ns(overhead.percentile(0.99)) << " ns (bucket bounds); exact median "
      << overhead_med << " ticks = " << ns(overhead_med) << " ns; included in every raw number\n"
      << "- clock resolution: smallest visible TSC step " << resolution << " ticks = " << ns(resolution)
      << " ns; differences below that are not resolvable\n"
      << "- thread: " << pinning << "\n"
      << "- runs: " << runs << ", each with a fresh handler; state hash checked after each run\n"
      << "- histogram: log-linear, 16 sub-buckets per power of two; values are bucket upper bounds (<= 6.25% high)\n"
      << "- build: g++ " << __VERSION__
#ifdef __OPTIMIZE__
      << ", optimised"
#endif
#ifdef NDEBUG
      << ", NDEBUG"
#endif
      << "\n\n";

    s << "## Throughput, and what the probe costs\n\n| | ns/msg (wall clock, whole file) |\n|---|---|\n";
    for (int r = 0; r < runs; ++r)
        s << "| without probe, run " << r + 1 << " | " << std::setprecision(1) << unprobed[static_cast<std::size_t>(r)] << " |\n";
    for (int r = 0; r < runs; ++r)
        s << "| with probe, run " << r + 1 << " | " << data[static_cast<std::size_t>(r)]->wall_ns_per_msg << " |\n";

    s << "\n## Book messages (A F E C X D U), ns per message\n\n" << kHeader;
    for (int r = 0; r < runs; ++r) s << row("run " + std::to_string(r + 1), pct(data[static_cast<std::size_t>(r)]->book));

    Histogram all_book, all_changed, all_unchanged;
    std::array<Histogram, 11> all_types, all_type_changed, all_type_unchanged;
    for (auto& d : data) {
        all_book.merge(d->book);
        all_changed.merge(d->book_changed);
        all_unchanged.merge(d->book_unchanged);
        for (std::size_t i = 0; i < all_types.size(); ++i) {
            all_types[i].merge(d->per_type[i]);
            all_type_changed[i].merge(d->type_changed[i]);
            all_type_unchanged[i].merge(d->type_unchanged[i]);
        }
    }
    s << row("**all runs merged**", pct(all_book));

    // Same, with the timer's own median cost taken off every percentile. An estimate of
    // the work itself: the timer cost varies by +/- one clock step, so +/- ~10 ns.
    auto net = [&](Pct p) {
        const double o = ns(overhead_med);
        for (double* v : {&p.p50, &p.p90, &p.p99, &p.p999, &p.p9999, &p.max, &p.mean}) *v = std::max(0.0, *v - o);
        return p;
    };
    s << "\n## Book messages, timer overhead (" << std::setprecision(0) << ns(overhead_med)
      << " ns) subtracted: estimate of the work itself\n\n" << kHeader;
    for (int r = 0; r < runs; ++r) s << row("run " + std::to_string(r + 1), net(pct(data[static_cast<std::size_t>(r)]->book)));
    s << row("**all runs merged**", net(pct(all_book)));

    s << "\n## By message type (all runs merged)\n\n" << kHeader;
    for (std::size_t i = 0; i < kTypes.size(); ++i)
        if (all_types[i].count()) s << row(std::string(1, kTypes[i]), pct(all_types[i]));
    if (all_types[kOther].count()) s << row("other (S H ...)", pct(all_types[kOther]));

    s << "\n## Level created/erased vs not (all runs merged)\n\n" << kHeader
      << row("level created/erased", pct(all_changed)) << row("no level change", pct(all_unchanged));

    s << "\n## By message type, split by level change (all runs merged)\n\n" << kHeader;
    for (char c : std::string("AFECXDU")) {
        const std::size_t i = kSlot[static_cast<unsigned char>(c)];
        if (all_type_changed[i].count()) s << row(std::string(1, c) + ", level created/erased", pct(all_type_changed[i]));
        if (all_type_unchanged[i].count()) s << row(std::string(1, c) + ", no level change", pct(all_type_unchanged[i]));
    }

    // Tail analysis
    const std::size_t tail_n = static_cast<std::size_t>(static_cast<double>(data[0]->book_msgs) * 0.001);
    s << "\n## The tail: slowest 0.1% of book messages (" << tail_n << " per run)\n\n"
      << "| run | share with a level change: tail | share: all book msgs | tail types (A F E C X D U) |\n|---|---|---|---|\n";
    for (int r = 0; r < runs; ++r) {
        const auto& t = tops[static_cast<std::size_t>(r)];
        std::size_t changed = 0;
        std::map<char, std::size_t> types;
        for (std::size_t i = 0; i < tail_n && i < t.size(); ++i) {
            changed += t[i].level_change;
            types[t[i].type]++;
        }
        const RunData& d = *data[static_cast<std::size_t>(r)];
        s << "| " << r + 1 << " | " << std::setprecision(1) << 100.0 * static_cast<double>(changed) / static_cast<double>(tail_n)
          << "% | " << 100.0 * static_cast<double>(d.book_changed.count()) / static_cast<double>(d.book.count()) << "% | ";
        for (char c : std::string("AFECXDU")) s << c << ' ' << types[c] << "  ";
        s << "|\n";
    }

    s << "\n## Same messages slow every run? (overlap of the slowest-N sets)\n\n| N | run 1 vs 2 | run 1 vs 3 | run 2 vs 3 | in all runs |\n|---|---|---|---|---|\n";
    for (std::size_t n : {std::size_t{10}, std::size_t{100}, std::size_t{1000}, tail_n}) {
        std::vector<std::set<std::uint64_t>> sets;
        for (const auto& t : tops) {
            std::set<std::uint64_t> st;
            for (std::size_t i = 0; i < n && i < t.size(); ++i) st.insert(t[i].index);
            sets.push_back(st);
        }
        auto overlap = [&](std::size_t a, std::size_t b) {
            if (a >= sets.size() || b >= sets.size()) return std::string("-");
            std::size_t k = 0;
            for (auto x : sets[a]) k += sets[b].count(x);
            return std::to_string(k) + " / " + std::to_string(n);
        };
        std::size_t all = 0;
        for (auto x : sets[0]) {
            bool in_all = true;
            for (std::size_t j = 1; j < sets.size(); ++j) in_all = in_all && sets[j].count(x);
            all += in_all;
        }
        s << "| " << n << " | " << overlap(0, 1) << " | " << overlap(0, 2) << " | " << overlap(1, 2) << " | " << all << " |\n";
    }

    // The reproducible tail: messages in the slowest 0.1% of EVERY run. By chance alone,
    // ~tail_n * 0.1% ^ (runs - 1) would be; far more means the message itself is slow.
    {
        std::map<std::uint64_t, std::vector<double>> times; // msg # -> ns in each run
        std::map<std::uint64_t, Slow> info;
        for (std::size_t r = 0; r < tops.size(); ++r)
            for (std::size_t i = 0; i < tail_n && i < tops[r].size(); ++i) {
                times[tops[r][i].index].push_back(ns(tops[r][i].ticks));
                info[tops[r][i].index] = tops[r][i];
            }
        std::vector<std::uint64_t> in_all;
        for (const auto& [idx, v] : times)
            if (v.size() == tops.size()) in_all.push_back(idx);
        std::size_t changed = 0;
        std::map<char, std::size_t> types;
        for (auto idx : in_all) {
            changed += info[idx].level_change;
            types[info[idx].type]++;
        }
        const double chance = static_cast<double>(tail_n) * std::pow(static_cast<double>(tail_n) / static_cast<double>(data[0]->book_msgs),
                                                                     static_cast<double>(tops.size() - 1));
        s << "\n## The reproducible tail: in the slowest 0.1% of every run\n\n"
          << "- " << in_all.size() << " messages (by chance alone: ~" << std::setprecision(3) << chance << ")\n"
          << "- with a level change: " << std::setprecision(1)
          << (in_all.empty() ? 0.0 : 100.0 * static_cast<double>(changed) / static_cast<double>(in_all.size())) << "%\n"
          << "- types:";
        for (char c : std::string("AFECXDU")) s << ' ' << c << ' ' << types[c];
        std::sort(in_all.begin(), in_all.end(), [&](std::uint64_t a, std::uint64_t b) {
            return *std::min_element(times[a].begin(), times[a].end()) > *std::min_element(times[b].begin(), times[b].end());
        });
        s << "\n\nSlowest 20 of them, by their fastest run:\n\n| msg # | type | time of day | level change | ns per run |\n|---|---|---|---|---|\n";
        for (std::size_t i = 0; i < 20 && i < in_all.size(); ++i) {
            const Slow& m = info[in_all[i]];
            s << "| " << m.index << " | " << m.type << " | " << fmt_tod(m.ts) << " | " << (m.level_change ? "yes" : "no") << " |";
            std::vector<double> per_run;
            for (const auto& t : tops)
                for (std::size_t k = 0; k < tail_n && k < t.size(); ++k)
                    if (t[k].index == m.index) per_run.push_back(ns(t[k].ticks));
            for (double x : per_run) s << ' ' << std::setprecision(0) << x;
            s << " |\n";
        }
    }

    // Where in the file are the slowest? Fraction of the tail in each 10% of the file.
    s << "\n## Where in the file the slowest 0.1% sit (by message number, 10 equal slices)\n\n| run |";
    for (int i = 0; i < 10; ++i) s << ' ' << i * 10 << "% |";
    s << "\n|---|";
    for (int i = 0; i < 10; ++i) s << "---|";
    s << '\n';
    for (int r = 0; r < runs; ++r) {
        std::array<std::size_t, 10> slice{};
        const auto& t = tops[static_cast<std::size_t>(r)];
        const std::uint64_t total = data[static_cast<std::size_t>(r)]->index;
        for (std::size_t i = 0; i < tail_n && i < t.size(); ++i) slice[std::min<std::size_t>(9, static_cast<std::size_t>(t[i].index * 10 / total))]++;
        s << "| " << r + 1 << " |";
        for (auto c : slice) s << ' ' << c << " |";
        s << '\n';
    }

    // One-line key metrics, for experiments that sweep a setting across processes.
    std::uint64_t peak_ws = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) peak_ws = pmc.PeakWorkingSetSize;
#endif
    Histogram adds_changed; // A and F that created a level, all runs
    for (auto& d : data) {
        adds_changed.merge(d->type_changed[kSlot['A']]);
        adds_changed.merge(d->type_changed[kSlot['F']]);
    }
    std::vector<double> sorted_unprobed = unprobed;
    std::sort(sorted_unprobed.begin(), sorted_unprobed.end());
    {
        const Pct a = pct(adds_changed);
        std::ofstream k(outdir / "key_metrics.md");
        k << std::fixed << std::setprecision(0) << "| " << Book::reserve_on_first_use << " | " << a.n << " | " << a.p50
          << " | " << a.p99 << " | " << a.p999 << " | " << a.p9999 << " | " << ns(all_book.percentile(0.5)) << " | "
          << std::setprecision(1) << static_cast<double>(peak_ws) / (1024.0 * 1024.0) << " | "
          << static_cast<double>(book_bytes) / (1024.0 * 1024.0) << " (" << sides_touched << " sides) | "
          << sorted_unprobed[sorted_unprobed.size() / 2] << " (";
        for (std::size_t i = 0; i < unprobed.size(); ++i) k << (i ? " / " : "") << unprobed[i];
        k << ") |\n";
    }

    std::ofstream(outdir / "latency.md") << s.str();
    std::cout << "wrote " << (outdir / "latency.md").string() << " and run1..run" << runs << ".md\n";
    return 0;
}
