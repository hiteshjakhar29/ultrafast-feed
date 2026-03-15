#include "ultrafast/feed_handler.hpp"

#include <arpa/inet.h>    // ntohs
#include <poll.h>         // poll, pollfd, POLLIN
#include <time.h>         // clock_gettime, CLOCK_MONOTONIC

#include <array>
#include <cstring>        // memcpy

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>  // _mm_pause
#  define UF_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define UF_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#  define UF_CPU_RELAX() ((void)0)
#endif

#include "ultrafast/packet.hpp"

namespace ultrafast {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helper: build XdpSocketConfig from FeedHandler::Config
// ─────────────────────────────────────────────────────────────────────────────

static XdpSocketConfig make_xdp_cfg(const FeedHandler::Config& cfg) {
    XdpSocketConfig c;
    c.iface      = cfg.iface;
    c.queue_id   = static_cast<uint32_t>(cfg.queue_id);
    c.xdp_flags  = cfg.xdp_flags;
    c.bind_flags = cfg.bind_flags;
    // rx_size / tx_size keep their defaults (2048 each)
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

FeedHandler::FeedHandler(const Config& cfg)
    : cfg_(cfg)
    , umem_()                               // UmemConfig defaults: 4096 frames, huge-page fallback
    , socket_(umem_, make_xdp_cfg(cfg))
{}

FeedHandler::~FeedHandler() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void FeedHandler::start() {
    stop_flag_.store(false, std::memory_order_relaxed);
    // Prime the fill ring once so the kernel has UMEM frames for DMA.
    // rx_release() handles replenishment after each rx_burst() batch.
    umem_.populate_fill_ring();
    rx_thread_ = std::thread(&FeedHandler::rx_loop, this);
}

void FeedHandler::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Consumer API
// ─────────────────────────────────────────────────────────────────────────────

std::optional<FeedEvent> FeedHandler::pop() noexcept {
    return ring_.pop();
}

uint64_t FeedHandler::packets_received() const noexcept {
    return pkts_rx_.load(std::memory_order_relaxed);
}

uint64_t FeedHandler::packets_dropped() const noexcept {
    return pkts_dropped_.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Clock
// ─────────────────────────────────────────────────────────────────────────────

uint64_t FeedHandler::clock_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// ─────────────────────────────────────────────────────────────────────────────
// RX loop — runs on rx_thread_
// ─────────────────────────────────────────────────────────────────────────────

void FeedHandler::rx_loop() {
    constexpr int kMaxBatch = 64;
    std::array<RxDesc, kMaxBatch> descs;

    struct pollfd pfd{};
    pfd.fd     = socket_.fd();
    pfd.events = POLLIN;

    while (!stop_flag_.load(std::memory_order_relaxed)) {

        const std::size_t n = socket_.rx_burst({descs.data(), kMaxBatch});

        if (n == 0) {
            if (cfg_.busy_poll) {
                UF_CPU_RELAX();
            } else {
                poll(&pfd, 1, 1 /*ms*/);
            }
            continue;
        }

        // Timestamp the batch once — one clock_gettime() amortised over n frames.
        const uint64_t batch_ts = clock_ns();
        pkts_rx_.fetch_add(n, std::memory_order_relaxed);

        for (std::size_t i = 0; i < n; ++i) {
            // RxDesc::addr is a byte offset into the UMEM region, not a frame index.
            // Umem::frame_ptr(idx) takes an index, so we compute the host pointer
            // directly via base() + offset.
            const uint8_t* frame =
                static_cast<const uint8_t*>(umem_.base()) + descs[i].addr;
            Packet pkt{frame, descs[i].len};

            if (!pkt.is_udp()) continue;

            FeedEvent ev{};
            ev.receive_ns = batch_ts;
            ev.src_port   = ntohs(pkt.udp()->src_port);
            ev.dst_port   = ntohs(pkt.udp()->dst_port);
            ev.src_ip     = pkt.ip()->src_ip;

            const uint8_t*  pay  = pkt.payload();
            const std::size_t plen = pkt.payload_len();

            // Extract inject timestamp written by SyntheticSender into payload[0..7].
            if (pay != nullptr && plen >= sizeof(uint64_t)) {
                std::memcpy(&ev.inject_ns, pay, sizeof(uint64_t));
            }

            ev.payload_len = static_cast<uint16_t>(
                std::min(plen, FeedEvent::kMaxPayload));
            if (pay != nullptr && ev.payload_len > 0) {
                std::memcpy(ev.payload, pay, ev.payload_len);
            }

            if (!ring_.push(ev)) {
                pkts_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Return the entire batch of UMEM frames to the fill ring for kernel reuse.
        // We do this *after* the memcpy loop — frames must stay valid until here.
        socket_.rx_release({descs.data(), n});
    }
}

} // namespace ultrafast
