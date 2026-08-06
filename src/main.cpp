#include "orderbook.h"
#include <iostream>

int main() {
    OrderBook book;

    std::cout << "Matching engine starting..." << std::endl;

    // Quick sanity check
    auto trades = book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });

    trades = book.add_order(Order{
        .id = 2, .side = Side::Sell, .type = OrderType::Limit,
        .price = 100.0, .quantity = 30, .filled = 0, .timestamp = 2
    });

    std::cout << "Trades executed: " << trades.size() << std::endl;
    if (!trades.empty()) {
        std::cout << "  Price: " << trades[0].price
                  << " Qty: " << trades[0].quantity << std::endl;
    }

    std::cout << "Best bid: " << book.best_bid() << std::endl;
    std::cout << "Best ask: " << book.best_ask() << std::endl;
    std::cout << "Orders in book: " << book.order_count() << std::endl;

    return 0;
}