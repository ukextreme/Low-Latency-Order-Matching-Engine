#pragma once

#include <atomic>
#include <array>
#include <optional>
#include <cstddef>

// ============================================================
// Lock-Free Single-Producer Single-Consumer (SPSC) Ring Buffer
//
// Template parameters:
//   T        — the type of elements stored (e.g., Order)
//   Capacity — the fixed size of the buffer (must be power of 2)
//
// Power-of-2 capacity is required because we use bitwise AND
// for wrapping instead of modulo. index & (Capacity - 1) is
// equivalent to index % Capacity but compiles to a single AND
// instruction instead of an expensive division.
//
// Example: Capacity = 1024 (2^10)
//   Capacity - 1 = 1023 = 0b1111111111
//   index & 1023 wraps index to 0-1023
//
// Thread safety:
//   - Exactly ONE thread calls push() (the producer)
//   - Exactly ONE thread calls pop() (the consumer)
//   - No other synchronization needed
//   - Using with multiple producers or consumers is UNDEFINED BEHAVIOR
//
// Performance:
//   - Zero heap allocation during operation (fixed-size array)
//   - Zero locks, zero syscalls
//   - Cache-line padding between head and tail prevents false sharing
// ============================================================
template <typename T, size_t Capacity>
class SPSCQueue {
    // Compile-time check: Capacity must be a power of 2.
    // static_assert runs at compile time — if the condition is
    // false, the program won't compile and shows the error message.
    // This catches misuse before the program even runs.
    static_assert(
        (Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2"
    );

public:
    SPSCQueue() : head_(0), tail_(0) {}

    // ========================================================
    // push: Producer writes an element into the buffer.
    //
    // Returns true if successful, false if the buffer is full.
    // Never blocks — if the buffer is full, it returns immediately.
    //
    // Only ONE thread may call this method.
    // ========================================================
    bool push(const T& item) {
        // Load head with relaxed ordering.
        // Only the producer modifies head_, so we don't need
        // synchronization to read our own variable — we always
        // see our own latest write.
        const size_t current_head = head_.load(std::memory_order_relaxed);

        // Calculate the next head position (with wrapping).
        // Bitwise AND with (Capacity - 1) is the fast modulo.
        const size_t next_head = (current_head + 1) & (Capacity - 1);

        // Check if the buffer is full.
        // Load tail_ with ACQUIRE ordering because we need to see
        // the consumer's latest pop (which advanced tail_).
        // If next_head == tail_, every slot is occupied — full.
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Buffer full — don't block, just fail
        }

        // Write the data into the buffer at the current head position.
        // This is a normal (non-atomic) write to the buffer array.
        // It's safe because:
        //   1. Only we (the producer) write to buffer_[current_head]
        //   2. The consumer won't read this slot until we advance head_
        //   3. The release store on head_ below ensures this write
        //      is visible before the head advance is visible
        buffer_[current_head] = item;

        // Advance head with RELEASE ordering.
        // This is the critical synchronization point:
        //   - RELEASE guarantees that the buffer write above (and all
        //     previous memory operations) are visible to any thread
        //     that does an ACQUIRE load on head_.
        //   - The consumer's acquire load on head_ will see this new
        //     value AND the buffer data we just wrote.
        //   - Without release, the CPU might reorder the head advance
        //     before the data write, and the consumer would read
        //     garbage from the buffer.
        head_.store(next_head, std::memory_order_release);

        return true;
    }

    // ========================================================
    // pop: Consumer reads an element from the buffer.
    //
    // Returns the element wrapped in std::optional if available,
    // or std::nullopt if the buffer is empty.
    // Never blocks — if empty, returns immediately.
    //
    // Only ONE thread may call this method.
    // ========================================================
    std::optional<T> pop() {
        // Load tail with relaxed ordering.
        // Only the consumer modifies tail_, so no synchronization
        // needed to read our own variable.
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check if the buffer is empty.
        // Load head_ with ACQUIRE ordering because we need to see
        // the producer's latest push (which advanced head_).
        // If tail_ == head_, no new data — empty.
        //
        // The ACQUIRE here pairs with the RELEASE store in push():
        // once we see the new head_ value, we're guaranteed to also
        // see the data the producer wrote into the buffer before
        // advancing head_.
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Buffer empty
        }

        // Read the data from the buffer at the current tail position.
        // Safe because:
        //   1. Only we (the consumer) read from buffer_[current_tail]
        //   2. The producer won't overwrite this slot until we advance tail_
        //   3. The acquire load on head_ above ensures we see the data
        //      the producer wrote
        T item = buffer_[current_tail];

        // Advance tail with RELEASE ordering.
        // This tells the producer that we're done with this slot —
        // it can now be reused. The release ensures our read of
        // the buffer data is complete before we signal availability.
        const size_t next_tail = (current_tail + 1) & (Capacity - 1);
        tail_.store(next_tail, std::memory_order_release);

        return item;
    }

    // ========================================================
    // Utility methods (all lock-free, safe from any thread)
    // ========================================================

    // Is the buffer empty? (approximate — may be stale by the
    // time you act on the result, but safe to call)
    bool empty() const {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    // How many elements are currently in the buffer?
    // Approximate — the actual count may change between reading
    // head and tail, but it's useful for monitoring/debugging.
    size_t size() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) & (Capacity - 1);
    }

    // Maximum number of elements the buffer can hold.
    // One slot is always empty (to distinguish full from empty),
    // so usable capacity is Capacity - 1.
    static constexpr size_t capacity() {
        return Capacity - 1;
    }

private:
    // The buffer: a fixed-size array, allocated inline (no heap).
    // std::array is a thin wrapper around a C array with bounds
    // checking in debug mode and zero overhead in release mode.
    std::array<T, Capacity> buffer_;

    // Head: where the producer writes next.
    // Tail: where the consumer reads next.
    //
    // These are on SEPARATE CACHE LINES to prevent false sharing.
    // Without the padding, head_ and tail_ would be adjacent in
    // memory (within the same 64-byte cache line). When the producer
    // writes head_, it invalidates the entire cache line — including
    // tail_, which the consumer is reading. The consumer's CPU has
    // to reload the cache line from main memory even though tail_
    // didn't change. This is "false sharing" — two unrelated
    // variables sharing a cache line and causing unnecessary
    // cache invalidations.
    //
    // alignas(64) ensures each atomic variable starts at a 64-byte
    // boundary, putting them on different cache lines. The producer
    // writing head_ doesn't affect the consumer reading tail_.
    //
    // THIS is where alignas(64) actually helps — not on data that's
    // scanned sequentially (where tight packing is better), but on
    // variables accessed by different threads (where isolation is better).
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};