#pragma once

#include "handlers/book_handler.h"
#include "measure.h"

namespace itch {

struct CombinedHandler {
    StatsHandler stats;
    BookHandler book;

    void on(const StockDirectoryMessage& m) { stats.on(m); book.on(m); }
    void on(const AddOrderMessage& m) { stats.on(m); book.on(m); }
    void on(const AddOrderMPIDMessage& m) { stats.on(m); book.on(m); }
    void on(const OrderExecutedMessage& m) { stats.on(m); book.on(m); }
    void on(const OrderExecutedWithPriceMessage& m) { stats.on(m); book.on(m); }
    void on(const OrderCancelMessage& m) { stats.on(m); book.on(m); }
    void on(const OrderDeleteMessage& m) { stats.on(m); book.on(m); }
    void on(const OrderReplaceMessage& m) { stats.on(m); book.on(m); }
};

} // namespace itch
