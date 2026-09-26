#pragma once

#include "core/order.h"

#include <cstdint>
#include <unordered_map>

namespace itch {

class Store {
private:
    std::unordered_map<std::uint64_t, Order> orders;
public:
    Store() { orders.reserve(1000000); }

    void add(std::uint64_t orderRef, Order order) { orders[orderRef] = order; }

    Order* find(std::uint64_t orderRef) {
        auto it = orders.find(orderRef);
        return it != orders.end() ? &it->second : nullptr;
    }

    void remove(std::uint64_t orderRef) { orders.erase(orderRef); }

    const std::unordered_map<std::uint64_t, Order>& data() const { return orders; }
    std::size_t size() const { return orders.size(); }
};

} // namespace itch
