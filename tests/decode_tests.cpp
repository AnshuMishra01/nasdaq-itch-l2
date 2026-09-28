// M7 layer 3: decoder unit tests. Hand-built messages with chosen values, byte offsets
// taken from the ITCH 5.0 spec, then the decoded struct is checked field by field.
// Runs on every build (ctest), so a refactor that breaks a field fails immediately.

#include "decode.h"
#include "core/book.h"
#include "latency.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

int failures = 0;
int checks = 0;

template <typename T>
auto show(const T& v) {
    if constexpr (std::is_arithmetic_v<T>) return +v; // chars and bytes print as numbers
    else return v;
}

template <typename A, typename B>
void check_eq(const A& got, const B& want, const char* expr, int line) {
    ++checks;
    if (!(got == want)) {
        ++failures;
        std::cout << "FAIL line " << line << ": " << expr << "  got " << show(got) << " want " << show(want) << '\n';
    }
}
#define CHECK_EQ(got, want) check_eq((got), (want), #got " == " #want, __LINE__)

// Writes `value` big-endian into the low `n` bytes at msg + offset.
template <std::size_t N>
void put(std::array<unsigned char, N>& msg, std::size_t offset, std::uint64_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) msg[offset + n - 1 - i] = static_cast<unsigned char>(value >> (8 * i));
}

template <std::size_t N>
void put_text(std::array<unsigned char, N>& msg, std::size_t offset, const char* text, std::size_t n) {
    std::memset(msg.data() + offset, ' ', n);
    std::memcpy(msg.data() + offset, text, std::min(n, std::strlen(text)));
}

bool symbol_is(const std::array<char, 8>& s, const char* want) {
    std::array<char, 8> padded;
    padded.fill(' ');
    std::memcpy(padded.data(), want, std::strlen(want));
    return s == padded;
}

constexpr std::uint64_t kMaxTs = 0xFFFF'FFFF'FFFFULL; // largest 6-byte timestamp
constexpr std::uint64_t kMaxRef = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();

