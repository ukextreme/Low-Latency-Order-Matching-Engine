#include <gtest/gtest.h>
#include "orderbook_opt.h"

// ============================================================
// Same tests as the naive version, ensuring the optimized
// book produces identical results.
// ============================================================

TEST(OrderBookOptTest, EmptyBook) {
    OrderBookOpt book(1.0, 200.0, 1.0);
    EXPECT_EQ(book.best_bid(), 0.0);
    EXPECT_EQ(book.best_ask(), 0.0);
    EXPECT_EQ(book.order_count(), 0);
}

TEST(OrderBookOptTest, SingleLimitOrder) {
    OrderBookOpt book(1.0, 200.0, 1.0);
    auto trades = book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), 100.0);
    EXPECT_EQ(book.order_count(), 1);
}

TEST(OrderBookOptTest, ExactMatch) {
    OrderBookOpt book(1.0, 200.0, 1.0);

    book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });

    auto trades = book.add_order(AlignedOrder{
        .id = 2, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Sell, .type = OrderType::Limit, .active = true,
        .timestamp = 2
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 50);
    EXPECT_EQ(trades[0].price, 100.0);
    EXPECT_EQ(book.order_count(), 0);
}

TEST(OrderBookOptTest, PartialFill) {
    OrderBookOpt book(1.0, 200.0, 1.0);

    book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 100, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });

    auto trades = book.add_order(AlignedOrder{
        .id = 2, .price = 100.0, .quantity = 30, .filled = 0,
        .side = Side::Sell, .type = OrderType::Limit, .active = true,
        .timestamp = 2
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 30);
    EXPECT_EQ(book.order_count(), 1);
}

TEST(OrderBookOptTest, PriceTimePriority) {
    OrderBookOpt book(1.0, 200.0, 1.0);

    book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });
    book.add_order(AlignedOrder{
        .id = 2, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 2
    });

    auto trades = book.add_order(AlignedOrder{
        .id = 3, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Sell, .type = OrderType::Limit, .active = true,
        .timestamp = 3
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].buy_order_id, 1);
    EXPECT_EQ(book.order_count(), 1);
}

TEST(OrderBookOptTest, CancelOrder) {
    OrderBookOpt book(1.0, 200.0, 1.0);

    book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });

    EXPECT_EQ(book.order_count(), 1);
    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.order_count(), 0);
    EXPECT_FALSE(book.cancel_order(999));
}

TEST(OrderBookOptTest, Spread) {
    OrderBookOpt book(1.0, 200.0, 1.0);

    book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });
    book.add_order(AlignedOrder{
        .id = 2, .price = 102.0, .quantity = 50, .filled = 0,
        .side = Side::Sell, .type = OrderType::Limit, .active = true,
        .timestamp = 2
    });

    EXPECT_EQ(book.best_bid(), 100.0);
    EXPECT_EQ(book.best_ask(), 102.0);
    EXPECT_DOUBLE_EQ(book.spread(), 2.0);
}

TEST(OrderBookOptTest, MarketOrder) {
    OrderBookOpt book(1.0, 200.0, 1.0);

    book.add_order(AlignedOrder{
        .id = 1, .price = 100.0, .quantity = 50, .filled = 0,
        .side = Side::Sell, .type = OrderType::Limit, .active = true,
        .timestamp = 1
    });

    auto trades = book.add_order(AlignedOrder{
        .id = 2, .price = 0.0, .quantity = 50, .filled = 0,
        .side = Side::Buy, .type = OrderType::Market, .active = true,
        .timestamp = 2
    });

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].price, 100.0);
    EXPECT_EQ(book.order_count(), 0);
}