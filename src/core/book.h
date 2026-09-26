#pragma once

#include "core/message.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace itch {

struct PriceLevel {
    std::uint32_t price;
    std::uint32_t shares;
    std::uint32_t order_count;
};

static_assert(sizeof(PriceLevel) == 12, "PriceLevel must be 12 bytes");

enum class ReduceResult : std::uint8_t {
    Ok,           // shares taken off the level (level erased if it hit zero)
    MissingLevel, // no level at that price: book and order store disagree
    Underflow,    // removing more shares than the level holds: a bug upstream
};

struct Book {
    // Both sides keep the best price at back():
    //   bids ascending  (best = highest)
    //   asks descending (best = lowest)
    // Measured: 50% of updates hit the best level, 92% are within 3 of it,
    // so every lookup scans backwards from back() instead of binary searching.
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;

    std::vector<PriceLevel>& levels(Side side) { return side == Side::Buy ? bids : asks; }
    const std::vector<PriceLevel>& levels(Side side) const { return side == Side::Buy ? bids : asks; }

    // true if price a sits closer to the top of the book than price b
    static bool better(Side side, std::uint32_t a, std::uint32_t b) {
        return side == Side::Buy ? a > b : a < b;
    }

    // Index one past where `price` lives or belongs: every level at or above
    // it is strictly better. Found iff i > 0 && lv[i - 1].price == price.
    static std::size_t scan(const std::vector<PriceLevel>& lv, Side side, std::uint32_t price) {
        std::size_t i = lv.size();
        while (i > 0 && better(side, lv[i - 1].price, price)) --i;
        return i;
    }

    // A new order joins the level at `price`.
    void add(Side side, std::uint32_t price, std::uint32_t shares) {
        auto& lv = levels(side);
        const std::size_t i = scan(lv, side, price);
        if (i > 0 && lv[i - 1].price == price) {
            lv[i - 1].shares += shares;
            lv[i - 1].order_count++;
        } else {
            lv.insert(lv.begin() + static_cast<std::ptrdiff_t>(i), PriceLevel{price, shares, 1});
        }
    }

    // Take shares off the level at `price`; order_gone means the order left the book.
    ReduceResult reduce(Side side, std::uint32_t price, std::uint32_t shares, bool order_gone) {
        auto& lv = levels(side);
        const std::size_t i = scan(lv, side, price);
        if (i == 0 || lv[i - 1].price != price) return ReduceResult::MissingLevel;

        PriceLevel& level = lv[i - 1];
        if (shares > level.shares) {
            lv.erase(lv.begin() + static_cast<std::ptrdiff_t>(i - 1));
            return ReduceResult::Underflow;
        }
        level.shares -= shares;
        if (order_gone) level.order_count--;
        if (level.shares == 0) lv.erase(lv.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return ReduceResult::Ok;
    }

    // Locked (==) counts as crossed: Nasdaq's own book should never be either.
    bool crossed() const {
        return !bids.empty() && !asks.empty() && bids.back().price >= asks.back().price;
    }

    // Top n levels, best first.
    std::vector<PriceLevel> top_levels(Side side, std::size_t n = 5) const {
        const auto& lv = levels(side);
        std::vector<PriceLevel> out;
        for (std::size_t k = 0; k < n && k < lv.size(); ++k) out.push_back(lv[lv.size() - 1 - k]);
        return out;
    }
};

} // namespace itch
