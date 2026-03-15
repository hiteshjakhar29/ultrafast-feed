#include <benchmark/benchmark.h>

#include <arpa/inet.h>   // htons
#include <cstdint>
#include <cstring>       // memset

#include <xdp/xsk.h>     // XSK_UMEM__DEFAULT_FRAME_SIZE

#include "ultrafast/packet.hpp"
#include "ultrafast/xdp_socket.hpp"

using namespace ultrafast;

// ─────────────────────────────────────────────────────────────────────────────
// These benchmarks measure the user-space overhead of the critical-path data
// structures.  They do NOT open AF_XDP sockets (no root required).
// ─────────────────────────────────────────────────────────────────────────────

/// Cost of computing a UMEM frame offset: pure integer arithmetic.
static void BM_UmemFrameOffset(benchmark::State& state) {
    const uint64_t frame_size  = XSK_UMEM__DEFAULT_FRAME_SIZE;  // 4096
    const std::size_t n_frames = 4096;
    std::size_t idx = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(static_cast<uint64_t>(idx) * frame_size);
        idx = (idx + 1) % n_frames;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_UmemFrameOffset);

/// Cost of building a well-formed Eth/IPv4/UDP Packet and calling udp().
/// Models the receive fast-path: one pointer-cast chain per incoming frame.
static void BM_PacketParseFull(benchmark::State& state) {
    // Build a minimal but valid Eth/IPv4/UDP frame in stack memory.
    alignas(64) uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    auto* eth      = reinterpret_cast<EthHeader*>(buf);
    eth->ether_type = htons(0x0800);

    auto* ip        = reinterpret_cast<IpHeader*>(buf + sizeof(EthHeader));
    ip->version_ihl = 0x45;   // IPv4, 20-byte header
    ip->protocol    = 17;     // UDP
    ip->total_length = htons(28);  // 20 IP + 8 UDP

    auto* udp       = reinterpret_cast<UdpHeader*>(buf + sizeof(EthHeader) + 20);
    udp->src_port   = htons(12345);
    udp->dst_port   = htons(9999);
    udp->length     = htons(8);  // header only, no payload

    Packet pkt{buf, sizeof(buf)};

    for (auto _ : state) {
        benchmark::DoNotOptimize(pkt.udp());
        benchmark::DoNotOptimize(pkt.payload_len());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PacketParseFull);

/// Cost of filling a batch of RxDesc structs — simulates the descriptor loop
/// that runs after rx_burst() returns.
static void BM_RxDescBatchFill(benchmark::State& state) {
    static constexpr std::size_t kBatch = 64;
    std::array<RxDesc, kBatch> descs;
    const uint64_t frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;

    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            descs[i] = RxDesc{
                static_cast<uint64_t>(i) * frame_size,
                static_cast<uint32_t>(frame_size),
                0
            };
        }
        benchmark::DoNotOptimize(descs.data());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                             static_cast<int64_t>(kBatch));
}
BENCHMARK(BM_RxDescBatchFill);

/// Throughput of copying packet payloads out of a simulated UMEM region
/// into a local buffer — measures memory bandwidth on the critical RX path.
static void BM_UmemPayloadCopy(benchmark::State& state) {
    static constexpr std::size_t kFrameSize  = 4096;
    static constexpr std::size_t kPayloadOff = sizeof(EthHeader) + 20 + sizeof(UdpHeader);
    static constexpr std::size_t kPayloadLen = 64;  // typical small market-data msg

    alignas(64) uint8_t src[kFrameSize]{};
    alignas(64) uint8_t dst[kPayloadLen]{};

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            memcpy(dst, src + kPayloadOff, kPayloadLen));
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(kPayloadLen));
}
BENCHMARK(BM_UmemPayloadCopy);
