#include "orderbook.h"

OrderBook::OrderBook() : order_count_(0) {}

std::vector<Trade> OrderBook::add_order(Order order) {
    std::vector<Trade> trades;

    if (order.type == OrderType::Market || order.type == OrderType::Limit) {
        // Try to match against the opposite side
        trades = match_order(order);
    }

    // If the order still has remaining quantity and it's a limit order,
    // add it to the book. Market orders that don't fully fill are dropped.
    if (!order.is_filled() && order.type == OrderType::Limit) {
        double price = order.price;
        uint64_t id = order.id;

        if (order.side == Side::Buy) {
            bids_[price].push_back(std::move(order));
        } else {
            asks_[price].push_back(std::move(order));
        }

        order_prices_[id] = price;
        order_count_++;
    }

    return trades;
}

std::vector<Trade> OrderBook::match_order(Order& incoming) {
    std::vector<Trade> trades;

    if (incoming.side == Side::Buy) {
        // Buy order matches against the sell side (asks)
        // Match while: there are asks, and the incoming price >= best ask
        while (!asks_.empty() && incoming.remaining() > 0) {
            auto& [best_price, orders_at_price] = *asks_.begin();

            // For limit orders, check price compatibility
            // A buy limit at 2500 can match a sell at 2500 or lower
            if (incoming.type == OrderType::Limit && incoming.price < best_price) {
                break; // Can't match — incoming price too low
            }

            // Match against orders at this price level (FIFO — first in, first out)
            while (!orders_at_price.empty() && incoming.remaining() > 0) {
                Order& resting = orders_at_price.front();

                // How many shares can be traded?
                // The minimum of what the buyer wants and what the seller has
                uint32_t fill_qty = std::min(incoming.remaining(), resting.remaining());

                // Record the trade
                trades.push_back(Trade{
                    .buy_order_id = incoming.id,
                    .sell_order_id = resting.id,
                    .price = best_price,
                    .quantity = fill_qty
                });

                // Update filled quantities
                incoming.filled += fill_qty;
                resting.filled += fill_qty;

                // If the resting order is fully filled, remove it
                if (resting.is_filled()) {
                    order_prices_.erase(resting.id);
                    orders_at_price.pop_front();
                    order_count_--;
                }
            }

            // If no orders left at this price level, remove the level
            if (orders_at_price.empty()) {
                asks_.erase(asks_.begin());
            }
        }
    } else {
        // Sell order matches against the buy side (bids)
        // Mirror of the above, but checking bids instead of asks
        while (!bids_.empty() && incoming.remaining() > 0) {
            auto& [best_price, orders_at_price] = *bids_.begin();

            // A sell limit at 2500 can match a buy at 2500 or higher
            if (incoming.type == OrderType::Limit && incoming.price > best_price) {
                break;
            }

            while (!orders_at_price.empty() && incoming.remaining() > 0) {
                Order& resting = orders_at_price.front();

                uint32_t fill_qty = std::min(incoming.remaining(), resting.remaining());

                trades.push_back(Trade{
                    .buy_order_id = resting.id,
                    .sell_order_id = incoming.id,
                    .price = best_price,
                    .quantity = fill_qty
                });

                incoming.filled += fill_qty;
                resting.filled += fill_qty;

                if (resting.is_filled()) {
                    order_prices_.erase(resting.id);
                    orders_at_price.pop_front();
                    order_count_--;
                }
            }

            if (orders_at_price.empty()) {
                bids_.erase(bids_.begin());
            }
        }
    }

    return trades;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    auto it = order_prices_.find(order_id);
    if (it == order_prices_.end()) {
        return false; // Order not found
    }

    double price = it->second;

    // Search the bids side
    auto bid_it = bids_.find(price);
    if (bid_it != bids_.end()) {
        auto& orders = bid_it->second;
        for (auto oit = orders.begin(); oit != orders.end(); ++oit) {
            if (oit->id == order_id) {
                orders.erase(oit);
                if (orders.empty()) {
                    bids_.erase(bid_it);
                }
                order_prices_.erase(it);
                order_count_--;
                return true;
            }
        }
    }

    // Search the asks side
    auto ask_it = asks_.find(price);
    if (ask_it != asks_.end()) {
        auto& orders = ask_it->second;
        for (auto oit = orders.begin(); oit != orders.end(); ++oit) {
            if (oit->id == order_id) {
                orders.erase(oit);
                if (orders.empty()) {
                    asks_.erase(ask_it);
                }
                order_prices_.erase(it);
                order_count_--;
                return true;
            }
        }
    }

    return false;
}

double OrderBook::best_bid() const {
    if (bids_.empty()) return 0.0;
    return bids_.begin()->first;
}

double OrderBook::best_ask() const {
    if (asks_.empty()) return 0.0;
    return asks_.begin()->first;
}

size_t OrderBook::order_count() const {
    return order_count_;
}

double OrderBook::spread() const {
    double bid = best_bid();
    double ask = best_ask();
    if (bid == 0.0 || ask == 0.0) return 0.0;
    return ask - bid;
}