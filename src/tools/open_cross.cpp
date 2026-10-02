// Opening-cross analysis: for every opening cross (Q, type 'O') with shares > 0, which
// same-stock messages (E, C printable N/Y, D) occur, and how far from that stock's Q in time.
// Answers: what removes the matched book orders at 09:30? usage: open_cross <full-day file>

#include "decode.h"
#include "itch_io.h"
#include <cstdio>
#include <map>
#include <vector>
#include <cstdint>
#include <cstdlib>
struct Ev { char type; std::uint64_t ts; std::uint64_t shares; char printable; };
struct Collect {
    std::vector<std::vector<Ev>> ev = std::vector<std::vector<Ev>>(65536);
    std::map<std::uint16_t, CrossTradeMessage> opens;
    static constexpr std::uint64_t lo = 34'198'000'000'000ULL, hi = 34'203'000'000'000ULL; // 09:29:58 .. 09:30:03
    bool in(std::uint64_t t) const { return t >= lo && t < hi; }
    void on(const CrossTradeMessage& m) { if (m.crossType == 'O') opens[m.stockLocate] = m; }
    void on(const OrderExecutedMessage& m) { if (in(m.timeStamp)) ev[m.stockLocate].push_back({'E', m.timeStamp, m.executedShares, 'Y'}); }
    void on(const OrderExecutedWithPriceMessage& m) { if (in(m.timeStamp)) ev[m.stockLocate].push_back({'C', m.timeStamp, m.executedShares, m.printable}); }
    void on(const OrderDeleteMessage& m) { if (in(m.timeStamp)) ev[m.stockLocate].push_back({'D', m.timeStamp, 0, 0}); }
    template <class M> void on(const M&) {}
};
int main(int, char** argv) {
    Collect c; std::string err;
    itch::parse_chunked(argv[1], c, 0, err);
    // deltas (event ts - Q ts) bucketed, per type

    const char* names[] = {"< -1s", "-1s..-1ms", "-1ms..-1us", "-1us..0", "0 (same ns)", "0..1us", "1us..1ms", "1ms..1s", "> 1s"};
    std::uint64_t cnt[4][9] = {}, sh[4][9] = {};
    auto typ = [](const Ev& e) { return e.type == 'E' ? 0 : e.type == 'C' ? (e.printable == 'N' ? 1 : 2) : 3; };
    std::uint64_t q_sh = 0, cn_total_sh = 0, crosses = 0;
    for (auto& [loc, q] : c.opens) {
        if (q.shares == 0) continue;
        ++crosses; q_sh += q.shares;
        for (const Ev& e : c.ev[loc]) {
            const long long d = static_cast<long long>(e.ts) - static_cast<long long>(q.timeStamp);


            int idx = d == 0 ? 4 : (d < 0 ? (d < -1000000000LL ? 0 : d < -1000000 ? 1 : d < -1000 ? 2 : 3) : (d <= 1000 ? 5 : d <= 1000000 ? 6 : d <= 1000000000LL ? 7 : 8));

            ++cnt[typ(e)][idx]; sh[typ(e)][idx] += e.shares;
            if (typ(e) == 1) cn_total_sh += e.shares;
        }
    }
    std::printf("opening crosses with shares > 0: %llu, crossed shares %llu\n", (unsigned long long)crosses, (unsigned long long)q_sh);
    std::printf("same-stock messages 09:29:58-09:30:03, by time relative to that stock's Q (count / shares):\n");
    std::printf("%-12s %22s %22s %22s %10s\n", "offset", "E", "C printable=N", "C printable=Y", "D");
    for (int i = 0; i < 9; ++i)
        std::printf("%-12s %10llu %11llu %10llu %11llu %10llu %11llu %10llu\n", names[i],
            (unsigned long long)cnt[0][i], (unsigned long long)sh[0][i], (unsigned long long)cnt[1][i], (unsigned long long)sh[1][i],
            (unsigned long long)cnt[2][i], (unsigned long long)sh[2][i], (unsigned long long)cnt[3][i]);
    std::printf("all C printable=N shares near the crosses: %llu (%.1f%% of crossed shares, %.1f%% of 2x)\n",
        (unsigned long long)cn_total_sh, 100.0 * cn_total_sh / q_sh, 50.0 * cn_total_sh / q_sh);
}
