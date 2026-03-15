/// bench_baseline_comparison.cpp
///
/// Side-by-side latency comparison: standard UDP socket (recvmmsg) vs AF_XDP.
///
/// Methodology (mirrors test_feed_handler.cpp EndToEndLatency):
///   - SyntheticSender in sender_ns → veth0 (10.11.0.1) → veth1 (10.11.0.2)
///   - Interleaved mode: send one packet, poll for up to 50 ms, record latency
///   - Warm-up: 10 packets (1 ms apart) discarded before measurement
///   - 1000 measured packets per path
///   - Latency = pop_ns − inject_ns  (inject_ns stamped just before sendto())
///
/// Run:
///   sudo tools/setup_veth.sh create
///   sudo ./build/bench/bench_baseline_comparison
///
/// ─────────────────────────────────────────────────────────────────────────────
/// INTERPRETING RESULTS ON VM / VETH
/// ─────────────────────────────────────────────────────────────────────────────
///
/// On a VM with a veth pair, the speedup of AF_XDP over standard sockets is
/// modest (roughly 1.1–3x). This is expected and not a flaw in the benchmark.
///
/// Why: veth is a kernel virtual switching device. Regardless of whether the
/// receiving end is an AF_XDP socket or a standard SOCK_DGRAM socket, packets
/// must still traverse the kernel's virtual switching layer to cross the veth
/// pair. Hardware DMA — the primary source of AF_XDP's latency advantage — is
/// not involved. Both paths share the same virtual NIC overhead, so the numbers
/// converge.
///
/// Where AF_XDP really matters: bare-metal x86_64 with a NIC that supports XDP
/// native mode (e.g. Intel i40e / X710, Mellanox ConnectX-4/5/6). In that
/// configuration the XDP program intercepts frames at the driver level, before
/// any sk_buff is allocated, and DMA delivers frames directly into UMEM without
/// a copy. The kernel network stack is bypassed entirely. On such hardware:
///
///   Standard socket p50 : 50–100 µs   (sk_buff alloc, IP/UDP demux, sockbuf copy)
///   AF_XDP p50          :  1–10 µs    (driver-level redirect → UMEM, zero copy)
///   Expected speedup    : 10–50x at p50
///
/// The VM/veth numbers in this benchmark are honest baselines for the test
/// environment; they do not represent the production use-case speedup.

#include <arpa/inet.h>    // htons
#include <net/if.h>       // if_nametoindex
#include <sys/stat.h>     // stat
#include <time.h>         // clock_gettime, CLOCK_MONOTONIC
#include <unistd.h>       // getuid

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ultrafast/feed_handler.hpp"
#include "ultrafast/standard_socket_receiver.hpp"
#include "ultrafast/synthetic_sender.hpp"

using namespace ultrafast;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

static bool has_root() { return ::getuid() == 0; }

static bool iface_exists(const char* name) {
    return ::if_nametoindex(name) != 0;
}

static bool netns_exists(const char* name) {
    struct stat st{};
    return ::stat((std::string("/var/run/netns/") + name).c_str(), &st) == 0;
}

