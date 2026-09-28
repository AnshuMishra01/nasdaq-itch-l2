#pragma once

#include "core/book.h"
#include "core/message.h"
#include "core/order.h"
#include "core/order_store.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace itch {

// Per-instrument trade state, for checking against published prices (M7 layer 2).
struct TradeState {
    std::uint32_t last_price = 0;    // last printable trade (E, printable C, P)
    std::uint64_t last_ns = 0;
    std::uint64_t volume = 0;        // printable shares, excluding crosses
    std::uint32_t open_cross = 0;    // Q type 'O'
    std::uint32_t close_cross = 0;   // Q type 'C': the official closing price
    std::uint64_t close_cross_shares = 0;
};

// Result of cross-checking the order store against the books (M7 layer 1).
struct VerifyResult {
    std::uint64_t orders = 0;               // orders walked in the store
    std::uint64_t levels = 0;               // levels walked in the books
    std::uint64_t share_mismatch = 0;       // level shares != sum of its orders' shares
    std::uint64_t count_mismatch = 0;       // level order_count != number of its orders
    std::uint64_t level_without_orders = 0; // level exists, no order in the store at that price
    std::uint64_t prices_without_level = 0; // (locate, side, price) with live orders but no level
    std::uint64_t unsorted = 0;             // side not strictly ordered best-at-back
    std::vector<std::string> examples;      // first few failures, for debugging

    bool ok() const {
        return share_mismatch + count_mismatch + level_without_orders + prices_without_level + unsorted == 0;
    }
};

// StoreT: the order store (see core/order_store.h). WithBooks = false keeps only the
// store, so the store's cost can be timed on its own (M9).
template <typename StoreT = Store, bool WithBooks = true>
struct BasicBookHandler {
    StoreT store;
    // Stock locate is a dense integer < 65,536: index directly, like the symbol table.
    // Every entry always exists, so a remove can never conjure up a phantom book.
    std::vector<Book> books = std::vector<Book>(65536);
    std::vector<std::array<char, 8>> symbols = std::vector<std::array<char, 8>>(65536);
    std::vector<TradeState> trades = std::vector<TradeState>(65536);

    std::uint64_t peak_live = 0;
    std::uint64_t removals = 0;

    // order-store anomalies
    std::uint64_t anomaly_exec_unknown = 0;
    std::uint64_t anomaly_cancel_unknown = 0;
    std::uint64_t anomaly_delete_unknown = 0;
    std::uint64_t anomaly_replace_orig_unknown = 0;
    std::uint64_t anomaly_add_existing = 0;
    std::uint64_t anomaly_over_execution = 0;
    std::uint64_t anomaly_over_cancel = 0;

    // book invariants (M5): all must be zero
    std::uint64_t level_missing = 0;   // order says price P, book has no level at P
    std::uint64_t level_underflow = 0; // removed more shares than the level held
    std::uint64_t crossed_updates = 0; // updates that left best_bid >= best_ask

    // price levels created or erased (a vector insert/erase); M8 relates these to the tail
    std::uint64_t level_changes = 0;

    std::size_t size() const { return store.size(); }
    std::uint64_t peak() const { return peak_live; }

    void book_add(const Order& o) {
        if constexpr (!WithBooks) return;
        Book& book = books[o.locate];
        const std::size_t before = book.levels(o.side).size();
        book.add(o.side, o.price, o.shares);
        level_changes += book.levels(o.side).size() != before;
        // Only adding liquidity can cross a book, so this checks every update that could.
        if (book.crossed()) crossed_updates++;
    }

    void book_reduce(const Order& o, std::uint32_t shares, bool order_gone) {
        if constexpr (!WithBooks) return;
        Book& book = books[o.locate];
        const std::size_t before = book.levels(o.side).size();
        const ReduceResult result = book.reduce(o.side, o.price, shares, order_gone);
        level_changes += book.levels(o.side).size() != before;
        switch (result) {
            case ReduceResult::Ok: break;
            case ReduceResult::MissingLevel: level_missing++; break;
            case ReduceResult::Underflow: level_underflow++; break;
        }
    }

    void add_order(std::uint64_t ref, const Order& info) {
        if (Order* existing = store.find(ref)) {
            anomaly_add_existing++;
            book_reduce(*existing, existing->shares, true); // the store overwrites it
        }
        store.add(ref, info);
        if (store.size() > peak_live) peak_live = static_cast<std::uint64_t>(store.size());
        book_add(info);
    }

    void remove_order(std::uint64_t ref, const Order& o) {
        book_reduce(o, o.shares, true);
        store.remove(ref);
        removals++;
    }

