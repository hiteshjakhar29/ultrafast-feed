#include "ultrafast/xdp_socket.hpp"

#include <sys/socket.h>   // sendto, MSG_DONTWAIT

#include <cerrno>
#include <cstring>        // strerror
#include <format>
#include <stdexcept>
#include <utility>        // std::move

namespace ultrafast {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

XdpSocket::XdpSocket(Umem& umem, const XdpSocketConfig& cfg)
    : umem_(&umem), cfg_(cfg)
{
    create_socket();
}

XdpSocket::~XdpSocket() noexcept {
    close_socket();
}

// ─────────────────────────────────────────────────────────────────────────────
// Move semantics
// ─────────────────────────────────────────────────────────────────────────────

XdpSocket::XdpSocket(XdpSocket&& other) noexcept
    : umem_(other.umem_)
    , cfg_(std::move(other.cfg_))
    , socket_(other.socket_)
    , rx_ring_(other.rx_ring_)
    , tx_ring_(other.tx_ring_)
{
    other.socket_ = nullptr;
}

XdpSocket& XdpSocket::operator=(XdpSocket&& other) noexcept {
    if (this != &other) {
        close_socket();
        umem_    = other.umem_;
        cfg_     = std::move(other.cfg_);
        socket_  = other.socket_;
        rx_ring_ = other.rx_ring_;
        tx_ring_ = other.tx_ring_;
        other.socket_ = nullptr;
    }
    return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void XdpSocket::create_socket() {
    // Zero-init first for forward ABI compatibility.
    struct xsk_socket_config cfg = {};
    cfg.rx_size      = static_cast<uint32_t>(cfg_.rx_size);
    cfg.tx_size      = static_cast<uint32_t>(cfg_.tx_size);
    cfg.xdp_flags    = cfg_.xdp_flags;
    cfg.bind_flags   = cfg_.bind_flags;
    cfg.libbpf_flags = 0;

    const int ret = xsk_socket__create(
        &socket_,
        cfg_.iface.c_str(),
        cfg_.queue_id,
        umem_->handle(),
        &rx_ring_,
        &tx_ring_,
        &cfg);

    if (ret != 0) {
        throw std::runtime_error(
            std::format("xsk_socket__create('{}', q{}): {}",
                        cfg_.iface, cfg_.queue_id, strerror(-ret)));
    }
}

void XdpSocket::close_socket() noexcept {
    if (socket_) {
        xsk_socket__delete(socket_);
        socket_ = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API — file descriptor
// ─────────────────────────────────────────────────────────────────────────────

int XdpSocket::fd() const noexcept {
    return xsk_socket__fd(socket_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Receive path
// ─────────────────────────────────────────────────────────────────────────────

std::size_t XdpSocket::rx_burst(std::span<RxDesc> out_descs) noexcept {
    uint32_t idx = 0;
    const std::size_t rcvd = xsk_ring_cons__peek(
        &rx_ring_,
        static_cast<uint32_t>(out_descs.size()),
        &idx);

    for (std::size_t i = 0; i < rcvd; ++i) {
        const struct xdp_desc* d = xsk_ring_cons__rx_desc(
            &rx_ring_, idx + static_cast<uint32_t>(i));
        out_descs[i] = RxDesc{d->addr, d->len, d->options};
    }

    if (rcvd > 0) {
        xsk_ring_cons__release(&rx_ring_, static_cast<uint32_t>(rcvd));
    }
    return rcvd;
}

void XdpSocket::rx_release(std::span<const RxDesc> descs) noexcept {
    if (descs.empty()) return;

    uint32_t idx = 0;
    const uint32_t reserved = xsk_ring_prod__reserve(
        &umem_->fill_ring(),
        static_cast<uint32_t>(descs.size()),
        &idx);

    for (uint32_t i = 0; i < reserved; ++i) {
        *xsk_ring_prod__fill_addr(&umem_->fill_ring(), idx + i) = descs[i].addr;
    }

    xsk_ring_prod__submit(&umem_->fill_ring(), reserved);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transmit path
// ─────────────────────────────────────────────────────────────────────────────

std::size_t XdpSocket::tx_burst(const uint64_t* addrs,
                                 const uint32_t* lens,
                                 std::size_t count) noexcept {
    if (count == 0) return 0;

    uint32_t idx = 0;
    const uint32_t reserved = xsk_ring_prod__reserve(
        &tx_ring_, static_cast<uint32_t>(count), &idx);

    for (uint32_t i = 0; i < reserved; ++i) {
        struct xdp_desc* d = xsk_ring_prod__tx_desc(&tx_ring_, idx + i);
        d->addr    = addrs[i];
        d->len     = lens[i];
        d->options = 0;
    }

    if (reserved > 0) {
        xsk_ring_prod__submit(&tx_ring_, reserved);
    }
    return static_cast<std::size_t>(reserved);
}

void XdpSocket::tx_kick() noexcept {
    if (xsk_ring_prod__needs_wakeup(&tx_ring_)) {
        // sendto() on an AF_XDP socket signals the kernel to drain the TX ring.
        // The return value is intentionally ignored — errors are non-fatal here.
        (void)sendto(fd(), nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    }
}

std::size_t XdpSocket::tx_complete(uint64_t* addrs,
                                    std::size_t max_count) noexcept {
    uint32_t idx = 0;
    const std::size_t n = xsk_ring_cons__peek(
        &umem_->completion_ring(),
        static_cast<uint32_t>(max_count),
        &idx);

    for (std::size_t i = 0; i < n; ++i) {
        addrs[i] = *xsk_ring_cons__comp_addr(
            &umem_->completion_ring(), idx + static_cast<uint32_t>(i));
    }

    if (n > 0) {
        xsk_ring_cons__release(
            &umem_->completion_ring(), static_cast<uint32_t>(n));
    }
    return n;
}

} // namespace ultrafast
