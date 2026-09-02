# Low-Latency Order Matching Engine

A price-time-priority limit order book in C++20, built twice: once the obvious way, then
again for latency. Both implementations live side by side behind the same test suite, so
every optimisation is measured against a working baseline rather than asserted.

The headline result is cancel latency — from **microseconds to 17 nanoseconds** — achieved
by replacing a linear scan with an indexed lookup and lazy deletion.

---

## What it is

An exchange matching engine takes a stream of orders and decides who trades with whom.
The rules are simple and unforgiving:

- **Price priority** — the best price matches first. A buyer at 101 gets filled before a
  buyer at 100.
- **Time priority** — at the same price, whoever arrived first gets filled first.
- Orders that cannot match immediately **rest** in the book and wait.

Supported order types: **limit**, **market**, and **cancel**.

```cpp
OrderBook book;

book.add_order({.id = 1, .side = Side::Buy,  .type = OrderType::Limit,
                .price = 100.0, .quantity = 50, .filled = 0, .timestamp = 1});

auto trades = book.add_order({.id = 2, .side = Side::Sell, .type = OrderType::Limit,
                              .price = 100.0, .quantity = 30, .filled = 0, .timestamp = 2});
// trades[0] => 30 shares at 100.0, buy #1 against sell #2
// 20 shares of order #1 remain resting at the top of the book
```

---

## The two order books

### `OrderBook` — the baseline (`include/orderbook.h`)

Price levels in a tree, orders within a level in a `std::list`. Textbook-correct,
and it establishes what the answers *should* be. Every optimisation is validated
against it.

### `OrderBookOpt` — the optimised book (`include/orderbook_opt.h`)

Four changes, each targeting a specific cost:

| # | Change | Replaces | Why it is faster |
|---|---|---|---|
| 1 | **Flat array of price levels**, indexed by price | Tree of price levels | `price_to_index()` is arithmetic, not traversal. Best bid/ask is a cached index — O(1), no tree walk. |
| 2 | **`std::vector` of orders** per level | `std::list` | Contiguous memory. Scanning a level walks one cache line to the next instead of chasing heap pointers. |
| 3 | **`unordered_map<order_id → location>`** | Linear scan to find an order | Cancel becomes O(1) instead of O(n). This is the 1000×. |
| 4 | **`front_idx` per price level** | Erasing filled orders from the vector | Filled orders are marked inactive, not removed. `front_idx` skips the dead prefix, so a fill never shifts the vector. |

The `AlignedOrder` struct is `alignas(64)` with fields reordered so the hot ones —
`id`, `price`, `quantity`, `filled`, `side`, `type`, `active` — sit in the first 32
bytes and share a cache line during matching. The cold `timestamp` is pushed out of
the way.

> **A note on `alignas(64)`.** Cache-line alignment is not free — it costs memory and
> hurts sequential scans, where tight packing wins. It pays off in the SPSC queue below,
> where two *different threads* touch adjacent variables.

---

## Lock-free SPSC ring buffer (`include/spsc_queue.h`)

A single-producer / single-consumer queue with no locks, no syscalls, and no heap
allocation after construction — the standard way to hand orders from a network thread
to a matching thread without either one blocking.

```cpp
SPSCQueue<Order, 1024> q;    // capacity must be a power of 2

q.push(order);               // producer thread — returns false if full, never blocks
auto item = q.pop();         // consumer thread — std::nullopt if empty, never blocks
```

Three things make it correct and fast:

**Power-of-two capacity.** Wrapping is `index & (Capacity - 1)`, a single AND
instruction, instead of a division. Enforced at compile time by `static_assert`.

**Acquire/release ordering, not sequential consistency.** The producer writes the slot,
then does a **release** store on `head_`. The consumer does an **acquire** load on
`head_`. That pairing is exactly enough to guarantee the consumer sees the buffer data
before it sees the advanced index — and nothing more, so no full memory fence is
emitted. Each thread reads *its own* index with `memory_order_relaxed`, since no
synchronisation is needed to observe your own writes.

