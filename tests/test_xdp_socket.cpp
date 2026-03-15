#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <unistd.h>       // getuid

#include <linux/if_link.h>   // XDP_FLAGS_SKB_MODE

#include "ultrafast/umem.hpp"
#include "ultrafast/xdp_socket.hpp"

using namespace ultrafast;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool has_root() {
    return getuid() == 0;
}

/// Try to open an XDP socket on "lo" in SKB mode (the most permissive mode,
/// no driver support required).  Returns nullptr and skips if "lo" rejects XDP.
static std::unique_ptr<XdpSocket> make_loopback_socket(Umem& umem) {
    XdpSocketConfig cfg;
    cfg.iface     = "lo";
    cfg.queue_id  = 0;
    cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
    try {
        return std::make_unique<XdpSocket>(umem, cfg);
    } catch (const std::runtime_error& e) {
        // Some kernel configs reject XDP on lo (e.g. in GCP test VMs without
        // driver support); skip gracefully rather than failing the suite.
        return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(XdpSocket, ConstructionFailsOnBadInterface) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;
    XdpSocketConfig cfg;
    cfg.iface    = "__does_not_exist__";
    cfg.queue_id = 0;

    EXPECT_THROW(XdpSocket(umem, cfg), std::runtime_error);
}

TEST(XdpSocket, ValidAfterConstruction) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;
    auto sock = make_loopback_socket(umem);
    if (!sock) GTEST_SKIP() << "XDP on loopback unavailable in this environment";

    EXPECT_TRUE(sock->valid());
    EXPECT_GE(sock->fd(), 0);
}

TEST(XdpSocket, MoveConstructor) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;
    auto a = make_loopback_socket(umem);
    if (!a) GTEST_SKIP() << "XDP on loopback unavailable in this environment";

    const int saved_fd = a->fd();
    XdpSocket b(std::move(*a));

    EXPECT_FALSE(a->valid());
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.fd(), saved_fd);
}

TEST(XdpSocket, MoveAssignment) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;

    auto a = make_loopback_socket(umem);
    if (!a) GTEST_SKIP() << "XDP on loopback unavailable in this environment";

    // Create a second socket to assign into
    XdpSocketConfig cfg2;
    cfg2.iface     = "lo";
    cfg2.queue_id  = 0;
    cfg2.xdp_flags = XDP_FLAGS_SKB_MODE;

    // b may fail if only one queue is allowed; use std::optional to handle
    std::unique_ptr<XdpSocket> b;
    try {
        b = std::make_unique<XdpSocket>(umem, cfg2);
    } catch (...) {
        GTEST_SKIP() << "Could not create second XDP socket (single-queue limit)";
    }

    const int fd_a = a->fd();
    *b = std::move(*a);

    EXPECT_FALSE(a->valid());
    EXPECT_TRUE(b->valid());
    EXPECT_EQ(b->fd(), fd_a);
}

TEST(XdpSocket, RxBurstReturnsZeroWithNoTraffic) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;
    auto sock = make_loopback_socket(umem);
    if (!sock) GTEST_SKIP() << "XDP on loopback unavailable in this environment";

    umem.populate_fill_ring();

    std::array<RxDesc, 64> descs;
    const std::size_t n = sock->rx_burst(descs);

    // No packets were sent, so zero frames should be available.
    EXPECT_EQ(n, 0u);
}

TEST(XdpSocket, TxBurstWithNoFrames) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;
    auto sock = make_loopback_socket(umem);
    if (!sock) GTEST_SKIP() << "XDP on loopback unavailable in this environment";

    // Passing count=0 must be a safe no-op.
    const std::size_t enqueued = sock->tx_burst(nullptr, nullptr, 0);
    EXPECT_EQ(enqueued, 0u);
}

TEST(XdpSocket, TxCompleteWithEmptyCompletionRing) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem umem;
    auto sock = make_loopback_socket(umem);
    if (!sock) GTEST_SKIP() << "XDP on loopback unavailable in this environment";

    std::array<uint64_t, 64> addrs{};
    const std::size_t n = sock->tx_complete(addrs.data(), addrs.size());
    EXPECT_EQ(n, 0u);
}
