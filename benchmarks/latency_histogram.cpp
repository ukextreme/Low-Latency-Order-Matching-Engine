// ============================================================
// PER-OPERATION LATENCY HISTOGRAM
//
// Google Benchmark reports the mean of each repetition, so a
// "p99" taken from its output is a p99 of averages — it hides
// exactly the tail that matters. This harness times every
// individual operation with rdtscp and reports true percentiles.
//
// Timing at ~18 ns means the clock itself is a material cost, so
// rdtscp overhead is measured and subtracted, and the TSC is
// calibrated against steady_clock to convert cycles to ns.
// ============================================================

#include "orderbook.h"
#include "orderbook_opt.h"

#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Keep the compiler from deleting work whose result we ignore.
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline uint64_t now_cycles() {
    unsigned aux;
    return __rdtscp(&aux);
}

// Median cost of the timing instruction pair itself.
uint64_t measure_clock_overhead() {
    constexpr int kSamples = 200000;
    std::vector<uint64_t> deltas;
    deltas.reserve(kSamples);
    for (int i = 0; i < kSamples; i++) {
        uint64_t a = now_cycles();
        uint64_t b = now_cycles();
        deltas.push_back(b - a);
    }
    std::sort(deltas.begin(), deltas.end());
    return deltas[deltas.size() / 2];
}

// Cycles per nanosecond, measured rather than assumed — the
// nominal 2688 MHz is not necessarily the TSC rate.
double measure_tsc_ghz() {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    uint64_t c0 = now_cycles();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now() - t0).count() < 200) {
    }
    uint64_t c1 = now_cycles();
    auto t1 = clock::now();
    double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;
}

double pct(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return static_cast<double>(sorted[lo]) * (1.0 - frac) +
           static_cast<double>(sorted[hi]) * frac;
}

void report(const std::string& name,
            std::vector<uint64_t> samples,
            uint64_t overhead,
            double ghz) {
    // Subtract clock overhead; clamp at zero rather than wrapping.
    for (auto& s : samples) s = (s > overhead) ? (s - overhead) : 0;
    std::sort(samples.begin(), samples.end());

    double sum = 0.0;
    for (auto s : samples) sum += static_cast<double>(s);
    double mean_ns = (sum / static_cast<double>(samples.size())) / ghz;

    std::printf("%-28s n=%-8zu mean=%8.1f  p50=%8.1f  p90=%8.1f  "
                "p99=%8.1f  p99.9=%9.1f  max=%10.1f\n",
                name.c_str(), samples.size(), mean_ns,
                pct(samples, 0.50) / ghz,
                pct(samples, 0.90) / ghz,
                pct(samples, 0.99) / ghz,
                pct(samples, 0.999) / ghz,
                static_cast<double>(samples.back()) / ghz);
}

constexpr int kOrders = 20000;
constexpr int kLevels = 200;

// ---- scan-depth sweep --------------------------------------
//
// Naive cancel is a linear walk of the std::list at one price
// level, so its cost is set by how deep the target sits, not by
// book size. Holding order count fixed and shrinking the number
// of price levels deepens each list, isolating that effect.
// Cancelling in reverse id order puts the target at the back of
// its level every time — the worst case for the naive book.

template <typename Fn>
std::vector<uint64_t> timed_reverse_cancel(int levels, Fn&& cancel) {
    std::vector<uint64_t> samples;
    samples.reserve(kOrders);
    for (int i = kOrders; i >= 1; i--) {
        uint64_t t0 = now_cycles();
        bool r = cancel(static_cast<uint64_t>(i));
        uint64_t t1 = now_cycles();
        do_not_optimize(r);
        samples.push_back(t1 - t0);
    }
    (void)levels;
    return samples;
}

std::vector<uint64_t> naive_depth_sweep(int levels) {
    OrderBook book;
    for (int i = 0; i < kOrders; i++) {
        book.add_order(Order{
            .id = static_cast<uint64_t>(i + 1),
            .side = Side::Buy, .type = OrderType::Limit,
            .price = 100.0 + static_cast<double>(i % levels),
            .quantity = 50, .filled = 0,
            .timestamp = static_cast<uint64_t>(i + 1),
        });
    }
    return timed_reverse_cancel(levels,
        [&book](uint64_t id) { return book.cancel_order(id); });
}

