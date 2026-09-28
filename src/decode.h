#ifndef DECODE_H
#define DECODE_H

#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <utility>
#include "core/message.h"

template <std::unsigned_integral T, std::size_t Count = sizeof(T)>
T read_be(const unsigned char* bytes) {
    static_assert(Count <= sizeof(T), "Count must not exceed the size of T");

    T value = 0;
    unsigned char temp[sizeof(T)] = {};
    std::memcpy(temp + (sizeof(T) - Count), bytes, Count);
    std::memcpy(&value, temp, sizeof(T));

    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    }
    return value;
}

[[nodiscard]] inline StockDirectoryMessage decode_stock_directory(const unsigned char* msg) {
    StockDirectoryMessage dir{};
    dir.stockLocate = read_be<std::uint16_t>(msg + 1);
    std::memcpy(dir.symbol.data(), msg + 11, 8);
    return dir;
}

[[nodiscard]] inline AddOrderMessage decode_add_order(const unsigned char* msg) {
    AddOrderMessage order{};
    order.stockLocate = read_be<std::uint16_t>(msg + 1);
    order.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    order.orderRef = read_be<std::uint64_t>(msg + 11);
    order.side = static_cast<Side>(msg[19]);
    order.shares = read_be<std::uint32_t>(msg + 20);
    order.price = read_be<std::uint32_t>(msg + 32);
    return order;
}

[[nodiscard]] inline AddOrderMPIDMessage decode_add_order_mpid(const unsigned char* msg) {
    AddOrderMPIDMessage order{};
    order.stockLocate = read_be<std::uint16_t>(msg + 1);
    order.trackingNumber = read_be<std::uint16_t>(msg + 3);
    order.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    order.orderRef = read_be<std::uint64_t>(msg + 11);
    order.side = static_cast<Side>(msg[19]);
    order.shares = read_be<std::uint32_t>(msg + 20);
    order.price = read_be<std::uint32_t>(msg + 32);
    std::memcpy(order.attribution.data(), msg + 36, 4);
    return order;
}

[[nodiscard]] inline OrderExecutedMessage decode_order_executed(const unsigned char* msg) {
    OrderExecutedMessage executed{};
    executed.stockLocate = read_be<std::uint16_t>(msg + 1);
    executed.trackingNumber = read_be<std::uint16_t>(msg + 3);
    executed.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    executed.orderRef = read_be<std::uint64_t>(msg + 11);
    executed.executedShares = read_be<std::uint32_t>(msg + 19);
    executed.matchNumber = read_be<std::uint64_t>(msg + 23);
    return executed;
}

[[nodiscard]] inline OrderExecutedWithPriceMessage decode_order_executed_with_price(const unsigned char* msg) {
    OrderExecutedWithPriceMessage executed{};
    executed.stockLocate = read_be<std::uint16_t>(msg + 1);
    executed.trackingNumber = read_be<std::uint16_t>(msg + 3);
    executed.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    executed.orderRef = read_be<std::uint64_t>(msg + 11);
    executed.executedShares = read_be<std::uint32_t>(msg + 19);
    executed.matchNumber = read_be<std::uint64_t>(msg + 23);
    executed.printable = static_cast<char>(msg[31]);
    executed.price = read_be<std::uint32_t>(msg + 32);
    return executed;
}

[[nodiscard]] inline OrderCancelMessage decode_order_cancel(const unsigned char* msg) {
    OrderCancelMessage cancel{};
    cancel.stockLocate = read_be<std::uint16_t>(msg + 1);
    cancel.trackingNumber = read_be<std::uint16_t>(msg + 3);
    cancel.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    cancel.orderRef = read_be<std::uint64_t>(msg + 11);
    cancel.cancelledShares = read_be<std::uint32_t>(msg + 19);
    return cancel;
}

[[nodiscard]] inline OrderDeleteMessage decode_order_delete(const unsigned char* msg) {
    OrderDeleteMessage del{};
    del.stockLocate = read_be<std::uint16_t>(msg + 1);
    del.trackingNumber = read_be<std::uint16_t>(msg + 3);
    del.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    del.orderRef = read_be<std::uint64_t>(msg + 11);
    return del;
}

[[nodiscard]] inline OrderReplaceMessage decode_order_replace(const unsigned char* msg) {
    OrderReplaceMessage replace{};
    replace.stockLocate = read_be<std::uint16_t>(msg + 1);
    replace.trackingNumber = read_be<std::uint16_t>(msg + 3);
    replace.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    replace.origRef = read_be<std::uint64_t>(msg + 11);
    replace.newRef = read_be<std::uint64_t>(msg + 19);
    replace.shares = read_be<std::uint32_t>(msg + 27);
    replace.price = read_be<std::uint32_t>(msg + 31);
    return replace;
}

[[nodiscard]] inline TradeMessage decode_trade(const unsigned char* msg) {
    TradeMessage trade{};
    trade.stockLocate = read_be<std::uint16_t>(msg + 1);
    trade.trackingNumber = read_be<std::uint16_t>(msg + 3);
    trade.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    trade.orderRef = read_be<std::uint64_t>(msg + 11);
    trade.side = static_cast<Side>(msg[19]);
    trade.shares = read_be<std::uint32_t>(msg + 20);
    std::memcpy(trade.symbol.data(), msg + 24, 8);
    trade.price = read_be<std::uint32_t>(msg + 32);
    trade.matchNumber = read_be<std::uint64_t>(msg + 36);
    return trade;
}

[[nodiscard]] inline CrossTradeMessage decode_cross_trade(const unsigned char* msg) {
    CrossTradeMessage cross{};
    cross.stockLocate = read_be<std::uint16_t>(msg + 1);
    cross.trackingNumber = read_be<std::uint16_t>(msg + 3);
    cross.timeStamp = read_be<std::uint64_t, 6>(msg + 5);
    cross.shares = read_be<std::uint64_t>(msg + 11);
    std::memcpy(cross.symbol.data(), msg + 19, 8);
    cross.crossPrice = read_be<std::uint32_t>(msg + 27);
    cross.matchNumber = read_be<std::uint64_t>(msg + 31);
    cross.crossType = static_cast<char>(msg[39]);
    return cross;
}

namespace itch {

// P and Q are decoded only for handlers that have an on() for them, so handlers that
// ignore trades pay nothing. They return false: they are not book events, and keeping
// them out of the count keeps ns/msg comparable with earlier runs.
template <typename Handler>
inline bool dispatch(const unsigned char type, const unsigned char* message, Handler& h) {
    switch (type) {
        case 'P':
            if constexpr (requires { h.on(std::declval<const TradeMessage&>()); }) h.on(decode_trade(message));
            return false;
        case 'Q':
            if constexpr (requires { h.on(std::declval<const CrossTradeMessage&>()); }) h.on(decode_cross_trade(message));
            return false;
        case 'R': h.on(decode_stock_directory(message)); return false;
        case 'A': h.on(decode_add_order(message)); return true;
        case 'F': h.on(decode_add_order_mpid(message)); return true;
        case 'E': h.on(decode_order_executed(message)); return true;
        case 'C': h.on(decode_order_executed_with_price(message)); return true;
        case 'X': h.on(decode_order_cancel(message)); return true;
        case 'D': h.on(decode_order_delete(message)); return true;
        case 'U': h.on(decode_order_replace(message)); return true;
        default: return false;
    }
    return true;
}

} // namespace itch

#endif

