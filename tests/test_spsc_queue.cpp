#include <gtest/gtest.h>
#include "spsc_queue.h"
#include <thread>
#include <vector>

// ============================================================
// Single-threaded tests (verify correctness without concurrency)
// ============================================================

TEST(SPSCQueueTest, EmptyQueue) {
    SPSCQueue<int, 16> queue;

    // A new queue should be empty
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0);

    // Pop from empty queue returns nullopt
    auto result = queue.pop();
    EXPECT_FALSE(result.has_value());
}

TEST(SPSCQueueTest, PushAndPop) {
    SPSCQueue<int, 16> queue;

    // Push an element
    EXPECT_TRUE(queue.push(42));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size(), 1);

    // Pop it back
    auto result = queue.pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);

    // Queue should be empty again
    EXPECT_TRUE(queue.empty());
}

TEST(SPSCQueueTest, FIFO_Order) {
    SPSCQueue<int, 16> queue;

    // Push 1, 2, 3
    queue.push(1);
    queue.push(2);
    queue.push(3);

    // Pop should return 1, 2, 3 (First In, First Out)
    EXPECT_EQ(queue.pop().value(), 1);
    EXPECT_EQ(queue.pop().value(), 2);
    EXPECT_EQ(queue.pop().value(), 3);
}

TEST(SPSCQueueTest, FullQueue) {
    // Capacity 4 means usable capacity is 3
    // (one slot always empty to distinguish full from empty)
    SPSCQueue<int, 4> queue;

    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.push(3));

    // Queue is full — push should fail
    EXPECT_FALSE(queue.push(4));

    // Pop one, now there's room
    EXPECT_EQ(queue.pop().value(), 1);
    EXPECT_TRUE(queue.push(4));
}

TEST(SPSCQueueTest, WrapAround) {
    // Small buffer to test wrapping behavior
    SPSCQueue<int, 4> queue;

    // Fill and drain multiple times to force wrap-around
    for (int round = 0; round < 5; round++) {
        // Push 3 (fills the buffer)
        for (int i = 0; i < 3; i++) {
            EXPECT_TRUE(queue.push(round * 10 + i));
        }

        // Pop 3
        for (int i = 0; i < 3; i++) {
            auto val = queue.pop();
            EXPECT_TRUE(val.has_value());
            EXPECT_EQ(val.value(), round * 10 + i);
        }
    }
}

TEST(SPSCQueueTest, StructType) {
    // Test with a struct, not just int — proves it works with Order-like types
    struct TestOrder {
        uint64_t id;
        double price;
        uint32_t quantity;
    };

    SPSCQueue<TestOrder, 16> queue;

    queue.push(TestOrder{.id = 1, .price = 100.5, .quantity = 50});
    queue.push(TestOrder{.id = 2, .price = 101.0, .quantity = 75});

    auto order1 = queue.pop();
    EXPECT_TRUE(order1.has_value());
    EXPECT_EQ(order1->id, 1);
    EXPECT_EQ(order1->price, 100.5);
    EXPECT_EQ(order1->quantity, 50);

    auto order2 = queue.pop();
    EXPECT_EQ(order2->id, 2);
}

// ============================================================
// Multi-threaded test: the real proof
//
// One thread pushes N integers, another thread pops them.
// We verify that every integer arrives exactly once, in order.
// This is the test that proves the lock-free implementation
// is correct under real concurrent access.
// ============================================================
TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    // Large buffer and many elements to stress-test
    constexpr size_t BUFFER_SIZE = 1024;
    constexpr int NUM_ELEMENTS = 1000000;

    SPSCQueue<int, BUFFER_SIZE> queue;
    std::vector<int> received;
    received.reserve(NUM_ELEMENTS);

    // Consumer thread: pop elements until we have all of them
    std::thread consumer([&queue, &received]() {
        int count = 0;
        while (count < NUM_ELEMENTS) {
            auto val = queue.pop();
            if (val.has_value()) {
                received.push_back(val.value());
                count++;
            }
            // If empty, just spin — no sleep, no yield.
            // In a real system you might yield or backoff,
            // but for the test we want maximum speed to
            // stress the synchronization.
        }
    });

    // Producer thread (this thread): push all elements
    for (int i = 0; i < NUM_ELEMENTS; i++) {
        // Spin until push succeeds (buffer might be full)
        while (!queue.push(i)) {
            // Buffer full — consumer hasn't caught up yet.
            // Spin and retry. In production, you might
            // yield or do other work here.
        }
    }

    // Wait for consumer to finish
    consumer.join();

    // Verify: every element received exactly once, in order
    ASSERT_EQ(received.size(), static_cast<size_t>(NUM_ELEMENTS));
    for (int i = 0; i < NUM_ELEMENTS; i++) {
        EXPECT_EQ(received[i], i) << "Mismatch at index " << i;
    }
}