std::vector<uint64_t> opt_depth_sweep(int levels) {
    OrderBookOpt book(1.0, 400.0, 1.0);
    for (int i = 0; i < kOrders; i++) {
        book.add_order(AlignedOrder{
            .id = static_cast<uint64_t>(i + 1),
            .price = 100.0 + static_cast<double>(i % levels),
            .quantity = 50, .filled = 0,
            .side = Side::Buy, .type = OrderType::Limit,
            .active = true, .timestamp = static_cast<uint64_t>(i + 1),
        });
    }
    return timed_reverse_cancel(levels,
        [&book](uint64_t id) { return book.cancel_order(id); });
}

// ---- optimized book ----------------------------------------

std::vector<uint64_t> opt_cancel_samples() {
    OrderBookOpt book(1.0, 400.0, 1.0);
    for (int i = 0; i < kOrders; i++) {
        book.add_order(AlignedOrder{
            .id = static_cast<uint64_t>(i + 1),
            .price = 100.0 + static_cast<double>(i % kLevels),
            .quantity = 50, .filled = 0,
            .side = Side::Buy, .type = OrderType::Limit,
            .active = true, .timestamp = static_cast<uint64_t>(i + 1),
        });
    }
    std::vector<uint64_t> samples;
    samples.reserve(kOrders);
    for (int i = 0; i < kOrders; i++) {
        uint64_t t0 = now_cycles();
        bool r = book.cancel_order(static_cast<uint64_t>(i + 1));
        uint64_t t1 = now_cycles();
        do_not_optimize(r);
        samples.push_back(t1 - t0);
    }
    return samples;
}

std::vector<uint64_t> opt_add_samples() {
    OrderBookOpt book(1.0, 400.0, 1.0);
    std::vector<uint64_t> samples;
    samples.reserve(kOrders);
    for (int i = 0; i < kOrders; i++) {
        AlignedOrder o{
            .id = static_cast<uint64_t>(i + 1),
            .price = 100.0 + static_cast<double>(i % kLevels),
            .quantity = 50, .filled = 0,
            .side = Side::Buy, .type = OrderType::Limit,
            .active = true, .timestamp = static_cast<uint64_t>(i + 1),
        };
        uint64_t t0 = now_cycles();
        auto trades = book.add_order(o);
        uint64_t t1 = now_cycles();
        do_not_optimize(trades);
        samples.push_back(t1 - t0);
    }
    return samples;
}

// ---- naive book --------------------------------------------

std::vector<uint64_t> naive_cancel_samples() {
    OrderBook book;
    for (int i = 0; i < kOrders; i++) {
        book.add_order(Order{
            .id = static_cast<uint64_t>(i + 1),
            .side = Side::Buy, .type = OrderType::Limit,
            .price = 100.0 + static_cast<double>(i % kLevels),
            .quantity = 50, .filled = 0,
            .timestamp = static_cast<uint64_t>(i + 1),
        });
    }
    std::vector<uint64_t> samples;
    samples.reserve(kOrders);
    for (int i = 0; i < kOrders; i++) {
        uint64_t t0 = now_cycles();
        bool r = book.cancel_order(static_cast<uint64_t>(i + 1));
        uint64_t t1 = now_cycles();
        do_not_optimize(r);
        samples.push_back(t1 - t0);
    }
    return samples;
}

