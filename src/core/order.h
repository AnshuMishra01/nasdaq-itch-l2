#pragma once
#include <cstdint>
#include "core/message.h"

struct Order{
    std::uint32_t price;
    std::uint32_t shares;
    std::uint16_t locate;
    Side side;
};

static_assert(sizeof(Order) == 12, "Order struct size must be 12 bytes");
