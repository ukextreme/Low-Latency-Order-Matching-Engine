#include <benchmark/benchmark.h>
#include "spsc_queue.h"
#include "order.h"
#include <thread>

// ============================================================
// Benchmark: throughput of the SPSC queue
//
// Measures how many elements per second can flow through the
// queue from producer to consumer. This is the number that
// tells you the queue's maximum throughput.
//
// We benchmark with Order structs, not ints, because that's
// the realistic use case — passing orders between threads.
// ============================================================
static void BM_SPSCThroughput(benchmark::State& state) {
    constexpr size_t QUEUE_SIZE = 65536;  // 2^16 = 64K slots
    SPSCQueue<Order, QUEUE_SIZE> queue;

    uint64_t total_ops = 0;

    for (auto _ : state) {
        // Push and pop 1000 orders per iteration.
        // We do batches because a single push+pop is too fast
        // to measure accurately.
        constexpr int BATCH = 1000;

        // Producer: push a batch
        for (int i = 0; i < BATCH; i++) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .side = Side::Buy,
                .type = OrderType::Limit,
                .price = 100.0,
                .quantity = 50,
                .filled = 0,
                .timestamp = static_cast<uint64_t>(i)
            };
            while (!queue.push(order)) {}
        }

        // Consumer: pop the entire batch
        for (int i = 0; i < BATCH; i++) {
            while (!queue.pop().has_value()) {}
        }

        total_ops += BATCH;
    }

    state.counters["ops"] = benchmark::Counter(
        static_cast<double>(total_ops),
        benchmark::Counter::kIsRate
    );
}
BENCHMARK(BM_SPSCThroughput);

// ============================================================
// Benchmark: concurrent throughput (the real test)
//
// Producer and consumer run on separate threads simultaneously.
// This measures the actual cross-thread throughput with the
// memory ordering overhead.
// ============================================================
static void BM_SPSCConcurrent(benchmark::State& state) {
    constexpr size_t QUEUE_SIZE = 65536;
    constexpr int ITEMS_PER_ITER = 100000;

    for (auto _ : state) {
        SPSCQueue<Order, QUEUE_SIZE> queue;
        std::atomic<bool> done{false};

        // Consumer thread
        std::thread consumer([&queue, &done]() {
            while (!done.load(std::memory_order_relaxed)) {
                queue.pop();  // Discard the result — we're measuring throughput
            }
            // Drain remaining items after producer signals done
            while (queue.pop().has_value()) {}
        });

        // Producer (this thread): push ITEMS_PER_ITER orders
        for (int i = 0; i < ITEMS_PER_ITER; i++) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .side = Side::Buy,
                .type = OrderType::Limit,
                .price = 100.0,
                .quantity = 50,
                .filled = 0,
                .timestamp = static_cast<uint64_t>(i)
            };
            while (!queue.push(order)) {}
        }

        done.store(true, std::memory_order_relaxed);
        consumer.join();
    }

    state.counters["ops"] = benchmark::Counter(
        static_cast<double>(ITEMS_PER_ITER),
        benchmark::Counter::kIsRate
    );
}
BENCHMARK(BM_SPSCConcurrent);

BENCHMARK_MAIN();