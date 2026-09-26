#pragma once

#include "core/message.h"

#include <fstream>
#include <string>

inline void dump_struct_sizes(const std::string& outpath = "struct.txt") {
    std::ofstream out(outpath);
    if (!out) return;
    out << "StockDirectoryMessage " << sizeof(StockDirectoryMessage) << '\n';
    out << "AddOrderMessage " << sizeof(AddOrderMessage) << '\n';
    out << "AddOrderMPIDMessage " << sizeof(AddOrderMPIDMessage) << '\n';
    out << "OrderExecutedMessage " << sizeof(OrderExecutedMessage) << '\n';
    out << "OrderExecutedWithPriceMessage " << sizeof(OrderExecutedWithPriceMessage) << '\n';
    out << "OrderCancelMessage " << sizeof(OrderCancelMessage) << '\n';
    out << "OrderDeleteMessage " << sizeof(OrderDeleteMessage) << '\n';
    out << "OrderReplaceMessage " << sizeof(OrderReplaceMessage) << '\n';
    out.close();
}
