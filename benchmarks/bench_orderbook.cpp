#include <benchmark/benchmark.h>
#include "orderbook.h"
#include <random>

// ============================================================
// HELPER: Generate a random order for benchmarking.
//
// We use random prices and sides to simulate realistic conditions.
// If every order were identical, the benchmark would only measure
// one code path (e.g., always matching, or never matching).
// Random orders exercise both matching and resting paths.
// ============================================================
static Order make_random_order(uint64_t id, std::mt19937& rng) {
    // std::uniform_int_distribution picks a random integer in a range.
    // We use it to randomly choose buy/sell and to generate prices.
    std::uniform_int_distribution<int> side_dist(0, 1);
    
    // Prices between 990 and 1010 — a tight range around 1000.
    // This ensures frequent matching (buy at 1005 meets sell at 995)
    // while still having some orders that rest in the book.
    std::uniform_int_distribution<int> price_dist(990, 1010);
    
    // Quantities between 1 and 100 shares
    std::uniform_int_distribution<int> qty_dist(1, 100);

    return Order{
        .id = id,
        .side = side_dist(rng) == 0 ? Side::Buy : Side::Sell,
        .type = OrderType::Limit,
        .price = static_cast<double>(price_dist(rng)),
        .quantity = static_cast<uint32_t>(qty_dist(rng)),
        .filled = 0,
        .timestamp = id  // Use id as timestamp for simplicity
    };
}

// ============================================================
// BENCHMARK 1: Adding orders to an empty book (no matching).
//
// All orders are on the same side, so nothing ever matches.
// This measures pure insertion speed — how fast can we add
// orders to the price-level data structure?
// ============================================================
static void BM_AddOrderNoMatch(benchmark::State& state) {
    // benchmark::State controls the iteration loop.
    // Google Benchmark decides how many iterations to run
    // to get statistically stable results. Each pass through
    // the for loop is one "iteration" — one measured operation.
    
    OrderBook book;
    uint64_t id = 0;

    // The loop runs millions of times. Each iteration adds one order.
    // Google Benchmark measures the total time and divides by
    // iteration count to get per-operation time.
    for (auto _ : state) {
        book.add_order(Order{
            .id = ++id,
            .side = Side::Buy,  // All buys — never matches
            .type = OrderType::Limit,
            .price = 100.0 + (id % 100),  // Spread across 100 price levels
            .quantity = 50,
            .filled = 0,
            .timestamp = id
        });
    }

    // Report how many orders were processed as a custom counter.
    // This appears in the output as "orders_processed/sec".
    state.counters["orders"] = benchmark::Counter(
        static_cast<double>(id),
        benchmark::Counter::kIsRate  // Display as rate (per second)
    );
}
// Register the benchmark so Google Benchmark knows to run it.
BENCHMARK(BM_AddOrderNoMatch);

// ============================================================
// BENCHMARK 2: Adding orders with matching.
//
// Random buy and sell orders around a central price.
// Some orders match immediately, some rest in the book.
// This measures the realistic hot path — matching + insertion.
// ============================================================
static void BM_AddOrderWithMatching(benchmark::State& state) {
    OrderBook book;
    uint64_t id = 0;
    
    // std::mt19937 is the Mersenne Twister random number generator.
    // Seeded with 42 for reproducibility — every benchmark run
    // generates the same sequence of orders, so results are
    // comparable across runs.
    std::mt19937 rng(42);

    for (auto _ : state) {
        Order order = make_random_order(++id, rng);
        book.add_order(order);
    }

    state.counters["orders"] = benchmark::Counter(
        static_cast<double>(id),
        benchmark::Counter::kIsRate
    );
}
BENCHMARK(BM_AddOrderWithMatching);

