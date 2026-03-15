#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "ultrafast/spsc_ring_buffer.hpp"

using namespace ultrafast;

// ─────────────────────────────────────────────────────────────────────────────
// Single-threaded benchmarks
// ─────────────────────────────────────────────────────────────────────────────

/// Round-trip latency: one push followed by one pop per iteration.
static void BM_SpscPushPop(benchmark::State& state) {
    SpscRingBuffer<int, 1024> ring;
    for (auto _ : state) {
        ring.push(42);
        benchmark::DoNotOptimize(ring.pop());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop);

/// Fill half the ring then drain it; measures amortised push+pop cost.
static void BM_SpscBatchFillDrain(benchmark::State& state) {
    static constexpr int kBatch = 512;
    SpscRingBuffer<int, 1024> ring;   // 1023 usable slots, kBatch fits easily

    for (auto _ : state) {
        for (int i = 0; i < kBatch; ++i) ring.push(i);
        for (int i = 0; i < kBatch; ++i) benchmark::DoNotOptimize(ring.pop());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kBatch);
}
BENCHMARK(BM_SpscBatchFillDrain);

/// Peek latency (does not consume the item).
static void BM_SpscPeek(benchmark::State& state) {
    SpscRingBuffer<int, 8> ring;
    ring.push(1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.peek());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPeek);

/// Measure size() query cost under no contention.
static void BM_SpscSize(benchmark::State& state) {
    SpscRingBuffer<uint64_t, 4096> ring;
    for (int i = 0; i < 100; ++i) ring.push(static_cast<uint64_t>(i));
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.size());
    }
}
BENCHMARK(BM_SpscSize);

// ─────────────────────────────────────────────────────────────────────────────
// Multi-threaded SPSC throughput
// ─────────────────────────────────────────────────────────────────────────────

/// Producer thread pushes kBatch items per iteration; consumer drains.
/// Reports items/s and bytes/s so the output is easy to compare.
static void BM_SpscThroughput(benchmark::State& state) {
    static constexpr std::size_t kBatch = 1024;

    SpscRingBuffer<uint64_t, 4096> ring;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> consumed{0};

    // Consumer: runs on a dedicated thread for the full benchmark duration.
    std::thread consumer([&] {
        while (running.load(std::memory_order_relaxed)) {
            if (auto v = ring.pop(); v.has_value()) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Drain whatever the producer left in the ring.
        while (auto v = ring.pop()) {
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            uint64_t val = static_cast<uint64_t>(i);
            while (!ring.push(val)) { /* spin: consumer is behind */ }
        }
    }

    running.store(false, std::memory_order_relaxed);
    consumer.join();

    const int64_t total =
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kBatch);
    state.SetItemsProcessed(total);
    state.SetBytesProcessed(total * static_cast<int64_t>(sizeof(uint64_t)));
}
BENCHMARK(BM_SpscThroughput)->UseRealTime();
