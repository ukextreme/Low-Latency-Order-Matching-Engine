#include <benchmark/benchmark.h>
#include "orderbook.h"

static void BM_AddOrder(benchmark::State& state) {
    OrderBook book;
    uint64_t id = 0;

    for (auto _ : state) {
        book.add_order(Order{
            .id = ++id, .side = Side::Buy, .type = OrderType::Limit,
            .price = 100.0, .quantity = 50, .filled = 0, .timestamp = id
        });
    }
}
BENCHMARK(BM_AddOrder);

BENCHMARK_MAIN();