#pragma once

#include <array>
#include <cstdint>

enum class Side : std::uint8_t {
    Buy = 'B',
    Sell = 'S'
};

struct StockDirectoryMessage {
    std::uint16_t stockLocate;
    std::array<char, 8> symbol;
};

struct AddOrderMessage {
    Side side;
    std::uint16_t stockLocate;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
};

struct AddOrderMPIDMessage {
    Side side;
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
    std::array<char, 4> attribution;
};

struct OrderExecutedMessage {
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t executedShares;
    std::uint64_t matchNumber;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
};

struct OrderExecutedWithPriceMessage {
    char printable;
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t price;
    std::uint32_t executedShares;
    std::uint64_t matchNumber;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
};

struct OrderCancelMessage {
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t cancelledShares;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
};

struct OrderDeleteMessage {
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
};

struct OrderReplaceMessage {
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint64_t timeStamp;
    std::uint64_t origRef;
    std::uint64_t newRef;
};

// 'P': execution against a non-displayed order. Never touches the book.
struct TradeMessage {
    Side side;
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint64_t timeStamp;
    std::uint64_t orderRef;
    std::uint64_t matchNumber;
    std::array<char, 8> symbol;
};

// 'Q': cross (auction) result. No order refs; crossType 'O' open, 'C' close, 'H' halt/IPO, 'I' intraday.
struct CrossTradeMessage {
    char crossType;
    std::uint16_t stockLocate;
    std::uint16_t trackingNumber;
    std::uint32_t crossPrice;
    std::uint64_t shares;
    std::uint64_t timeStamp;
    std::uint64_t matchNumber;
    std::array<char, 8> symbol;
};
