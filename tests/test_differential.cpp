#include <gtest/gtest.h>
#include "orderbook.h"
#include "orderbook_opt.h"

#include <random>
#include <sstream>
#include <string>
#include <vector>

// ============================================================
// DIFFERENTIAL TESTS
//
// The per-book suites assert against hardcoded expected values,
// so the two books are only "the same" by hand. These tests drive
// ONE identical operation stream through both and assert they
// agree fill-for-fill. Any divergence is a bug in the optimized
// path that value-based unit tests cannot see.
// ============================================================

namespace {

// The optimized book needs a bounded price grid. Every price the
// generators produce must land inside it.
constexpr double kMinPrice = 1.0;
constexpr double kMaxPrice = 300.0;
constexpr double kTick = 1.0;

AlignedOrder to_aligned(const Order& o) {
    return AlignedOrder{
        .id = o.id,
        .price = o.price,
        .quantity = o.quantity,
        .filled = o.filled,
        .side = o.side,
        .type = o.type,
        .active = true,
        .timestamp = o.timestamp,
    };
}

std::string describe(const Trade& t) {
    std::ostringstream os;
    os << "{buy=" << t.buy_order_id << " sell=" << t.sell_order_id
       << " px=" << t.price << " qty=" << t.quantity << "}";
    return os.str();
}

std::string describe(const std::vector<Trade>& ts) {
    if (ts.empty()) return "[]";
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < ts.size(); i++) {
        if (i) os << ", ";
        os << describe(ts[i]);
    }
    os << "]";
    return os.str();
}

// Drives both books in lockstep and records the FIRST divergence,
// so a failure names the exact step that broke rather than dumping
// every downstream mismatch.
class DifferentialBooks {
public:
    DifferentialBooks() : opt_(kMinPrice, kMaxPrice, kTick) {}

    bool ok() const { return divergence_.empty(); }
    const std::string& report() const { return divergence_; }

    // Coverage counters. A differential test that compares nothing
    // passes just as loudly as one that compares everything, so the
    // tests assert on these too.
    size_t trades_compared() const { return trades_compared_; }
    size_t fills_matched() const { return fills_matched_; }

    void submit(const Order& o, int step) {
        auto naive_trades = naive_.add_order(o);
        auto opt_trades = opt_.add_order(to_aligned(o));
        compare_trades(naive_trades, opt_trades, step, o);
    }

    void cancel(uint64_t id, int step) {
        bool naive_ok = naive_.cancel_order(id);
        bool opt_ok = opt_.cancel_order(id);
        if (naive_ok != opt_ok) {
            std::ostringstream os;
            os << "cancel(" << id << ") disagreed: naive returned "
               << std::boolalpha << naive_ok << ", opt returned " << opt_ok;
            note(step, os.str());
        }
    }

    // Book-level state, checked separately from the trade stream so a
    // failure says which of the two diverged.
    void compare_state(int step) {
        if (naive_.best_bid() != opt_.best_bid()) {
            std::ostringstream os;
            os << "best_bid diverged: naive=" << naive_.best_bid()
               << " opt=" << opt_.best_bid();
            note(step, os.str());
        }
        if (naive_.best_ask() != opt_.best_ask()) {
            std::ostringstream os;
            os << "best_ask diverged: naive=" << naive_.best_ask()
               << " opt=" << opt_.best_ask();
            note(step, os.str());
        }
        if (naive_.order_count() != opt_.order_count()) {
            std::ostringstream os;
            os << "order_count diverged: naive=" << naive_.order_count()
               << " opt=" << opt_.order_count();
            note(step, os.str());
        }
    }

private:
    void note(int step, const std::string& detail) {
        if (!divergence_.empty()) return;  // keep only the first
        std::ostringstream os;
        os << "step " << step << ": " << detail;
        divergence_ = os.str();
    }

    void compare_trades(const std::vector<Trade>& naive_trades,
                        const std::vector<Trade>& opt_trades,
                        int step,
                        const Order& o) {
        std::ostringstream ctx;
        ctx << "order{id=" << o.id
            << " side=" << (o.side == Side::Buy ? "Buy" : "Sell")
            << " type=" << (o.type == OrderType::Limit ? "Limit" : "Market")
            << " px=" << o.price << " qty=" << o.quantity << "} -> ";

        if (naive_trades.size() != opt_trades.size()) {
            std::ostringstream os;
            os << ctx.str() << "trade COUNT diverged: naive produced "
               << naive_trades.size() << " " << describe(naive_trades)
               << ", opt produced " << opt_trades.size() << " "
               << describe(opt_trades);
            note(step, os.str());
            return;
        }

        trades_compared_++;
        for (size_t i = 0; i < naive_trades.size(); i++) {
            fills_matched_++;
            const Trade& a = naive_trades[i];
            const Trade& b = opt_trades[i];
            if (a.buy_order_id != b.buy_order_id ||
                a.sell_order_id != b.sell_order_id ||
                a.price != b.price ||
                a.quantity != b.quantity) {
                std::ostringstream os;
                os << ctx.str() << "trade[" << i << "] diverged: naive="
                   << describe(a) << " opt=" << describe(b);
                note(step, os.str());
                return;
            }
        }
    }

