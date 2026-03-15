#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <linux/if_link.h>   // XDP_FLAGS_DRV_MODE, XDP_FLAGS_SKB_MODE, ...
#include <linux/if_xdp.h>    // XDP_USE_NEED_WAKEUP, XDP_COPY, XDP_ZEROCOPY
#include <xdp/xsk.h>         // xsk_socket, xsk_ring_cons, xsk_ring_prod, ...

#include "ultrafast/umem.hpp"

namespace ultrafast {

/// Configuration for a single AF_XDP socket.
struct XdpSocketConfig {
    std::string iface;                                          ///< Network interface name, e.g. "eth0"
    uint32_t    queue_id    = 0;                               ///< NIC RX/TX queue index
    std::size_t rx_size     = XSK_RING_CONS__DEFAULT_NUM_DESCS; ///< RX ring depth (power of 2)
    std::size_t tx_size     = XSK_RING_PROD__DEFAULT_NUM_DESCS; ///< TX ring depth (power of 2)
    /// XDP programme flags (XDP_FLAGS_DRV_MODE / XDP_FLAGS_SKB_MODE / ...).
    /// 0 means let the kernel pick the best available mode.
    uint32_t    xdp_flags   = 0;
    /// Socket bind flags (XDP_USE_NEED_WAKEUP recommended for reduced CPU).
    uint16_t    bind_flags  = XDP_USE_NEED_WAKEUP;
};

/// Descriptor for a single received frame — an index into the UMEM region.
struct RxDesc {
    uint64_t addr;     ///< UMEM offset; pass to Umem::frame_ptr() to get a host pointer
    uint32_t len;      ///< Received byte count
    uint32_t options;  ///< xdp_desc options field (usually 0)
};

/// RAII wrapper around xsk_socket.
///
/// The supplied Umem must outlive this object.  Constructor throws
/// std::runtime_error on failure (interface not found, no CAP_NET_ADMIN, etc.).
///
/// Non-copyable; movable.
class XdpSocket {
public:
    /// Creates and binds an AF_XDP socket on umem using cfg.
    XdpSocket(Umem& umem, const XdpSocketConfig& cfg);
    ~XdpSocket() noexcept;

    XdpSocket(XdpSocket&& other) noexcept;
    XdpSocket& operator=(XdpSocket&& other) noexcept;

    XdpSocket(const XdpSocket&)            = delete;
    XdpSocket& operator=(const XdpSocket&) = delete;

    // ── File descriptor ───────────────────────────────────────────────────────

    /// AF_XDP socket file descriptor, suitable for poll() / epoll().
    [[nodiscard]] int fd() const noexcept;

    [[nodiscard]] bool valid() const noexcept { return socket_ != nullptr; }

    // ── Receive path ──────────────────────────────────────────────────────────

    /// Non-blocking RX burst.  Fills out_descs with up to out_descs.size()
    /// received frame descriptors.  Returns the number of descriptors written.
    ///
    /// After processing the frames, call rx_release() to return them to the
    /// UMEM fill ring so the kernel can reuse those buffers.
    [[nodiscard]] std::size_t rx_burst(std::span<RxDesc> out_descs) noexcept;

    /// Return the frames described by descs back to the UMEM fill ring.
    void rx_release(std::span<const RxDesc> descs) noexcept;

    // ── Transmit path ─────────────────────────────────────────────────────────

    /// Non-blocking TX burst.  Enqueues up to count frames for transmission.
    ///   addrs — UMEM offsets of the frames to send
    ///   lens  — byte length of each frame
    /// Returns the number of frames actually enqueued (may be < count if TX ring full).
    ///
    /// After calling tx_burst(), call tx_kick() if XDP_USE_NEED_WAKEUP is set.
    [[nodiscard]] std::size_t tx_burst(const uint64_t* addrs,
                                       const uint32_t* lens,
                                       std::size_t count) noexcept;

    /// Kick the kernel to process pending TX frames.
    /// A no-op when xsk_ring_prod__needs_wakeup() returns false (busy-poll mode).
    void tx_kick() noexcept;

    /// Reclaim completed TX frame offsets from the completion ring so they can
    /// be reused.  Writes up to max_count offsets into addrs[].
    /// Returns the number of frames reclaimed.
    [[nodiscard]] std::size_t tx_complete(uint64_t* addrs,
                                          std::size_t max_count) noexcept;

private:
    void create_socket();
    void close_socket() noexcept;

    Umem*           umem_   = nullptr;
    XdpSocketConfig cfg_;
    xsk_socket*     socket_ = nullptr;
    xsk_ring_cons   rx_ring_{};
    xsk_ring_prod   tx_ring_{};
};

} // namespace ultrafast
