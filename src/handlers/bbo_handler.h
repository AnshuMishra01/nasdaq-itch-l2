#pragma once

// A consumer of the book: tracks every stock's best bid and offer (BBO) as messages are
// applied, the way a strategy or market-data publisher would read it.
//   - per stock: how many times the BBO changed, and the time-weighted quoted spread
//     during regular hours (09:30-16:00), counting only time with both sides present
//   - for one chosen stock: a snapshot of its top 5 levels at a chosen time, and a log of
//     every BBO change inside a chosen time window

#include "handlers/book_handler.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace itch {

struct Bbo {
    std::uint32_t bid = 0, bid_shares = 0, ask = 0, ask_shares = 0; // 0 = side empty
    bool operator==(const Bbo&) const = default;
};

struct BboHandler {
    static constexpr std::uint64_t kOpen = 34'200'000'000'000ULL;  // 09:30:00
    static constexpr std::uint64_t kClose = 57'600'000'000'000ULL; // 16:00:00

    BookHandler book;

    struct PerStock {
        Bbo last;
        std::uint64_t since = 0;        // when `last` became the BBO
        std::uint64_t changes = 0;
        double spread_x_ns = 0;         // sum of spread * time, regular hours, two-sided only
        std::uint64_t two_sided_ns = 0; // time with both sides present, regular hours
    };
    std::vector<PerStock> stocks = std::vector<PerStock>(65536);

    // the one stock to show in detail
    std::string focus;
    std::uint16_t focus_locate = 0;
    std::uint64_t snapshot_at = 0, log_from = 0, log_to = 0;
    bool snapshot_done = false;
    std::vector<std::string> snapshot_lines;
    struct Change { std::uint64_t ts; Bbo bbo; };
    std::vector<Change> log;

    // HH:MM:SS.nnnnnnnnn
    static std::string fmt_full(std::uint64_t ns) {
        const std::uint64_t s = ns / 1'000'000'000ULL;
        std::ostringstream o;
        o << std::setfill('0') << std::setw(2) << s / 3600 << ':' << std::setw(2) << s / 60 % 60 << ':' << std::setw(2) << s % 60
          << '.' << std::setw(9) << ns % 1'000'000'000ULL;
        return o.str();
    }

    Bbo current(std::uint16_t loc) const {
        const Book& b = book.books[loc];
        Bbo q;
        if (!b.bids.empty()) { q.bid = b.bids.back().price; q.bid_shares = b.bids.back().shares; }
        if (!b.asks.empty()) { q.ask = b.asks.back().price; q.ask_shares = b.asks.back().shares; }
        return q;
    }

    // time-weight the spread that was in force from s.since to now, clipped to regular hours
    void accrue(PerStock& s, std::uint64_t now) {
        const std::uint64_t a = std::max(s.since, kOpen), z = std::min(now, kClose);
        if (z > a && s.last.bid && s.last.ask) {
            const auto dt = z - a;
            s.two_sided_ns += dt;
            s.spread_x_ns += (static_cast<double>(s.last.ask) - static_cast<double>(s.last.bid)) * static_cast<double>(dt);
        }
    }

    void take_snapshot() {
        const Book& b = book.books[focus_locate];
        auto line = [&](const char* side, const PriceLevel& l) {
            std::ostringstream o;
            o << "  " << side << "  " << std::setw(10) << BookHandler::fmt_price(l.price) << "  " << std::setw(7) << l.shares
              << " shares  " << std::setw(3) << l.order_count << " orders";
            snapshot_lines.push_back(o.str());
        };
        const auto asks = b.top_levels(Side::Sell, 5), bids = b.top_levels(Side::Buy, 5);
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) line("ask", *it);
        snapshot_lines.push_back("  ----------------------------------------");
        for (const auto& l : bids) line("bid", l);
        snapshot_done = true;
    }

    void after(std::uint16_t loc, std::uint64_t ts) {
        if (!focus_locate && !focus.empty() && book.symbol_of(loc) == focus) focus_locate = loc;
        if (focus_locate && !snapshot_done && ts >= snapshot_at) take_snapshot();
        const Bbo q = current(loc);
        PerStock& s = stocks[loc];
        if (q == s.last) return;
        accrue(s, ts);
        s.last = q;
        s.since = ts;
        ++s.changes;
        if (loc == focus_locate && ts >= log_from && ts < log_to) log.push_back({ts, q});
    }

    template <typename M>
    void on(const M& m) {
        book.on(m);
        if constexpr (requires { m.stockLocate; m.timeStamp; }) after(m.stockLocate, m.timeStamp);
    }
    void on(const StockDirectoryMessage& m) {
        book.on(m);
        if (!focus_locate && book.symbol_of(m.stockLocate) == focus) focus_locate = m.stockLocate;
    }

    void report(std::ostream& out) {
        for (std::size_t loc = 0; loc < stocks.size(); ++loc) accrue(stocks[loc], kClose);
        out << std::fixed;
        if (focus_locate) {
            out << "Top of book " << focus << " at " << BookHandler::fmt_ns(snapshot_at) << " (best ask and best bid next to the line):\n";
            for (const auto& l : snapshot_lines) out << l << '\n';
            out << "\nBBO changes for " << focus << " from " << BookHandler::fmt_ns(log_from) << " to "
                << BookHandler::fmt_ns(log_to) << " (" << log.size() << "):\n"
                << "  time                     bid x shares          ask x shares   spread\n";
            for (const auto& c : log)
                out << "  " << fmt_full(c.ts)
                    << "  " << std::setw(10) << BookHandler::fmt_price(c.bbo.bid) << " x " << std::setw(6) << c.bbo.bid_shares
                    << "   " << std::setw(10) << BookHandler::fmt_price(c.bbo.ask) << " x " << std::setw(6) << c.bbo.ask_shares
                    << "   " << std::setprecision(2) << (c.bbo.ask && c.bbo.bid ? (static_cast<double>(c.bbo.ask) - c.bbo.bid) / 100.0 : 0.0) << "c\n";
        }
        std::vector<std::size_t> order;
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < stocks.size(); ++i)
            if (stocks[i].changes) { order.push_back(i); total += stocks[i].changes; }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return stocks[a].changes > stocks[b].changes; });
        out << "\nBBO changes (best price or its size) over the whole file: " << total << " across " << order.size() << " stocks.\n"
            << "Most active 15, with time-weighted quoted spread during 09:30-16:00 (two-sided time only):\n"
            << "  symbol    BBO changes   avg spread    two-sided\n";
        for (std::size_t k = 0; k < order.size() && k < 15; ++k) {
            const PerStock& s = stocks[order[k]];
            const double spread_cents = s.two_sided_ns ? s.spread_x_ns / static_cast<double>(s.two_sided_ns) / 100.0 : 0.0;
            out << "  " << std::left << std::setw(8) << book.symbol_of(order[k]) << std::right << std::setw(13) << s.changes
                << std::setw(11) << std::setprecision(2) << spread_cents << "c" << std::setw(11) << std::setprecision(1)
                << 100.0 * static_cast<double>(s.two_sided_ns) / static_cast<double>(kClose - kOpen) << "%\n";
        }
        if (focus_locate) {
            const PerStock& s = stocks[focus_locate];
            out << "  " << focus << ": " << s.changes << " BBO changes, time-weighted spread " << std::setprecision(2)
                << (s.two_sided_ns ? s.spread_x_ns / static_cast<double>(s.two_sided_ns) / 100.0 : 0.0) << "c\n";
        }
    }
};

} // namespace itch
