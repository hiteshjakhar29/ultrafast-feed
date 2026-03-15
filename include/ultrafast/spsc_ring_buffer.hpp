#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace ultrafast {

/// Single-Producer Single-Consumer lock-free ring buffer.
///
/// Template parameters:
///   T — element type; must be trivially copyable.
///   N — capacity; must be a power of two.  Usable slots = N - 1.
///
/// Thread safety:
///   push() / push(T&&) — call from the single producer thread only.
///   pop() / peek()     — call from the single consumer thread only.
///   empty() / full() / size() — safe to call from either thread but the
///   result may be stale by the time the caller acts on it.
///
/// Memory ordering:
///   tail_ (producer index): relaxed load by producer; release store after write.
///   head_ (consumer index): acquire load by producer to check fullness;
///                           relaxed load by consumer; release store after read.
///   This is the canonical acquire/release SPSC ordering — no explicit fences.
///
/// Cache-line layout:
///   tail_ and head_ are each padded to their own 64-byte cache line to
///   prevent false sharing between the producer and consumer cores.

template <typename T, std::size_t N>
class SpscRingBuffer {
    static_assert(N >= 2 && (N & (N - 1)) == 0,
        "SpscRingBuffer: N must be a power of two and >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
        "SpscRingBuffer: T must be trivially copyable for lock-free safety");

    // Use the literal 64 to avoid the GCC ABI warning on
    // std::hardware_destructive_interference_size.
    static constexpr std::size_t kCacheLine = 64;
    static constexpr std::size_t kMask      = N - 1;

    // Producer advances tail_.  Written by producer, read by consumer.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};

    // Consumer advances head_.  Written by consumer, read by producer.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};

    // Storage on its own cache lines to avoid false sharing with the indices.
    alignas(kCacheLine) std::array<T, N> buf_{};

public:
    SpscRingBuffer() noexcept = default;

    // Non-copyable, non-movable (atomics cannot be copied/moved).
    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // ── Producer interface ────────────────────────────────────────────────────

    /// Enqueue item.  Returns false (non-blocking) if the ring is full.
    /// Must be called from the producer thread only.
    [[nodiscard]] bool push(const T& item) noexcept {
        const std::size_t t      = tail_.load(std::memory_order_relaxed);
        const std::size_t next_t = (t + 1U) & kMask;
        if (next_t == head_.load(std::memory_order_acquire)) {
            return false;   // full
        }
        buf_[t] = item;
        tail_.store(next_t, std::memory_order_release);
        return true;
    }

    /// Move-enqueue overload.
    [[nodiscard]] bool push(T&& item) noexcept {
        const std::size_t t      = tail_.load(std::memory_order_relaxed);
        const std::size_t next_t = (t + 1U) & kMask;
        if (next_t == head_.load(std::memory_order_acquire)) {
            return false;
        }
        buf_[t] = std::move(item);
        tail_.store(next_t, std::memory_order_release);
        return true;
    }

    // ── Consumer interface ────────────────────────────────────────────────────

    /// Dequeue an item.  Returns std::nullopt (non-blocking) if the ring is empty.
    /// Must be called from the consumer thread only.
    [[nodiscard]] std::optional<T> pop() noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;    // empty
        }
        T item = buf_[h];
        head_.store((h + 1U) & kMask, std::memory_order_release);
        return item;
    }

    /// Peek at the front element without consuming it.
    /// Returns nullptr if empty.  Must be called from the consumer thread only.
    [[nodiscard]] const T* peek() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return &buf_[h];
    }

    // ── State queries (approximate — race-free only in single-threaded use) ──

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return ((t + 1U) & kMask) == h;
    }

    /// Returns the number of items currently in the ring.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return (t - h + N) & kMask;
    }

    static constexpr std::size_t capacity() noexcept { return N; }
};

} // namespace ultrafast