    OrderBook naive_;
    OrderBookOpt opt_;
    std::string divergence_;
    size_t trades_compared_ = 0;
    size_t fills_matched_ = 0;
};

}  // namespace

// ------------------------------------------------------------
// A randomized stream of crossing limit orders. The price band is
// deliberately narrow so most orders match rather than rest.
// ------------------------------------------------------------
TEST(DifferentialTest, LimitOrderStreamProducesIdenticalTrades) {
    DifferentialBooks books;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 110);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    for (int step = 1; step <= 2000; step++) {
        Order o{
            .id = static_cast<uint64_t>(step),
            .side = side_dist(rng) ? Side::Buy : Side::Sell,
            .type = OrderType::Limit,
            .price = static_cast<double>(price_dist(rng)),
            .quantity = static_cast<uint32_t>(qty_dist(rng)),
            .filled = 0,
            .timestamp = static_cast<uint64_t>(step),
        };
        books.submit(o, step);
        if (!books.ok()) break;
    }

    EXPECT_TRUE(books.ok()) << books.report();
    EXPECT_GT(books.fills_matched(), 500u)
        << "stream produced too few fills to be a meaningful comparison";
}

// ------------------------------------------------------------
// Same idea, but cancels are interleaved so the books must agree
// on which resting orders are still matchable.
// ------------------------------------------------------------
TEST(DifferentialTest, InterleavedCancelsProduceIdenticalTrades) {
    DifferentialBooks books;
    std::mt19937 rng(999);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(98, 106);
    std::uniform_int_distribution<int> qty_dist(1, 50);

    for (int step = 1; step <= 2000; step++) {
        Order o{
            .id = static_cast<uint64_t>(step),
            .side = side_dist(rng) ? Side::Buy : Side::Sell,
            .type = OrderType::Limit,
            .price = static_cast<double>(price_dist(rng)),
            .quantity = static_cast<uint32_t>(qty_dist(rng)),
            .filled = 0,
            .timestamp = static_cast<uint64_t>(step),
        };
        books.submit(o, step);
        if (!books.ok()) break;

        // Cancel an order from a few steps back — often already
        // filled, which exercises the "cancel a dead order" path.
        if (step % 3 == 0 && step > 10) {
            books.cancel(static_cast<uint64_t>(step - 7), step);
            if (!books.ok()) break;
        }
    }

    EXPECT_TRUE(books.ok()) << books.report();
    EXPECT_GT(books.fills_matched(), 500u)
        << "stream produced too few fills to be a meaningful comparison";
}

// ------------------------------------------------------------
// Market orders sweeping resting liquidity across several levels.
// ------------------------------------------------------------
TEST(DifferentialTest, MarketOrderSweepsProduceIdenticalTrades) {
    DifferentialBooks books;
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> price_dist(100, 108);
    std::uniform_int_distribution<int> qty_dist(1, 40);

    uint64_t id = 0;
    for (int round = 1; round <= 300; round++) {
        // Rest liquidity on both sides.
        for (int i = 0; i < 4; i++) {
            Order sell{
                .id = ++id,
                .side = Side::Sell,
                .type = OrderType::Limit,
                .price = static_cast<double>(price_dist(rng)),
                .quantity = static_cast<uint32_t>(qty_dist(rng)),
                .filled = 0,
                .timestamp = id,
            };
            books.submit(sell, round);
            if (!books.ok()) return;
        }

        // Sweep it with a market buy large enough to cross levels.
        Order mkt{
            .id = ++id,
            .side = Side::Buy,
            .type = OrderType::Market,
            .price = 0.0,
            .quantity = 90,
            .filled = 0,
            .timestamp = id,
        };
        books.submit(mkt, round);
        if (!books.ok()) return;
    }

    EXPECT_TRUE(books.ok()) << books.report();
    EXPECT_GT(books.fills_matched(), 500u)
        << "sweeps produced too few fills to be a meaningful comparison";
}

// ------------------------------------------------------------
// Public book state (best quotes and active count) must also agree,
// checked after every operation rather than only at the end.
// ------------------------------------------------------------
TEST(DifferentialTest, BookStateStaysIdentical) {
    DifferentialBooks books;
    std::mt19937 rng(777);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 112);
    std::uniform_int_distribution<int> qty_dist(1, 60);

    for (int step = 1; step <= 1500; step++) {
        Order o{
            .id = static_cast<uint64_t>(step),
            .side = side_dist(rng) ? Side::Buy : Side::Sell,
            .type = OrderType::Limit,
            .price = static_cast<double>(price_dist(rng)),
            .quantity = static_cast<uint32_t>(qty_dist(rng)),
            .filled = 0,
            .timestamp = static_cast<uint64_t>(step),
        };
        books.submit(o, step);
        books.compare_state(step);
        if (!books.ok()) break;
    }

    EXPECT_TRUE(books.ok()) << books.report();
    EXPECT_GT(books.trades_compared(), 1000u)
        << "state comparison ran over too few operations";
}
