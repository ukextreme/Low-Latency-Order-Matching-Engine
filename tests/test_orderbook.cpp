#include <gtest/gtest.h>
#include "orderbook.h"

TEST(OrderBookTest, EmptyBook) {
    OrderBook book;
    EXPECT_EQ(book.best_bid(), 0.0);
    EXPECT_EQ(book.best_ask(), 0.0);
    EXPECT_EQ(book.order_count(), 0);
}

TEST(OrderBookTest, SingleLimitOrder) {
    OrderBook book;
    auto trades = book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });

    EXPECT_TRUE(trades.empty()); // No match yet
    EXPECT_EQ(book.best_bid(), 100.0);
    EXPECT_EQ(book.order_count(), 1);
}

TEST(OrderBookTest, ExactMatch) {
    OrderBook book;

    book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });

    auto trades = book.add_order(Order{
        .id = 2, .side = Side::Sell, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 2
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 50);
    EXPECT_EQ(trades[0].price, 100.0);
    EXPECT_EQ(book.order_count(), 0); // Both fully filled
}

TEST(OrderBookTest, PartialFill) {
    OrderBook book;

    book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 100, .filled = 0, .timestamp = 1
    });

    auto trades = book.add_order(Order{
        .id = 2, .side = Side::Sell, .type = OrderType::Limit,
        .price = 100.0, .quantity = 30, .filled = 0, .timestamp = 2
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 30);
    EXPECT_EQ(book.order_count(), 1); // Buy order still has 70 remaining
    EXPECT_EQ(book.best_bid(), 100.0);
}

TEST(OrderBookTest, PriceTimePriority) {
    OrderBook book;

    // Two buy orders at the same price — first one should fill first
    book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });
    book.add_order(Order{
        .id = 2, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 2
    });

    auto trades = book.add_order(Order{
        .id = 3, .side = Side::Sell, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 3
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].buy_order_id, 1); // Order 1 was first
    EXPECT_EQ(book.order_count(), 1); // Order 2 still resting
}

TEST(OrderBookTest, PricePriority) {
    OrderBook book;

    // Lower buy price, added first
    book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 99.0, .quantity = 50, .filled = 0, .timestamp = 1
    });

    // Higher buy price, added second — should match first (better price)
    book.add_order(Order{
        .id = 2, .side = Side::Buy, .type = OrderType::Limit,
        .price = 101.0, .quantity = 50, .filled = 0, .timestamp = 2
    });

    auto trades = book.add_order(Order{
        .id = 3, .side = Side::Sell, .type = OrderType::Limit,
        .price = 99.0, .quantity = 50, .filled = 0, .timestamp = 3
    });

    // Should match order 2 (price 101) because it's the highest bid
    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].buy_order_id, 2);
    EXPECT_EQ(trades[0].price, 101.0);
}

TEST(OrderBookTest, MarketOrder) {
    OrderBook book;

    book.add_order(Order{
        .id = 1, .side = Side::Sell, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });

    // Market buy — should match immediately at best ask
    auto trades = book.add_order(Order{
        .id = 2, .side = Side::Buy, .type = OrderType::Market,
        .price = 0.0, .quantity = 50, .filled = 0, .timestamp = 2
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].price, 100.0);
    EXPECT_EQ(book.order_count(), 0);
}

TEST(OrderBookTest, CancelOrder) {
    OrderBook book;

    book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });

    EXPECT_EQ(book.order_count(), 1);
    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.order_count(), 0);
    EXPECT_EQ(book.best_bid(), 0.0);

    // Cancel non-existent order
    EXPECT_FALSE(book.cancel_order(999));
}

TEST(OrderBookTest, MultiplePriceLevels) {
    OrderBook book;

    book.add_order(Order{
        .id = 1, .side = Side::Buy, .type = OrderType::Limit,
        .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1
    });
    book.add_order(Order{
        .id = 2, .side = Side::Buy, .type = OrderType::Limit,
        .price = 99.0, .quantity = 50, .filled = 0, .timestamp = 2
    });
    book.add_order(Order{
        .id = 3, .side = Side::Sell, .type = OrderType::Limit,
        .price = 101.0, .quantity = 50, .filled = 0, .timestamp = 3
    });
    book.add_order(Order{
        .id = 4, .side = Side::Sell, .type = OrderType::Limit,
        .price = 102.0, .quantity = 50, .filled = 0, .timestamp = 4
    });

    EXPECT_EQ(book.best_bid(), 100.0);
    EXPECT_EQ(book.best_ask(), 101.0);
    EXPECT_DOUBLE_EQ(book.spread(), 1.0);
    EXPECT_EQ(book.order_count(), 4);
}