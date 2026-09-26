#pragma once

#include "core/message.h"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <ostream>
#include <iomanip>
#include <cmath>

namespace itch {

struct Stats {
    std::uint64_t count_AF = 0; // A + F messages (adds)
    std::uint64_t count_D = 0;  // D messages (deletes)
    std::uint64_t count_U = 0; // U messages (replace)
    std::uint64_t count_X = 0; // X messages (cancel)
    std::uint64_t count_E = 0; // E/C executions

    std::uint64_t max_order_ref = 0;
    std::uint64_t min_order_ref = std::numeric_limits<std::uint64_t>::max();

    std::uint64_t peak_live = 0;

    // current live orders map: orderRef -> remaining shares
    std::unordered_map<std::uint64_t, std::uint32_t> order_shares;

    // number of times an order actually left the book (removed)
    std::uint64_t removals = 0;
};

struct StatsHandler {
    Stats stats;

    void on(const AddOrderMessage& m) {
        stats.count_AF++;
        update_refs(m.orderRef);
        // add order with initial shares
        stats.order_shares[m.orderRef] = m.shares;
        stats.peak_live = std::max(stats.peak_live, static_cast<std::uint64_t>(stats.order_shares.size()));
    }

    void on(const AddOrderMPIDMessage& m) {
        stats.count_AF++;
        update_refs(m.orderRef);
        stats.order_shares[m.orderRef] = m.shares;
        stats.peak_live = std::max(stats.peak_live, static_cast<std::uint64_t>(stats.order_shares.size()));
    }

    void on(const OrderCancelMessage& m) {
        stats.count_X++;
        auto it = stats.order_shares.find(m.orderRef);
        if (it != stats.order_shares.end()) {
            if (m.cancelledShares >= it->second) {
                // removed
                stats.order_shares.erase(it);
                stats.removals++;
            } else {
                it->second -= m.cancelledShares;
            }
        }
    }

    void on(const OrderDeleteMessage& m) {
        stats.count_D++;
        auto it = stats.order_shares.find(m.orderRef);
        if (it != stats.order_shares.end()) {
            stats.order_shares.erase(it);
            stats.removals++;
        }
    }

    void on(const OrderReplaceMessage& m) {
        stats.count_U++;
        // remove original ref if present
        auto it = stats.order_shares.find(m.origRef);
        if (it != stats.order_shares.end()) {
            stats.order_shares.erase(it);
            stats.removals++;
        }
        // add new ref with provided shares
        stats.order_shares[m.newRef] = m.shares;
        update_refs(m.newRef);
        stats.peak_live = std::max(stats.peak_live, static_cast<std::uint64_t>(stats.order_shares.size()));
    }

    void on(const OrderExecutedMessage& m) {
        stats.count_E++;
        auto it = stats.order_shares.find(m.orderRef);
        if (it != stats.order_shares.end()) {
            if (m.executedShares >= it->second) {
                stats.order_shares.erase(it);
                stats.removals++;
            } else {
                it->second -= m.executedShares;
            }
        }
    }

    void on(const OrderExecutedWithPriceMessage& m) {
        stats.count_E++;
        auto it = stats.order_shares.find(m.orderRef);
        if (it != stats.order_shares.end()) {
            if (m.executedShares >= it->second) {
                stats.order_shares.erase(it);
                stats.removals++;
            } else {
                it->second -= m.executedShares;
            }
        }
    }

    void on(const StockDirectoryMessage&) {}

    void update_refs(std::uint64_t ref) {
        if (ref > stats.max_order_ref) stats.max_order_ref = ref;
        if (ref < stats.min_order_ref) stats.min_order_ref = ref;
    }
};

inline void print_stats(const Stats& s, std::ostream& out) {
    out << "Metrics:\n";
    out << "  A+F count = " << s.count_AF << '\n';
    out << "  D count = " << s.count_D << '\n';
    out << "  U count = " << s.count_U << '\n';
    out << "  X count = " << s.count_X << '\n';
    out << "  E/C count = " << s.count_E << '\n';
    if (s.min_order_ref == std::numeric_limits<std::uint64_t>::max()) {
        out << "  No order refs seen\n";
    } else {
        out << "  Min order ref = " << s.min_order_ref << '\n';
        out << "  Max order ref = " << s.max_order_ref << '\n';
    }
    const std::uint64_t dxu = s.count_D + s.count_X + s.count_U;
    const double ratio = dxu == 0 ? 0.0 : static_cast<double>(s.count_AF) / static_cast<double>(dxu);
    out << "  A+F / D+X+U ratio = ";
    out << std::fixed << std::setprecision(4) << ratio << '\n';
    out << std::resetiosflags(std::ios_base::fixed);
    out << "  Peak live orders = " << s.peak_live << '\n';
    out << "  Removals observed = " << s.removals << '\n';

    // density and memory estimate for direct-indexed array
    if (s.max_order_ref > 0 && s.count_AF > 0) {
        const double density = static_cast<double>(s.count_AF) / static_cast<double>(s.max_order_ref);
        out << "  Reference density (adds/maxRef) = " << std::fixed << std::setprecision(4) << density << '\n';
        out << std::resetiosflags(std::ios_base::fixed);
        // estimate direct-indexed memory: 16 bytes per slot
        const std::uint64_t mem_bytes = static_cast<std::uint64_t>(s.max_order_ref) * 16ULL;
        out << "  Direct-indexed memory estimate = " << std::fixed << std::setprecision(2)
            << (static_cast<double>(mem_bytes) / (1024.0 * 1024.0)) << " MB\n";
        out << std::resetiosflags(std::ios_base::fixed);
        // hash-table estimate sized to peak_live: 16 bytes * next power of two
        std::uint64_t slots = 1ULL;
        while (slots < s.peak_live) slots <<= 1ULL;
        std::uint64_t hash_mem = slots * 16ULL;
        out << "  Dense hash estimate (power-of-two slots) = " << std::fixed << std::setprecision(2)
            << (static_cast<double>(hash_mem) / (1024.0 * 1024.0)) << " MB\n";
        out << std::resetiosflags(std::ios_base::fixed);
    }
    out << "\n";
}

} // namespace itch