    void record_trade(std::uint16_t locate, std::uint32_t price, std::uint32_t shares, std::uint64_t ts) {
        TradeState& t = trades[locate];
        t.last_price = price;
        t.last_ns = ts;
        t.volume += shares;
    }

    enum class Print : std::uint8_t {
        None,         // X: a cancel, not a trade
        AtOrderPrice, // E: executed at the resting order's price
        AtPrice,      // printable C: executed at the price in the message
    };

    // E, C and X: take shares off a live order. `over` counts removing more than it has.
    void reduce_order(std::uint64_t ref, std::uint32_t shares,
                      std::uint64_t& unknown, std::uint64_t& over,
                      Print print = Print::None, std::uint32_t price = 0, std::uint64_t ts = 0) {
        Order* o = store.find(ref);
        if (!o) {
            unknown++;
            return;
        }
        if (print == Print::AtOrderPrice) record_trade(o->locate, o->price, shares, ts);
        if (print == Print::AtPrice) record_trade(o->locate, price, shares, ts);
        if (shares > o->shares) over++;
        if (shares >= o->shares) {
            remove_order(ref, *o);
        } else {
            book_reduce(*o, shares, false);
            o->shares -= shares;
        }
    }

    void on(const StockDirectoryMessage& m) { symbols[m.stockLocate] = m.symbol; }

    void on(const AddOrderMessage& m) {
        add_order(m.orderRef, Order{m.price, m.shares, m.stockLocate, m.side});
    }

    void on(const AddOrderMPIDMessage& m) {
        add_order(m.orderRef, Order{m.price, m.shares, m.stockLocate, m.side});
    }

    void on(const OrderExecutedMessage& m) {
        reduce_order(m.orderRef, m.executedShares, anomaly_exec_unknown, anomaly_over_execution,
                     Print::AtOrderPrice, 0, m.timeStamp);
    }

    void on(const OrderExecutedWithPriceMessage& m) {
        // printable 'N': the shares are reported in aggregate elsewhere (e.g. a cross's Q)
        const Print print = m.printable == 'Y' ? Print::AtPrice : Print::None;
        reduce_order(m.orderRef, m.executedShares, anomaly_exec_unknown, anomaly_over_execution,
                     print, m.price, m.timeStamp);
    }

    // P: hidden-order execution. Printable trade, no book change.
    void on(const TradeMessage& m) { record_trade(m.stockLocate, m.price, m.shares, m.timeStamp); }

    // Q: cross result. Informational only: it names no orders, so the book is untouched.
    void on(const CrossTradeMessage& m) {
        TradeState& t = trades[m.stockLocate];
        if (m.crossType == 'O') t.open_cross = m.crossPrice;
        if (m.crossType == 'C') {
            t.close_cross = m.crossPrice;
            t.close_cross_shares = m.shares;
        }
    }

    void on(const OrderCancelMessage& m) {
        reduce_order(m.orderRef, m.cancelledShares, anomaly_cancel_unknown, anomaly_over_cancel);
    }

    void on(const OrderDeleteMessage& m) {
        Order* o = store.find(m.orderRef);
        if (!o) {
            anomaly_delete_unknown++;
            return;
        }
        remove_order(m.orderRef, *o);
    }

    void on(const OrderReplaceMessage& m) {
        // Replace: remove orig, add new with new shares/price but keep side/locate from orig
        Order* o = store.find(m.origRef);
        if (!o) {
            anomaly_replace_orig_unknown++;
            return; // do not add a garbage order when original is missing
        }
        Order info = *o;
        remove_order(m.origRef, info);
        info.shares = m.shares;
        info.price = m.price;
        add_order(m.newRef, info);
    }

    // Linear scan over the symbol table; for reporting only, not the hot path.
    // Returns 0 if not found (locate 0 is never assigned by Nasdaq).
    std::uint16_t find_locate(std::string_view symbol) const {
        for (std::size_t loc = 0; loc < symbols.size(); ++loc) {
            const auto& s = symbols[loc];
            std::size_t len = 0;
            while (len < s.size() && s[len] != ' ' && s[len] != '\0') ++len;
            if (std::string_view(s.data(), len) == symbol) return static_cast<std::uint16_t>(loc);
        }
        return 0;
    }

    // optional: dump current store to file (ref shares price locate side)
    void dump(const std::string& path) const {
        std::ofstream out(path);
        if (!out) return;
        store.for_each([&](std::uint64_t ref, const Order& info) {
            out << ref << ' ' << info.shares << ' ' << info.price << ' ' << info.locate << ' ' << static_cast<char>(info.side) << '\n';
        });
    }

