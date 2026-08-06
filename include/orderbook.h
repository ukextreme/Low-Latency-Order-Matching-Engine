#pragma once

#include "order.h"
#include <map>
#include <list>
#include <unordered_map>
#include <vector>

// The order book: maintains buy and sell sides,
// matches incoming orders against resting orders.
//
// This is the NAIVE version using std::map.
// M3 will replace the internals with optimized structures.
class OrderBook {
public:
    OrderBook();

    // Add a new order. Returns any trades that resulted.
    std::vector<Trade> add_order(Order order);

    // Cancel an existing order. Returns true if found and cancelled.
    bool cancel_order(uint64_t order_id);

    // Get the best bid (highest buy price). 0 if no bids.
    double best_bid() const;

    // Get the best ask (lowest sell price). 0 if no asks.
    double best_ask() const;

    // Total number of active (unfilled, uncancelled) orders
    size_t order_count() const;

    // Get the current spread (best_ask - best_bid)
    double spread() const;

private:
    // Match an incoming order against the opposite side.
    // Called internally by add_order.
    std::vector<Trade> match_order(Order& order);

    // Buy side: sorted by price DESCENDING (highest first)
    // Key: price, Value: list of orders at that price (time-ordered)
    // std::map sorts ascending by default, so we use std::greater
    // to reverse it — highest price first.
    std::map<double, std::list<Order>, std::greater<double>> bids_;

    // Sell side: sorted by price ASCENDING (lowest first)
    // Default std::map ordering — lowest price first.
    std::map<double, std::list<Order>> asks_;

    // Fast lookup: order_id → pointer to the price level and position
    // Used by cancel_order to find an order without scanning
    std::unordered_map<uint64_t, double> order_prices_;

    // Counter for total active orders
    size_t order_count_;
};