// ---- wide sparse book --------------------------------------
//
// The flat array's weak spot: when an aggressive order empties a
// price level, the book must find the next OCCUPIED level. On a wide
// grid with sparse liquidity that search dominates. Each aggressive
// buy below consumes exactly one resting order, emptying its level
// and forcing that search every time.
std::vector<uint64_t> wide_sparse_sweep() {
    constexpr int kWideLevels = 20000;
    constexpr int kGap = 500;   // levels between resting orders
    constexpr int kRounds = 40;

    std::vector<uint64_t> samples;
    samples.reserve(kRounds * (kWideLevels / kGap));

    for (int round = 0; round < kRounds; round++) {
        OrderBookOpt book(1.0, static_cast<double>(kWideLevels) + 1.0, 1.0);
        uint64_t id = 0;
        for (int lvl = 0; lvl < kWideLevels; lvl += kGap) {
            book.add_order(AlignedOrder{
                .id = ++id,
                .price = 1.0 + static_cast<double>(lvl),
                .quantity = 1, .filled = 0,
                .side = Side::Sell, .type = OrderType::Limit,
                .active = true, .timestamp = id,
            });
        }
        for (int i = 0; i < kWideLevels / kGap; i++) {
            AlignedOrder buy{
                .id = ++id,
                .price = static_cast<double>(kWideLevels),
                .quantity = 1, .filled = 0,
                .side = Side::Buy, .type = OrderType::Limit,
                .active = true, .timestamp = id,
            };
            uint64_t t0 = now_cycles();
            auto trades = book.add_order(buy);
            uint64_t t1 = now_cycles();
            do_not_optimize(trades);
            samples.push_back(t1 - t0);
        }
    }
    return samples;
}

}  // namespace

int main() {
    uint64_t overhead = measure_clock_overhead();
    double ghz = measure_tsc_ghz();

    std::printf("TSC calibration: %.4f cycles/ns  |  rdtscp overhead: %llu cycles "
                "(%.1f ns, subtracted from every sample)\n",
                ghz, static_cast<unsigned long long>(overhead),
                static_cast<double>(overhead) / ghz);
    std::printf("Book: %d resting orders across %d price levels\n\n",
                kOrders, kLevels);
    std::printf("%-28s %-10s %9s %10s %10s %10s %11s %12s\n",
                "operation", "samples", "mean(ns)", "p50(ns)", "p90(ns)",
                "p99(ns)", "p99.9(ns)", "max(ns)");

    report("OptBook::cancel_order", opt_cancel_samples(), overhead, ghz);
    report("OptBook::add_order", opt_add_samples(), overhead, ghz);
    report("NaiveBook::cancel_order", naive_cancel_samples(), overhead, ghz);

    std::printf("\n");
    report("OptBook::sweep(20k levels)", wide_sparse_sweep(), overhead, ghz);

    std::printf("\n\nSCAN-DEPTH SWEEP — %d orders held constant, price levels "
                "reduced to deepen each level's list.\n"
                "Cancelling in reverse id order puts the target at the back of "
                "its level (naive worst case).\n\n", kOrders);
    std::printf("%-12s %-14s %-28s %10s %10s %10s\n",
                "levels", "orders/level", "book", "p50(ns)", "p99(ns)",
                "mean(ns)");

    for (int levels : {200, 50, 10, 2}) {
        char label[64];
        std::snprintf(label, sizeof(label), "%-12d %-14d %-28s",
                      levels, kOrders / levels, "NaiveBook::cancel_order");
        auto s = naive_depth_sweep(levels);
        for (auto& v : s) v = (v > overhead) ? (v - overhead) : 0;
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (auto v : s) sum += static_cast<double>(v);
        std::printf("%s %10.1f %10.1f %10.1f\n", label,
                    pct(s, 0.50) / ghz, pct(s, 0.99) / ghz,
                    (sum / static_cast<double>(s.size())) / ghz);
    }
    std::printf("\n");
    for (int levels : {200, 50, 10, 2}) {
        char label[64];
        std::snprintf(label, sizeof(label), "%-12d %-14d %-28s",
                      levels, kOrders / levels, "OptBook::cancel_order");
        auto s = opt_depth_sweep(levels);
        for (auto& v : s) v = (v > overhead) ? (v - overhead) : 0;
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (auto v : s) sum += static_cast<double>(v);
        std::printf("%s %10.1f %10.1f %10.1f\n", label,
                    pct(s, 0.50) / ghz, pct(s, 0.99) / ghz,
                    (sum / static_cast<double>(s.size())) / ghz);
    }

    return 0;
}