void test_read_be() {
    const unsigned char six[6] = {0x0A, 0x11, 0xEA, 0x0E, 0x8C, 0x43};
    CHECK_EQ((read_be<std::uint64_t, 6>(six)), 0x0A11EA0E8C43ULL);
    const unsigned char ones[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ((read_be<std::uint64_t, 6>(ones)), kMaxTs); // must not read a 7th byte
    CHECK_EQ(read_be<std::uint64_t>(ones), kMaxRef);
    const unsigned char two[2] = {0x12, 0x34};
    CHECK_EQ(read_be<std::uint16_t>(two), 0x1234);
}

void test_stock_directory() {
    std::array<unsigned char, 39> m{};
    m[0] = 'R';
    put(m, 1, 0xFFFF, 2);
    put(m, 5, kMaxTs, 6);
    put_text(m, 11, "ABCDEFGH", 8); // symbol with no padding at all
    const auto d = decode_stock_directory(m.data());
    CHECK_EQ(d.stockLocate, 0xFFFF);
    CHECK_EQ(symbol_is(d.symbol, "ABCDEFGH"), true);

    put_text(m, 11, "A", 8); // one letter, seven spaces
    CHECK_EQ(symbol_is(decode_stock_directory(m.data()).symbol, "A"), true);
}

void test_add_order() {
    std::array<unsigned char, 36> m{};
    m[0] = 'A';
    put(m, 1, 13, 2);
    put(m, 3, 7, 2);
    put(m, 5, kMaxTs, 6);
    put(m, 11, kMaxRef, 8);
    m[19] = 'S';
    put(m, 20, kMaxU32, 4);
    put_text(m, 24, "AAPL", 8);
    put(m, 32, 0, 4); // price 0
    const auto a = decode_add_order(m.data());
    CHECK_EQ(a.stockLocate, 13);
    CHECK_EQ(a.timeStamp, kMaxTs);
    CHECK_EQ(a.orderRef, kMaxRef);
    CHECK_EQ(a.side == Side::Sell, true);
    CHECK_EQ(a.shares, kMaxU32);
    CHECK_EQ(a.price, 0u);

    m[19] = 'B';
    put(m, 32, 2'915'200, 4); // $291.52 with 4 implied decimals
    const auto b = decode_add_order(m.data());
    CHECK_EQ(b.side == Side::Buy, true);
    CHECK_EQ(b.price, 2'915'200u);
}

void test_add_order_mpid() {
    std::array<unsigned char, 40> m{};
    m[0] = 'F';
    put(m, 1, 42, 2);
    put(m, 3, 0xBEEF, 2);
    put(m, 5, 1, 6);
    put(m, 11, 0x0102030405060708ULL, 8);
    m[19] = 'B';
    put(m, 20, 100, 4);
    put_text(m, 24, "MSFT", 8);
    put(m, 32, 1'587'800, 4);
    put_text(m, 36, "GSCO", 4);
    const auto f = decode_add_order_mpid(m.data());
    CHECK_EQ(f.stockLocate, 42);
    CHECK_EQ(f.trackingNumber, 0xBEEF);
    CHECK_EQ(f.timeStamp, 1u);
    CHECK_EQ(f.orderRef, 0x0102030405060708ULL);
    CHECK_EQ(f.side == Side::Buy, true);
    CHECK_EQ(f.shares, 100u);
    CHECK_EQ(f.price, 1'587'800u);
    CHECK_EQ(std::string(f.attribution.data(), 4), std::string("GSCO"));
}

void test_executed() {
    std::array<unsigned char, 31> m{};
    m[0] = 'E';
    put(m, 1, 9, 2);
    put(m, 3, 2, 2);
    put(m, 5, kMaxTs, 6);
    put(m, 11, kMaxRef, 8);
    put(m, 19, 250, 4);
    put(m, 23, kMaxRef - 1, 8);
    const auto e = decode_order_executed(m.data());
    CHECK_EQ(e.stockLocate, 9);
    CHECK_EQ(e.trackingNumber, 2);
    CHECK_EQ(e.timeStamp, kMaxTs);
    CHECK_EQ(e.orderRef, kMaxRef);
    CHECK_EQ(e.executedShares, 250u);
    CHECK_EQ(e.matchNumber, kMaxRef - 1);
}

void test_executed_with_price() {
    std::array<unsigned char, 36> m{};
    m[0] = 'C';
    put(m, 1, 9, 2);
    put(m, 5, 34'200'000'000'000ULL, 6); // 09:30:00
    put(m, 11, 5'706'488, 8);
    put(m, 19, 1, 4);
    put(m, 23, 77, 8);
    m[31] = 'N';
    put(m, 32, kMaxU32, 4);
    const auto c = decode_order_executed_with_price(m.data());
    CHECK_EQ(c.timeStamp, 34'200'000'000'000ULL);
    CHECK_EQ(c.orderRef, 5'706'488u);
    CHECK_EQ(c.executedShares, 1u);
    CHECK_EQ(c.matchNumber, 77u);
    CHECK_EQ(c.printable, 'N');
    CHECK_EQ(c.price, kMaxU32);
}

void test_cancel_delete_replace() {
    std::array<unsigned char, 23> x{};
    x[0] = 'X';
    put(x, 1, 3, 2);
    put(x, 5, 12345, 6);
    put(x, 11, kMaxRef, 8);
    put(x, 19, 99, 4);
    const auto cx = decode_order_cancel(x.data());
    CHECK_EQ(cx.stockLocate, 3);
    CHECK_EQ(cx.timeStamp, 12345u);
    CHECK_EQ(cx.orderRef, kMaxRef);
    CHECK_EQ(cx.cancelledShares, 99u);

    std::array<unsigned char, 19> d{};
    d[0] = 'D';
    put(d, 1, 4, 2);
    put(d, 3, 5, 2);
    put(d, 5, kMaxTs, 6);
    put(d, 11, 42, 8);
    const auto dd = decode_order_delete(d.data());
    CHECK_EQ(dd.stockLocate, 4);
    CHECK_EQ(dd.trackingNumber, 5);
    CHECK_EQ(dd.timeStamp, kMaxTs);
    CHECK_EQ(dd.orderRef, 42u);

    std::array<unsigned char, 35> u{};
    u[0] = 'U';
    put(u, 1, 6, 2);
    put(u, 5, 1, 6);
    put(u, 11, kMaxRef, 8);
    put(u, 19, kMaxRef - 7, 8);
    put(u, 27, 300, 4);
    put(u, 31, 0, 4);
    const auto uu = decode_order_replace(u.data());
    CHECK_EQ(uu.stockLocate, 6);
    CHECK_EQ(uu.origRef, kMaxRef);
    CHECK_EQ(uu.newRef, kMaxRef - 7);
    CHECK_EQ(uu.shares, 300u);
    CHECK_EQ(uu.price, 0u);
}

void test_trade_and_cross() {
    std::array<unsigned char, 44> p{};
    p[0] = 'P';
    put(p, 1, 13, 2);
    put(p, 5, kMaxTs, 6);
    put(p, 11, 0, 8); // P order refs are zeroed by Nasdaq
    p[19] = 'B';
    put(p, 20, 500, 4);
    put_text(p, 24, "AAPL", 8);
    put(p, 32, 2'896'300, 4);
    put(p, 36, kMaxRef, 8);
    const auto t = decode_trade(p.data());
    CHECK_EQ(t.stockLocate, 13);
    CHECK_EQ(t.timeStamp, kMaxTs);
    CHECK_EQ(t.orderRef, 0u);
    CHECK_EQ(t.side == Side::Buy, true);
    CHECK_EQ(t.shares, 500u);
    CHECK_EQ(symbol_is(t.symbol, "AAPL"), true);
    CHECK_EQ(t.price, 2'896'300u);
    CHECK_EQ(t.matchNumber, kMaxRef);

    std::array<unsigned char, 40> q{};
    q[0] = 'Q';
    put(q, 1, 13, 2);
    put(q, 5, 57'600'000'000'000ULL, 6); // 16:00:00
    put(q, 11, kMaxRef, 8);              // 8-byte share count
    put_text(q, 19, "AAPL", 8);
    put(q, 27, 2'915'200, 4);
    put(q, 31, 123, 8);
    q[39] = 'C';
    const auto c = decode_cross_trade(q.data());
    CHECK_EQ(c.stockLocate, 13);
    CHECK_EQ(c.timeStamp, 57'600'000'000'000ULL);
    CHECK_EQ(c.shares, kMaxRef);
    CHECK_EQ(symbol_is(c.symbol, "AAPL"), true);
    CHECK_EQ(c.crossPrice, 2'915'200u);
    CHECK_EQ(c.matchNumber, 123u);
    CHECK_EQ(c.crossType, 'C');
}

// Dispatch routes each type to the right on(); P and Q reach only handlers that want them.
struct Recorder {
    std::string seen;
    void on(const StockDirectoryMessage&) { seen += 'R'; }
    void on(const AddOrderMessage&) { seen += 'A'; }
    void on(const AddOrderMPIDMessage&) { seen += 'F'; }
    void on(const OrderExecutedMessage&) { seen += 'E'; }
    void on(const OrderExecutedWithPriceMessage&) { seen += 'C'; }
    void on(const OrderCancelMessage&) { seen += 'X'; }
    void on(const OrderDeleteMessage&) { seen += 'D'; }
    void on(const OrderReplaceMessage&) { seen += 'U'; }
};
struct TradeRecorder : Recorder {
    using Recorder::on;
    void on(const TradeMessage&) { seen += 'P'; }
    void on(const CrossTradeMessage&) { seen += 'Q'; }
};

void test_dispatch() {
    std::array<unsigned char, 64> buf{};
    Recorder r;
    TradeRecorder tr;
    int counted = 0;
    for (char type : std::string("RAFECXDUPQS")) {
        buf[0] = static_cast<unsigned char>(type);
        counted += itch::dispatch(buf[0], buf.data(), r) ? 1 : 0;
        itch::dispatch(buf[0], buf.data(), tr);
    }
    CHECK_EQ(r.seen, std::string("RAFECXDU"));    // P, Q skipped: Recorder has no on() for them
    CHECK_EQ(tr.seen, std::string("RAFECXDUPQ")); // S has no handler anywhere
    CHECK_EQ(counted, 7);                         // A F E C X D U count; R P Q S do not
}

// Book basics, the same paths the handler uses.
void test_book() {
    itch::Book b;
    b.add(Side::Buy, 100, 10);
    b.add(Side::Buy, 102, 5);
    b.add(Side::Buy, 101, 7);
    b.add(Side::Buy, 102, 1);
    CHECK_EQ(b.bids.size(), 3u);
    CHECK_EQ(b.bids.back().price, 102u); // best at back
    CHECK_EQ(b.bids.back().shares, 6u);
    CHECK_EQ(b.bids.back().order_count, 2u);
    CHECK_EQ(b.bids.front().price, 100u);

    b.add(Side::Sell, 105, 3);
    b.add(Side::Sell, 104, 2);
    CHECK_EQ(b.asks.back().price, 104u); // lowest ask at back
    CHECK_EQ(b.crossed(), false);
    b.add(Side::Sell, 102, 1);
    CHECK_EQ(b.crossed(), true); // locked counts as crossed

    CHECK_EQ(b.reduce(Side::Buy, 101, 7, true) == itch::ReduceResult::Ok, true);
    CHECK_EQ(b.bids.size(), 2u); // emptied level erased
    CHECK_EQ(b.reduce(Side::Buy, 101, 1, true) == itch::ReduceResult::MissingLevel, true);
    CHECK_EQ(b.reduce(Side::Buy, 100, 11, true) == itch::ReduceResult::Underflow, true);
    CHECK_EQ(b.reduce(Side::Buy, 102, 2, false) == itch::ReduceResult::Ok, true);
    CHECK_EQ(b.bids.back().shares, 4u);
    CHECK_EQ(b.bids.back().order_count, 2u); // partial: order still there
}

// M8 histogram: every value lands in a bucket whose range contains it, buckets are
// contiguous, and percentiles of a known distribution come out right.
void test_histogram() {
    using itch::Histogram;
    bool buckets_ok = true;
    auto check_value = [&](std::uint64_t v) {
        const std::size_t i = Histogram::index(v);
        const bool inside = Histogram::upper_bound(i) >= v && (i == 0 || Histogram::upper_bound(i - 1) < v);
        const bool narrow = v < 16 || (Histogram::upper_bound(i) - v) * 16 <= v; // <= 6.25% wide
        if (!inside || !narrow || i >= Histogram::kBuckets) buckets_ok = false;
    };
    for (std::uint64_t v = 0; v < 100'000; ++v) check_value(v);
    for (unsigned e = 17; e < 64; ++e) {
        const std::uint64_t p = std::uint64_t{1} << e;
        check_value(p - 1);
        check_value(p);
        check_value(p + 1);
    }
    check_value(~std::uint64_t{0});
    CHECK_EQ(buckets_ok, true);

    Histogram h;
    for (std::uint64_t v = 1; v <= 10'000; ++v) h.record(v); // uniform 1..10000
    CHECK_EQ(h.count(), 10'000u);
    CHECK_EQ(h.max(), 10'000u);
    const auto near = [](std::uint64_t got, std::uint64_t want) { return got >= want && got <= want + want / 16; };
    CHECK_EQ(near(h.percentile(0.5), 5'000), true);
    CHECK_EQ(near(h.percentile(0.99), 9'900), true);
    CHECK_EQ(h.percentile(1.0), 10'000u);
    CHECK_EQ(h.mean(), 5'000.5);

    Histogram tail;
    for (int i = 0; i < 9'999; ++i) tail.record(50);
    tail.record(1'000'000); // one outlier in 10,000
    CHECK_EQ(tail.percentile(0.999), 51u); // 50 is in bucket [50, 51]: upper bound reported
    CHECK_EQ(tail.percentile(0.99995), 1'000'000u); // p99.995 is the outlier, capped at max
}

} // namespace

int main() {
    test_read_be();
    test_stock_directory();
    test_add_order();
    test_add_order_mpid();
    test_executed();
    test_executed_with_price();
    test_cancel_delete_replace();
    test_trade_and_cross();
    test_dispatch();
    test_book();
    test_histogram();
    std::cout << (failures ? "FAILED " : "OK ") << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