    // print top 5 levels each side of one symbol for eyeballing
    void print_top(std::string_view symbol, std::ostream& out) const {
        const std::uint16_t locate = find_locate(symbol);
        if (locate == 0) { out << "No symbol " << symbol << '\n'; return; }
        const Book& book = books[locate];
        auto print_side = [&](const char* name, Side side) {
            out << "  " << name << ":\n";
            for (const auto& pl : book.top_levels(side, 5)) {
                out << "    " << std::setw(6) << pl.price / 10000U << '.'
                    << std::setw(4) << std::setfill('0') << pl.price % 10000U << std::setfill(' ')
                    << "  shares=" << std::setw(7) << pl.shares
                    << "  orders=" << pl.order_count << '\n';
            }
        };
        out << "Top of book " << symbol << " (locate " << locate << "), best first:\n";
        print_side("asks", Side::Sell);
        print_side("bids", Side::Buy);
    }

    std::string symbol_of(std::size_t locate) const {
        const auto& s = symbols[locate];
        std::size_t len = 0;
        while (len < s.size() && s[len] != ' ' && s[len] != '\0') ++len;
        return len ? std::string(s.data(), len) : "#" + std::to_string(locate);
    }

    static std::string fmt_price(std::uint32_t p) {
        std::ostringstream o;
        o << p / 10000U << '.' << std::setw(4) << std::setfill('0') << p % 10000U;
        return o.str();
    }

    static std::string fmt_ns(std::uint64_t ns) {
        const std::uint64_t s = ns / 1'000'000'000ULL;
        std::ostringstream o;
        o << std::setfill('0') << std::setw(2) << s / 3600 << ':' << std::setw(2) << s / 60 % 60 << ':'
          << std::setw(2) << s % 60 << '.' << std::setw(3) << ns % 1'000'000'000ULL / 1'000'000ULL;
        return o.str();
    }

    // M7 layer 1: rebuild every level from the order store and compare with the books.
    // The two paths share no arithmetic, so any drift (missed decrement, double add) shows
    // up exactly. O(orders + levels) with allocation: a verification mode, not hot path.
    VerifyResult verify_levels() const {
        struct Agg {
            std::uint64_t shares = 0;
            std::uint32_t count = 0;
        };
        auto key = [](std::size_t loc, Side side, std::uint32_t price) {
            return (std::uint64_t{loc} << 33) | (std::uint64_t{side == Side::Buy} << 32) | price;
        };
        VerifyResult r;
        auto where = [&](std::size_t loc, Side side, std::uint32_t price) {
            return symbol_of(loc) + ' ' + static_cast<char>(side) + ' ' + fmt_price(price);
        };
        auto note = [&](std::string s) {
            if (r.examples.size() < 10) r.examples.push_back(std::move(s));
        };

        std::unordered_map<std::uint64_t, Agg> from_store;
        from_store.reserve(store.size());
        store.for_each([&](std::uint64_t, const Order& o) {
            Agg& a = from_store[key(o.locate, o.side, o.price)];
            a.shares += o.shares;
            a.count++;
            r.orders++;
        });

        for (std::size_t loc = 0; loc < books.size(); ++loc) {
            for (Side side : {Side::Buy, Side::Sell}) {
                const auto& lv = books[loc].levels(side);
                for (std::size_t i = 0; i < lv.size(); ++i) {
                    const PriceLevel& l = lv[i];
                    r.levels++;
                    if (i > 0 && !Book::better(side, l.price, lv[i - 1].price)) {
                        r.unsorted++;
                        note("unsorted: " + where(loc, side, l.price));
                    }
                    auto it = from_store.find(key(loc, side, l.price));
                    if (it == from_store.end()) {
                        r.level_without_orders++;
                        note("level without orders: " + where(loc, side, l.price));
                        continue;
                    }
                    if (it->second.shares != l.shares) {
                        r.share_mismatch++;
                        note("shares: " + where(loc, side, l.price) + " book=" + std::to_string(l.shares) +
                             " store=" + std::to_string(it->second.shares));
                    }
                    if (it->second.count != l.order_count) {
                        r.count_mismatch++;
                        note("order_count: " + where(loc, side, l.price) + " book=" + std::to_string(l.order_count) +
                             " store=" + std::to_string(it->second.count));
                    }
                    from_store.erase(it);
                }
            }
        }
        // whatever is left in from_store is a price with live orders but no level
        r.prices_without_level = from_store.size();
        for (const auto& [k, a] : from_store) {
            const auto loc = static_cast<std::size_t>(k >> 33);
            const Side side = (k >> 32 & 1U) ? Side::Buy : Side::Sell;
            note("price without level: " + where(loc, side, static_cast<std::uint32_t>(k)) +
                 " (" + std::to_string(a.count) + " orders)");
        }
        return r;
    }

