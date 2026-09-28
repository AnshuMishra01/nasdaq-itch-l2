#pragma once

#include "core/message.h"

#include <cstdint>

namespace itch {

// Parse + decode + discard: the floor no book builder can beat. Every decoded field is
// folded into a checksum (printed by the caller) so the optimiser cannot delete the decode.
struct DiscardHandler {
    std::uint64_t sum = 0;
    void on(const StockDirectoryMessage& m) { sum += m.stockLocate; }
    void on(const AddOrderMessage& m) { sum += m.orderRef ^ m.price ^ m.shares ^ m.stockLocate ^ m.timeStamp ^ static_cast<std::uint8_t>(m.side); }
    void on(const AddOrderMPIDMessage& m) { sum += m.orderRef ^ m.price ^ m.shares ^ m.stockLocate ^ m.timeStamp ^ static_cast<std::uint8_t>(m.side); }
    void on(const OrderExecutedMessage& m) { sum += m.orderRef ^ m.executedShares ^ m.matchNumber ^ m.timeStamp; }
    void on(const OrderExecutedWithPriceMessage& m) { sum += m.orderRef ^ m.executedShares ^ m.price ^ m.matchNumber ^ m.timeStamp; }
    void on(const OrderCancelMessage& m) { sum += m.orderRef ^ m.cancelledShares ^ m.timeStamp; }
    void on(const OrderDeleteMessage& m) { sum += m.orderRef ^ m.timeStamp; }
    void on(const OrderReplaceMessage& m) { sum += m.origRef ^ m.newRef ^ m.price ^ m.shares ^ m.timeStamp; }
};

} // namespace itch
