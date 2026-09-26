#pragma once

#include "decode.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <array>

namespace itch {

inline std::string_view read_symbol(const unsigned char* bytes, std::size_t len) {
    std::size_t i = 0;
    while (i < len && bytes[i] != ' ') ++i;
    return std::string_view(reinterpret_cast<const char*>(bytes), i);
}

inline std::chrono::nanoseconds convert_timestamp(const unsigned char* bytes) {
    const std::uint64_t timestampNs = read_be<std::uint64_t, 6>(bytes);
    return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(timestampNs)};
}

struct PrintHandler {
    std::vector<std::array<char, 8>> symbols;

    PrintHandler() : symbols(65536) {}

    std::string_view lookup(std::uint16_t locate) const {
        const auto& entry = symbols.at(locate);
        std::size_t len = 0;
        while (len < entry.size() && entry[len] != ' ') ++len;
        return std::string_view(entry.data(), len);
    }

    void on(const StockDirectoryMessage& message) {
        std::copy_n(message.symbol.begin(), message.symbol.size(), symbols[message.stockLocate].begin());
    }

    void on(const AddOrderMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const std::uint32_t whole = message.price / 10000U;
        const std::uint32_t frac = message.price % 10000U;
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }

    void on(const AddOrderMPIDMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const std::uint32_t whole = message.price / 10000U;
        const std::uint32_t frac = message.price % 10000U;
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }

    void on(const OrderExecutedMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }

    void on(const OrderExecutedWithPriceMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const std::uint32_t whole = message.price / 10000U;
        const std::uint32_t frac = message.price % 10000U;
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }

    void on(const OrderCancelMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }

    void on(const OrderDeleteMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }

    void on(const OrderReplaceMessage& message) {
        const auto symbol = lookup(message.stockLocate);
        const std::uint32_t whole = message.price / 10000U;
        const std::uint32_t frac = message.price % 10000U;
        const auto hms = std::chrono::hh_mm_ss{std::chrono::nanoseconds{message.timeStamp}};

        // Removed printing logic to avoid I/O includes
    }
    
};

} // namespace itch
