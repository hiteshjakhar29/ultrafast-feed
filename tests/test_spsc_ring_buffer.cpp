#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>

#include "ultrafast/spsc_ring_buffer.hpp"

using namespace ultrafast;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

using Ring8  = SpscRingBuffer<int, 8>;
using Ring4  = SpscRingBuffer<int, 4>;
using Ring16 = SpscRingBuffer<int, 16>;

// ─────────────────────────────────────────────────────────────────────────────
// Single-threaded correctness
// ─────────────────────────────────────────────────────────────────────────────

TEST(SpscRingBuffer, InitiallyEmpty) {
    Ring8 ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.full());
    EXPECT_EQ(ring.size(), 0u);
    EXPECT_EQ(ring.capacity(), 8u);
}

TEST(SpscRingBuffer, PushPopRoundTrip) {
    Ring8 ring;
    ASSERT_TRUE(ring.push(42));

    EXPECT_FALSE(ring.empty());
    EXPECT_EQ(ring.size(), 1u);

    const auto val = ring.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);

    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.size(), 0u);
}

TEST(SpscRingBuffer, PushMoveOverload) {
    SpscRingBuffer<int, 4> ring;
    int x = 77;
    ASSERT_TRUE(ring.push(std::move(x)));
    const auto val = ring.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 77);
}

TEST(SpscRingBuffer, FullRingRejectsPush) {
    // N=8: usable slots = N-1 = 7
    Ring8 ring;
    for (int i = 0; i < 7; ++i) {
        EXPECT_TRUE(ring.push(i)) << "push " << i << " should succeed";
    }
    EXPECT_TRUE(ring.full());
    EXPECT_FALSE(ring.push(99)) << "push on full ring must return false";
}

TEST(SpscRingBuffer, EmptyRingReturnsNullopt) {
    Ring8 ring;
    EXPECT_EQ(ring.pop(), std::nullopt);
}

TEST(SpscRingBuffer, PeekDoesNotConsume) {
    Ring8 ring;
    ring.push(7);

    const int* p = ring.peek();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 7);
    EXPECT_EQ(ring.size(), 1u);   // item still present

    const auto val = ring.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 7);

    EXPECT_EQ(ring.peek(), nullptr);  // empty
}

TEST(SpscRingBuffer, PeekOnEmptyReturnsNullptr) {
    Ring8 ring;
    EXPECT_EQ(ring.peek(), nullptr);
}

TEST(SpscRingBuffer, WrapAround) {
    // N=4: 3 usable slots.  Push/pop past the end-of-array to exercise wrap.
    Ring4 ring;

    EXPECT_TRUE(ring.push(1));
    EXPECT_TRUE(ring.push(2));
    EXPECT_TRUE(ring.push(3));
    EXPECT_TRUE(ring.full());

    EXPECT_EQ(ring.pop(), 1);
    EXPECT_EQ(ring.pop(), 2);

    // Slots 0,1 are now free — new items wrap around the array
    EXPECT_TRUE(ring.push(4));
    EXPECT_TRUE(ring.push(5));
    EXPECT_TRUE(ring.full());

    EXPECT_EQ(ring.pop(), 3);
    EXPECT_EQ(ring.pop(), 4);
    EXPECT_EQ(ring.pop(), 5);
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingBuffer, SizeTracking) {
    Ring8 ring;
    for (std::size_t i = 0; i < 7; ++i) {
        ring.push(static_cast<int>(i));
        EXPECT_EQ(ring.size(), i + 1);
    }
    for (std::size_t i = 7; i > 0; --i) {
        EXPECT_EQ(ring.size(), i);
        ring.pop();
    }
    EXPECT_EQ(ring.size(), 0u);
}

TEST(SpscRingBuffer, FillToCapacityAndDrain) {
    Ring16 ring;
    constexpr int kUsable = 15;  // N-1
    for (int i = 0; i < kUsable; ++i) {
        ASSERT_TRUE(ring.push(i));
    }
    EXPECT_TRUE(ring.full());
    for (int i = 0; i < kUsable; ++i) {
        const auto v = ring.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingBuffer, TriviallyCopiableStruct) {
    struct Tick {
        uint64_t price;
        uint32_t qty;
        uint32_t seq;
    };
    static_assert(std::is_trivially_copyable_v<Tick>);

    SpscRingBuffer<Tick, 16> ring;
    ASSERT_TRUE(ring.push({100ULL, 500U, 1U}));

    const auto v = ring.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->price, 100u);
    EXPECT_EQ(v->qty,   500u);
    EXPECT_EQ(v->seq,     1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-threaded SPSC correctness
// ─────────────────────────────────────────────────────────────────────────────

TEST(SpscRingBuffer, SpscThroughputAndCorrectness) {
    static constexpr std::size_t kTotal = 100'000;
    SpscRingBuffer<uint64_t, 4096> ring;

    std::atomic<bool> start{false};
    std::vector<uint64_t> received;
    received.reserve(kTotal);

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) { /* spin */ }
        for (uint64_t i = 0; i < kTotal; ++i) {
            while (!ring.push(i)) { /* spin: yield to consumer */ }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) { /* spin */ }
        while (received.size() < kTotal) {
            if (auto v = ring.pop(); v.has_value()) {
                received.push_back(*v);
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
        EXPECT_EQ(received[i], static_cast<uint64_t>(i))
            << "ordering violation at index " << i;
    }
}