static uint64_t clock_ns() noexcept {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static uint64_t percentile(std::vector<uint64_t>& v, double pct) {
    if (v.empty()) return 0;
    const size_t idx = static_cast<size_t>(
        pct / 100.0 * static_cast<double>(v.size() - 1));
    return v[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Measurement kernel — identical methodology for both paths
// ─────────────────────────────────────────────────────────────────────────────

template <typename Receiver>
static std::vector<uint64_t> measure(Receiver& recv,
                                     SyntheticSender& sender,
                                     int n_packets,
                                     int& lost_out) {
    std::vector<uint64_t> lats;
    lats.reserve(static_cast<size_t>(n_packets));
    int lost = 0;

    for (int i = 0; i < n_packets; ++i) {
        sender.send_one();

        const auto deadline = std::chrono::steady_clock::now() + 50ms;
        bool got = false;

        while (std::chrono::steady_clock::now() < deadline) {
            auto ev = recv.pop();
            if (ev && ev->inject_ns > 0) {
                lats.push_back(clock_ns() - ev->inject_ns);
                got = true;
                break;
            }
            std::this_thread::sleep_for(10us);
        }
        if (!got) { ++lost; }
    }

    lost_out = lost;
    return lats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<uint64_t> run_standard_socket(int n, int& lost) {
    StandardSocketReceiver::Config cfg;
    cfg.port      = 9999;
    cfg.busy_poll = false;

    StandardSocketReceiver recv(cfg);
    recv.start();
    std::this_thread::sleep_for(20ms);  // let rx_loop enter poll

    SyntheticSender sender("10.11.0.1", "10.11.0.2", 9999, "sender_ns");

    // Warm-up: resolve ARP, fill kernel buffers, discard results.
    for (int i = 0; i < 10; ++i) {
        sender.send_one();
        std::this_thread::sleep_for(1ms);
        while (auto ev = recv.pop()) { (void)ev; }
    }
    std::this_thread::sleep_for(10ms);
    while (auto ev = recv.pop()) { (void)ev; }  // flush stragglers

    auto lats = measure(recv, sender, n, lost);

    std::printf("  rx=%" PRIu64 "  dropped=%" PRIu64 "  lost=%d\n",
                recv.packets_received(), recv.packets_dropped(), lost);
    recv.stop();
    return lats;
}

static std::vector<uint64_t> run_af_xdp(int n, int& lost) {
    FeedHandler::Config cfg;
    cfg.iface     = "veth1";
    cfg.busy_poll = false;

    FeedHandler handler(cfg);
    handler.start();
    std::this_thread::sleep_for(20ms);  // let rx_loop enter poll

    SyntheticSender sender("10.11.0.1", "10.11.0.2", 9999, "sender_ns");

    // Warm-up: prime XDP path, discard results.
    for (int i = 0; i < 10; ++i) {
        sender.send_one();
        std::this_thread::sleep_for(1ms);
        while (auto ev = handler.pop()) { (void)ev; }
    }
    std::this_thread::sleep_for(10ms);
    while (auto ev = handler.pop()) { (void)ev; }  // flush stragglers

    auto lats = measure(handler, sender, n, lost);

    std::printf("  rx=%" PRIu64 "  dropped=%" PRIu64 "  lost=%d\n",
                handler.packets_received(), handler.packets_dropped(), lost);
    handler.stop();
    return lats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Results table
// ─────────────────────────────────────────────────────────────────────────────

static void print_table(std::vector<uint64_t>& std_lats,
                        std::vector<uint64_t>& xdp_lats,
                        int                    n_packets,
                        int                    std_lost,
                        int                    xdp_lost) {
    std::sort(std_lats.begin(), std_lats.end());
    std::sort(xdp_lats.begin(), xdp_lats.end());

    struct Row {
        const char* label;
        uint64_t    std_ns;
        uint64_t    xdp_ns;
    };

    const Row rows[] = {
        {"min",   std_lats.empty() ? 0 : std_lats.front(),
                  xdp_lats.empty() ? 0 : xdp_lats.front()},
        {"p50",   percentile(std_lats, 50.0), percentile(xdp_lats, 50.0)},
        {"p99",   percentile(std_lats, 99.0), percentile(xdp_lats, 99.0)},
        {"p99.9", percentile(std_lats, 99.9), percentile(xdp_lats, 99.9)},
        {"max",   std_lats.empty() ? 0 : std_lats.back(),
                  xdp_lats.empty() ? 0 : xdp_lats.back()},
    };

    const int std_recv = static_cast<int>(std_lats.size());
    const int xdp_recv = static_cast<int>(xdp_lats.size());

    const double std_p50 = static_cast<double>(percentile(std_lats, 50.0)) / 1e3;
    const double xdp_p50 = static_cast<double>(percentile(xdp_lats, 50.0)) / 1e3;
    const double speedup = (xdp_p50 > 0.0 && std_p50 > 0.0) ? std_p50 / xdp_p50 : 0.0;

    std::printf("\n");
    std::printf("=================================================================\n");
    std::printf("  Baseline Comparison: Standard UDP (recvmmsg) vs AF_XDP\n");
    std::printf("  %d packets, veth pair (sender_ns -> veth1), interleaved mode\n",
                n_packets);
    std::printf("=================================================================\n");
    std::printf("  samples  : std_socket %d/%d (lost %d)  |  af_xdp %d/%d (lost %d)\n",
                std_recv, n_packets, std_lost,
                xdp_recv, n_packets, xdp_lost);
    std::printf("-----------------------------------------------------------------\n");
    std::printf("  %-8s   %16s   %16s   %8s\n",
                "Metric", "Std Socket (us)", "AF_XDP (us)", "Speedup");
    std::printf("  %-8s   %16s   %16s   %8s\n",
                "--------", "---------------", "----------", "-------");

    for (const auto& r : rows) {
        const double s_us = static_cast<double>(r.std_ns) / 1e3;
        const double x_us = static_cast<double>(r.xdp_ns) / 1e3;
        const double sp   = (x_us > 0.0 && s_us > 0.0) ? s_us / x_us : 0.0;
        std::printf("  %-8s   %16.2f   %16.2f   %7.2fx\n",
                    r.label, s_us, x_us, sp);
    }

    std::printf("-----------------------------------------------------------------\n");
    if (speedup > 1.0) {
        std::printf("  AF_XDP is %.2fx faster than standard sockets at p50\n", speedup);
    } else if (speedup > 0.0) {
        std::printf("  Standard sockets are %.2fx faster than AF_XDP at p50\n",
                    1.0 / speedup);
    } else {
        std::printf("  (insufficient samples for p50 speedup calculation)\n");
    }
    std::printf("=================================================================\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    if (!has_root()) {
        std::fprintf(stderr,
            "bench_baseline_comparison: must run as root (CAP_NET_ADMIN required)\n"
            "  Use: sudo ./build/bench/bench_baseline_comparison\n");
        return 1;
    }

    if (!iface_exists("veth1") || !netns_exists("sender_ns")) {
        std::fprintf(stderr,
            "bench_baseline_comparison: veth pair or sender_ns not found.\n"
            "  Run: sudo tools/setup_veth.sh create\n");
        return 1;
    }

    constexpr int kPackets = 1000;

    std::printf("=== Baseline Comparison Benchmark ===\n");
    std::printf("Packets per run : %d\n", kPackets);
    std::printf("Measurement     : inject_ns (pre-sendto) -> pop_ns (post-ring-pop)\n\n");

    std::printf("--- Pass 1: Standard Socket (SOCK_DGRAM + recvmmsg) ---\n");
    int std_lost = 0;
    auto std_lats = run_standard_socket(kPackets, std_lost);

    // Allow the XDP socket to bind cleanly without a port-in-use race.
    std::this_thread::sleep_for(200ms);

    std::printf("\n--- Pass 2: AF_XDP (XDP_FLAGS_SKB_MODE + XDP_COPY, veth1) ---\n");
    int xdp_lost = 0;
    auto xdp_lats = run_af_xdp(kPackets, xdp_lost);

    print_table(std_lats, xdp_lats, kPackets, std_lost, xdp_lost);

    return 0;
}
