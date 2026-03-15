#include <gtest/gtest.h>

#include <arpa/inet.h>    // inet_ntoa, ntohl
#include <net/if.h>       // if_nametoindex
#include <sys/stat.h>     // stat
#include <unistd.h>       // getuid

#include <algorithm>      // std::sort
#include <chrono>
#include <cinttypes>      // PRIu64
#include <cstdio>
#include <thread>
#include <vector>

#include "ultrafast/feed_handler.hpp"
#include "ultrafast/synthetic_sender.hpp"

using namespace ultrafast;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool has_root() {
    return ::getuid() == 0;
}

static bool iface_exists(const char* name) {
    return ::if_nametoindex(name) != 0;
}

static bool netns_exists(const char* name) {
    struct stat st{};
    const std::string path = std::string("/var/run/netns/") + name;
    return ::stat(path.c_str(), &st) == 0;
}

static uint64_t percentile(std::vector<uint64_t>& v, double pct) {
    if (v.empty()) return 0;
    const std::size_t idx = static_cast<std::size_t>(
        pct / 100.0 * static_cast<double>(v.size() - 1));
    return v[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture — skip if not root or veth pair absent
// ─────────────────────────────────────────────────────────────────────────────

class FeedHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!has_root()) {
            GTEST_SKIP() << "Requires CAP_NET_ADMIN (run as root)";
        }
        if (!iface_exists("veth1") || !netns_exists("sender_ns")) {
            GTEST_SKIP()
                << "veth pair / sender_ns not present — run: sudo tools/setup_veth.sh create";
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Smoke test: FeedHandler opens veth1 without throwing
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FeedHandlerTest, ConstructionSucceeds) {
    FeedHandler::Config cfg;
    cfg.iface = "veth1";
    EXPECT_NO_THROW({ FeedHandler handler(cfg); });
}

// ─────────────────────────────────────────────────────────────────────────────
// Smoke test: start + stop with no traffic
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FeedHandlerTest, StartStop) {
    FeedHandler::Config cfg;
    cfg.iface = "veth1";

    FeedHandler handler(cfg);
    EXPECT_NO_THROW({
        handler.start();
        std::this_thread::sleep_for(20ms);
        handler.stop();
    });
    EXPECT_EQ(handler.packets_received(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end latency: inject UDP on veth0 → pop from ring → p50/p99/p99.9
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FeedHandlerTest, EndToEndLatency) {
    FeedHandler::Config cfg;
    cfg.iface     = "veth1";
    cfg.busy_poll = false;

    FeedHandler handler(cfg);
    handler.start();

    // Give the RX thread time to enter its poll loop before injecting.
    std::this_thread::sleep_for(20ms);

    // SyntheticSender enters sender_ns, binds to veth0 (10.11.0.1), and sends
    // to veth1 (10.11.0.2) — packets physically cross the veth pair.
    SyntheticSender sender("10.11.0.1", "10.11.0.2", 9999, "sender_ns");

    constexpr int kPackets = 1000;

    // Warm-up: resolve ARP and prime the XDP path before measuring.
    // Send 10 packets with 1 ms gaps and discard any results.
    for (int i = 0; i < 10; ++i) {
        sender.send_one();
        std::this_thread::sleep_for(1ms);
        // drain without recording
        while (auto ev = handler.pop()) { (void)ev; }
    }
    // Extra settle time after warm-up.
    std::this_thread::sleep_for(10ms);
    while (auto ev = handler.pop()) { (void)ev; }  // flush any stragglers

    // Interleaved mode: send one packet, wait up to 50 ms for it to appear
    // in the ring, record latency, repeat. This gives true per-packet numbers
    // rather than inject→pop latency across a batched send+drain design.
    std::vector<uint64_t> latencies;
    latencies.reserve(kPackets);

    int lost = 0;
    for (int i = 0; i < kPackets; ++i) {
        sender.send_one();

        const auto deadline = std::chrono::steady_clock::now() + 50ms;
        bool got = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto ev = handler.pop();
            if (ev && ev->inject_ns > 0) {
                const uint64_t pop_ns = FeedHandler::clock_ns();
                latencies.push_back(pop_ns - ev->inject_ns);
                got = true;
                break;
            }
            std::this_thread::sleep_for(10us);
        }
        if (!got) {
            ++lost;
        }
    }

    handler.stop();

    // Allow up to 10% loss (ARP, XDP warm-up, first few packets).
    const int received = static_cast<int>(latencies.size());
    ASSERT_GE(received, kPackets * 90 / 100)
        << "Only received " << received << "/" << kPackets << " packets "
        << "(lost=" << lost << "). "
        << "rx=" << handler.packets_received()
        << " dropped=" << handler.packets_dropped();

    std::sort(latencies.begin(), latencies.end());

    const uint64_t p50  = percentile(latencies, 50.0);
    const uint64_t p99  = percentile(latencies, 99.0);
    const uint64_t p999 = percentile(latencies, 99.9);
    const uint64_t pmin = latencies.front();
    const uint64_t pmax = latencies.back();

    std::printf("\n--- AF_XDP end-to-end latency (inject → ring pop) ---\n");
    std::printf("  samples : %d / %d\n", received, kPackets);
    std::printf("  min     : %7" PRIu64 " ns  (%.2f µs)\n", pmin,  pmin  / 1e3);
    std::printf("  p50     : %7" PRIu64 " ns  (%.2f µs)\n", p50,   p50   / 1e3);
    std::printf("  p99     : %7" PRIu64 " ns  (%.2f µs)\n", p99,   p99   / 1e3);
    std::printf("  p99.9   : %7" PRIu64 " ns  (%.2f µs)\n", p999,  p999  / 1e3);
    std::printf("  max     : %7" PRIu64 " ns  (%.2f µs)\n", pmax,  pmax  / 1e3);
    std::printf("  rx_pkts : %" PRIu64 "  dropped: %" PRIu64 "  lost: %d\n",
                handler.packets_received(), handler.packets_dropped(), lost);
    std::fflush(stdout);

    // p99 < 10 ms — reasonable for a VM with veth (interleaved, per-packet).
    EXPECT_LT(p99, 10'000'000ULL)
        << "p99 exceeded 10 ms — check system load or offload settings";
}