**Cache-line padding.** `head_` and `tail_` are each `alignas(64)`, putting them on
separate cache lines. Without it, the producer writing `head_` invalidates the line
holding `tail_`, forcing the consumer to reload from memory even though `tail_` never
changed — **false sharing**, and it is expensive.

One slot is always left empty so that "full" and "empty" are distinguishable; usable
capacity is `Capacity - 1`.

---

## Benchmarks

Google Benchmark, release build (`-O2 -DNDEBUG`), 16-core x86-64, L1d 48 KiB / L2 1280 KiB
/ L3 24 MiB. Reproduce with `./build/engine_bench` and `./build/spsc_bench`.

| Operation | Baseline | Optimised | Speedup |
|---|---:|---:|---|
| Add order, no matching | 104 ns | **70.5 ns** | 1.5× — 33% faster |
| Add order, with matching | 163 ns | **128 ns** | 1.3× |
| **Cancel order** | 6,566 ns | **17.0 ns** | **~390×** |
| Match against deep book | 32.3 ns | 42.1 ns | 0.8× — see below |

| SPSC queue | Result |
|---|---:|
| Single-threaded throughput | **98.2 M ops/s** |
| Concurrent producer/consumer | 220 k round-trips/s |

**On cancel.** The baseline scans a price level linearly, so its cost is a function of
how deep the book is — it climbs into the tens of microseconds as orders accumulate,
and the speedup grows with it. The optimised figure does not move: a hash lookup plus a
flag write is 17 ns whether the book holds ten orders or ten million. Constant-time is
the point, not the ratio.

**On the deep-book regression.** The optimised book is *slower* on one benchmark, and
that is worth stating plainly rather than hiding. Lazy deletion means matching walks
past inactive orders that the baseline had already erased; `front_idx` amortises this
but does not eliminate it. The trade is deliberate — a ~10 ns cost on match, in exchange
for removing a linear scan from cancel. In real markets most orders are cancelled, not
filled, so it is the right side of the trade.

---

## Building

```bash
git clone --recurse-submodules https://github.com/ukextreme/Low-Latency-Order-Matching-Engine.git
cd Low-Latency-Order-Matching-Engine

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Already cloned without `--recurse-submodules`?

```bash
git submodule update --init --recursive
```

Requires a C++20 compiler and CMake ≥ 3.16. GoogleTest and Google Benchmark are pinned
submodules under `third_party/`.

### Run

```bash
./build/engine          # demo
./build/engine_tests    # 24 tests across 3 suites
./build/engine_bench    # order book benchmarks
./build/spsc_bench      # queue benchmarks
```

A debug build turns on ASan and UBSan (`-fsanitize=address,undefined`):

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug -j
```

---

## Tests

24 GoogleTest cases, all passing:

- **`OrderBookTest`** — price priority, time priority at equal price, partial fills,
  market orders sweeping levels, cancel, spread, empty-book edges.
- **`OrderBookOptTest`** — the same behaviour on the optimised book. Identical
  expectations: an optimisation that changes an answer is a bug.
- **`SPSCQueueTest`** — FIFO order, full and empty conditions, index wrap-around,
  struct payloads, and a genuine two-thread concurrent producer/consumer run.

---

## Layout

```
include/
  order.h            Order, Trade, Side, OrderType
  orderbook.h        baseline book
  orderbook_opt.h    optimised book — flat arrays, O(1) cancel, cache-aligned orders
  spsc_queue.h       lock-free ring buffer (header-only template)
src/
  order.cpp          order helpers
  orderbook.cpp      baseline matching
  orderbook_opt.cpp  optimised matching
  main.cpp           demo
tests/               GoogleTest suites
benchmarks/          Google Benchmark suites
third_party/         googletest, benchmark (submodules)
```

## Built with

C++20 · CMake · GoogleTest · Google Benchmark · `std::atomic` / C++ memory model