    static void print_verify(const VerifyResult& r, std::ostream& out) {
        out << (r.ok() ? "  PASS" : "  FAIL") << "  orders=" << r.orders << " levels=" << r.levels
            << " share_mismatch=" << r.share_mismatch << " count_mismatch=" << r.count_mismatch
            << " level_without_orders=" << r.level_without_orders
            << " prices_without_level=" << r.prices_without_level << " unsorted=" << r.unsorted << '\n';
        for (const auto& e : r.examples) out << "      " << e << '\n';
    }

    // M7 layer 4: one number for the whole final state. Books and trades are hashed in
    // locate/level order. The store is hashed order-independently (a sum of per-order
    // hashes), because hash-table iteration order is not part of the state and will
    // change when the table is replaced in M9.
    struct StateHash {
        std::uint64_t book = 0;
        std::uint64_t store = 0;
        std::uint64_t trades = 0;
        std::uint64_t combined = 0;
    };

    static std::uint64_t mix(std::uint64_t x) { // splitmix64 finaliser
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    StateHash state_hash() const {
        StateHash h;
        auto feed = [](std::uint64_t& acc, std::uint64_t v) { acc = mix(acc ^ v) + 0x9e3779b97f4a7c15ULL; };

        for (std::size_t loc = 0; loc < books.size(); ++loc) {
            for (Side side : {Side::Buy, Side::Sell}) {
                const auto& lv = books[loc].levels(side);
                if (lv.empty()) continue;
                feed(h.book, loc << 8 | static_cast<std::uint8_t>(side));
                for (const auto& l : lv) {
                    feed(h.book, l.price);
                    feed(h.book, std::uint64_t{l.shares} << 32 | l.order_count);
                }
            }
        }

        store.for_each([&](std::uint64_t ref, const Order& o) {
            h.store += mix(ref ^ mix(std::uint64_t{o.price} << 32 | o.shares) ^
                           mix(std::uint64_t{o.locate} << 8 | static_cast<std::uint8_t>(o.side)));
        });

        for (std::size_t loc = 0; loc < trades.size(); ++loc) {
            const TradeState& t = trades[loc];
            if (t.last_ns == 0 && t.open_cross == 0 && t.close_cross == 0) continue;
            feed(h.trades, loc);
            feed(h.trades, t.last_price);
            feed(h.trades, t.last_ns);
            feed(h.trades, t.volume);
            feed(h.trades, std::uint64_t{t.open_cross} << 32 | t.close_cross);
            feed(h.trades, t.close_cross_shares);
        }

        h.combined = mix(h.book ^ mix(h.store ^ mix(h.trades)));
        return h;
    }

    // M7 layer 2: what to compare with the published close for each symbol.
    void print_trades(const std::vector<std::string>& wanted, std::ostream& out) const {
        out << "Trades (compare close_cross with the published close):\n"
            << "  symbol      last_trade  at            volume  open_cross   close_cross  close_shares\n";
        for (const auto& sym : wanted) {
            const std::uint16_t loc = find_locate(sym);
            if (loc == 0) { out << "  " << sym << ": not in file\n"; continue; }
            const TradeState& t = trades[loc];
            auto opt = [&](std::uint32_t p) { return p ? fmt_price(p) : std::string("-"); };
            out << "  " << std::left << std::setw(8) << sym << std::right
                << std::setw(14) << opt(t.last_price)
                << "  " << std::setw(12) << (t.last_ns ? fmt_ns(t.last_ns) : std::string("-"))
                << std::setw(10) << t.volume
                << std::setw(13) << opt(t.open_cross)
                << std::setw(14) << opt(t.close_cross)
                << std::setw(14) << t.close_cross_shares << '\n';
        }
    }

    static void print_anomalies(const BasicBookHandler& h, std::ostream& out) {
        out << "Anomalies:\n";
        out << "  exec_unknown=" << h.anomaly_exec_unknown << '\n';
        out << "  cancel_unknown=" << h.anomaly_cancel_unknown << '\n';
        out << "  delete_unknown=" << h.anomaly_delete_unknown << '\n';
        out << "  replace_orig_unknown=" << h.anomaly_replace_orig_unknown << '\n';
        out << "  add_existing=" << h.anomaly_add_existing << '\n';
        out << "  over_execution=" << h.anomaly_over_execution << '\n';
        out << "  over_cancel=" << h.anomaly_over_cancel << '\n';
        out << "  peak_live=" << h.peak() << '\n';
        out << "Book invariants:\n";
        out << "  level_missing=" << h.level_missing << '\n';
        out << "  level_underflow=" << h.level_underflow << '\n';
        out << "  crossed_updates=" << h.crossed_updates << '\n';
    }
};


using BookHandler = BasicBookHandler<>;

} // namespace itch
