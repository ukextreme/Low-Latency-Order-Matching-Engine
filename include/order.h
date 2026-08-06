#pragma once

#include <cstdint>
#include <chrono>

// Side of the order: buy or sell
enum class Side : uint8_t {
    Buy,
    Sell
};

// Type of order
enum class OrderType : uint8_t {
    Limit,   // Execute at specified price or better
    Market   // Execute immediately at best available price
};

// A single order in the order book
struct Order {
    uint64_t id;          // Unique order ID
    Side side;            // Buy or Sell
    OrderType type;       // Limit or Market
    double price;         // Limit price (ignored for Market orders)
    uint32_t quantity;    // Number of shares
    uint32_t filled;      // Number of shares already filled
    uint64_t timestamp;   // Nanosecond timestamp for time priority

    // How many shares are still waiting to be filled
    uint32_t remaining() const {
        return quantity - filled;
    }

    // Is this order completely filled?
    bool is_filled() const {
        return filled >= quantity;
    }
};

// A trade that occurred when two orders matched
struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    double price;
    uint32_t quantity;
};