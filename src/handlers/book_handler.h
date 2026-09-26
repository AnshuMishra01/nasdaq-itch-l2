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
#include <string_view>
#include <vector>

namespace itch {

struct BookHandler {
    Store store;
    // Stock locate is a dense integer < 65,536: index directly, like the symbol table.
    // Every entry always exists, so a remove can never conjure up a phantom book.
    std::vector<Book> books = std::vector<Book>(65536);
    std::vector<std::array<char, 8>> symbols = std::vector<std::array<char, 8>>(65536);

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

    std::size_t size() const { return store.size(); }
    std::uint64_t peak() const { return peak_live; }

    void book_add(const Order& o) {
        Book& book = books[o.locate];
        book.add(o.side, o.price, o.shares);
        // Only adding liquidity can cross a book, so this checks every update that could.
        if (book.crossed()) crossed_updates++;
    }

    void book_reduce(const Order& o, std::uint32_t shares, bool order_gone) {
        switch (books[o.locate].reduce(o.side, o.price, shares, order_gone)) {
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

    // E, C and X: take shares off a live order. `over` counts removing more than it has.
    void reduce_order(std::uint64_t ref, std::uint32_t shares,
                      std::uint64_t& unknown, std::uint64_t& over) {
        Order* o = store.find(ref);
        if (!o) {
            unknown++;
            return;
        }
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
        reduce_order(m.orderRef, m.executedShares, anomaly_exec_unknown, anomaly_over_execution);
    }

    void on(const OrderExecutedWithPriceMessage& m) {
        reduce_order(m.orderRef, m.executedShares, anomaly_exec_unknown, anomaly_over_execution);
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
        for (const auto& p : store.data()) {
            const auto& ref = p.first;
            const auto& info = p.second;
            out << ref << ' ' << info.shares << ' ' << info.price << ' ' << info.locate << ' ' << static_cast<char>(info.side) << '\n';
        }
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

    static void print_anomalies(const BookHandler& h, std::ostream& out) {
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

} // namespace itch