// ============================================================
// BENCHMARK 3: Matching against a pre-filled book.
//
// First fills the book with 10,000 resting orders (not timed),
// then measures how fast incoming orders match against them.
// This simulates a realistic scenario: the book isn't empty,
// it has depth (many resting orders at various price levels).
// ============================================================
static void BM_MatchAgainstDeepBook(benchmark::State& state) {
    OrderBook book;
    std::mt19937 rng(42);
    uint64_t id = 0;

    // Pre-fill the book with 10,000 orders.
    // Half buys at prices 950-999, half sells at prices 1001-1050.
    // This creates a spread with no immediate matches — the book
    // has depth on both sides.
    for (int i = 0; i < 5000; i++) {
        std::uniform_int_distribution<int> buy_price(950, 999);
        std::uniform_int_distribution<int> qty(1, 100);
        book.add_order(Order{
            .id = ++id,
            .side = Side::Buy,
            .type = OrderType::Limit,
            .price = static_cast<double>(buy_price(rng)),
            .quantity = static_cast<uint32_t>(qty(rng)),
            .filled = 0,
            .timestamp = id
        });
    }
    for (int i = 0; i < 5000; i++) {
        std::uniform_int_distribution<int> sell_price(1001, 1050);
        std::uniform_int_distribution<int> qty(1, 100);
        book.add_order(Order{
            .id = ++id,
            .side = Side::Sell,
            .type = OrderType::Limit,
            .price = static_cast<double>(sell_price(rng)),
            .quantity = static_cast<uint32_t>(qty(rng)),
            .filled = 0,
            .timestamp = id
        });
    }

    // Now benchmark: send aggressive orders that cross the spread
    // and match immediately. These are market-like limit orders
    // priced to guarantee a match.
    uint64_t bench_count = 0;
    for (auto _ : state) {
        bench_count++;
        std::uniform_int_distribution<int> side_dist(0, 1);
        
        if (side_dist(rng) == 0) {
            // Aggressive buy — priced above best ask to guarantee match
            book.add_order(Order{
                .id = ++id,
                .side = Side::Buy,
                .type = OrderType::Limit,
                .price = 1050.0,  // Will match any ask
                .quantity = 1,
                .filled = 0,
                .timestamp = id
            });
        } else {
            // Aggressive sell — priced below best bid to guarantee match
            book.add_order(Order{
                .id = ++id,
                .side = Side::Sell,
                .type = OrderType::Limit,
                .price = 950.0,  // Will match any bid
                .quantity = 1,
                .filled = 0,
                .timestamp = id
            });
        }
    }

    state.counters["orders"] = benchmark::Counter(
        static_cast<double>(bench_count),
        benchmark::Counter::kIsRate
    );
}
BENCHMARK(BM_MatchAgainstDeepBook);

// ============================================================
// BENCHMARK 4: Cancel orders from a filled book.
//
// Measures how fast we can find and remove an order.
// In the naive implementation, cancel requires scanning a
// linked list — this is one of the things optimization improves.
// ============================================================
static void BM_CancelOrder(benchmark::State& state) {
    OrderBook book;
    uint64_t id = 0;

    // Pre-fill with 10,000 buy orders across many price levels
    for (int i = 0; i < 10000; i++) {
        book.add_order(Order{
            .id = ++id,
            .side = Side::Buy,
            .type = OrderType::Limit,
            .price = 100.0 + (i % 200),  // 200 price levels
            .quantity = 50,
            .filled = 0,
            .timestamp = id
        });
    }

    // Benchmark: cancel orders one by one from the middle.
    // We cancel order IDs 5000, 5001, 5002, ... — orders
    // buried in the middle of the book, not at the edges.
    uint64_t cancel_id = 5000;
    for (auto _ : state) {
        // benchmark::DoNotOptimize prevents the compiler from
        // realizing that we never use the return value and
        // optimizing the entire cancel_order call away.
        // Without this, the compiler might skip the work entirely,
        // and we'd measure nothing instead of measuring cancel speed.
        benchmark::DoNotOptimize(book.cancel_order(cancel_id++));
        
        // Once we've cancelled all orders, stop
        if (cancel_id > id) {
            state.PauseTiming();  // Don't count the refill time
            // Refill the book
            for (int i = 0; i < 10000; i++) {
                book.add_order(Order{
                    .id = ++id,
                    .side = Side::Buy,
                    .type = OrderType::Limit,
                    .price = 100.0 + (i % 200),
                    .quantity = 50,
                    .filled = 0,
                    .timestamp = id
                });
            }
            cancel_id = id - 5000;  // Cancel from the middle again
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_CancelOrder);

// ============================================================
// This macro generates the main() function for the benchmark.
// It handles command-line argument parsing (like --benchmark_filter),
// runs all registered benchmarks, and prints the results.
// ============================================================
BENCHMARK_MAIN();