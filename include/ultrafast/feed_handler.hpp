#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include <linux/if_link.h>   // XDP_FLAGS_SKB_MODE
#include <linux/if_xdp.h>    // XDP_COPY

#include "ultrafast/feed_event.hpp"
#include "ultrafast/spsc_ring_buffer.hpp"
#include "ultrafast/umem.hpp"
#include "ultrafast/xdp_socket.hpp"

namespace ultrafast {

/// Ties together Umem + XdpSocket + SpscRingBuffer<FeedEvent> into a
/// threaded RX loop for AF_XDP market-data ingestion.
///
/// Usage:
///   FeedHandler handler(cfg);
///   handler.start();
///   while (running) {
///       if (auto ev = handler.pop()) process(*ev);
///   }
///   handler.stop();
///
/// Thread safety:
///   start() / stop() / packets_received() / packets_dropped() — any thread.
///   pop() — single consumer thread only.
class FeedHandler {
public:
    struct Config {
        std::string iface      = "veth1";
        int         queue_id   = 0;
        /// XDP programme flags. XDP_FLAGS_SKB_MODE is required for veth and
        /// loopback where native-mode driver support is absent.
        uint32_t    xdp_flags  = XDP_FLAGS_SKB_MODE;
        /// Socket bind flags. XDP_COPY is required for veth (no zero-copy DMA).
        uint16_t    bind_flags = XDP_COPY;
        /// Maximum descriptors consumed per rx_burst() call.
        int         rx_batch   = 64;
        /// true = spin on empty RX ring (_mm_pause); false = poll(1 ms).
        bool        busy_poll  = false;
    };

    explicit FeedHandler(const Config& cfg);
    ~FeedHandler();

    FeedHandler(const FeedHandler&)            = delete;
    FeedHandler& operator=(const FeedHandler&) = delete;

    /// Prime the fill ring and launch the RX thread.
    /// Must not be called while the handler is already running.
    void start();

    /// Signal the RX thread to stop and join it. Safe to call more than once.
    void stop();

    /// Consumer-side drain. Returns the next FeedEvent or std::nullopt if
    /// the ring is empty. Must be called from a single consumer thread only.
    [[nodiscard]] std::optional<FeedEvent> pop() noexcept;

    [[nodiscard]] uint64_t packets_received() const noexcept;
    [[nodiscard]] uint64_t packets_dropped()  const noexcept;

    /// CLOCK_MONOTONIC nanoseconds — public so callers can compute latency
    /// without duplicating the clock_gettime boilerplate.
    static uint64_t clock_ns() noexcept;

private:
    void rx_loop();

    /// Ring large enough that even a brief consumer stall won't cause drops
    /// at typical market-data rates. 2^16 × sizeof(FeedEvent) ≈ 6 MiB.
    static constexpr std::size_t kRingCap = 65536;

    Config    cfg_;
    Umem      umem_;    ///< Must be declared before socket_ — socket_ holds a Umem&
    XdpSocket socket_;
    SpscRingBuffer<FeedEvent, kRingCap> ring_;

    std::atomic<bool>     stop_flag_{false};
    std::thread           rx_thread_;
    std::atomic<uint64_t> pkts_rx_{0};
    std::atomic<uint64_t> pkts_dropped_{0};
};

} // namespace ultrafast
