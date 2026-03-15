[![CI](https://github.com/hiteshjakhar29/ultrafast-feed/actions/workflows/ci.yml/badge.svg)](https://github.com/hiteshjakhar29/ultrafast-feed/actions/workflows/ci.yml)

# ultrafast-feed

A C++20 market data feed handler built on AF_XDP kernel bypass. Raw Ethernet frames arrive from the NIC directly into a user-space memory region — no kernel TCP/IP stack, no socket buffer copies, no `recv()` system call on the hot path. Parsed packets are forwarded to a lock-free SPSC ring buffer for consumption by a signal handler or order engine.

Built to demonstrate the low-level primitives that matter in production HFT infrastructure: kernel bypass, cache-conscious data structures, and zero-copy packet processing.

---

## Architecture

```
                          ┌─────────────────────────────────────────────────────┐
                          │                   Linux Kernel                      │
  ┌─────────┐   DMA       │  ┌────────────┐   XDP hook   ┌──────────────────┐  │
  │   NIC   │ ─────────── │  │  RX Queue  │ ──────────── │   XDP Program    │  │
  └─────────┘             │  └────────────┘              │  (redirect/pass) │  │
                          │                              └────────┬─────────┘  │
                          └───────────────────────────────────────│────────────┘
                                                                  │ AF_XDP
                                                                  ▼
                          ┌─────────────────────────────────────────────────────┐
                          │                  User Space                         │
                          │                                                     │
                          │  ┌──────────────────────────────────────────────┐  │
                          │  │                   UMEM Region                │  │
                          │  │  (mmap'd, huge-page-backed, pre-allocated)   │  │
                          │  │                                              │  │
                          │  │   frame[0]  frame[1]  frame[2]  ...          │  │
                          │  └───────────────────┬──────────────────────────┘  │
                          │                      │                             │
                          │                      ▼                             │
                          │  ┌───────────────────────────────────────────────┐ │
                          │  │            XdpSocket::rx_burst()              │ │
                          │  │   peek fill ring → copy RxDesc → release      │ │
                          │  └───────────────────┬───────────────────────────┘ │
                          │                      │                             │
                          │                      ▼                             │
                          │  ┌───────────────────────────────────────────────┐ │
                          │  │             Packet (non-owning view)          │ │
                          │  │   eth() → ip() → udp() → payload()           │ │
                          │  │   zero-copy: ptr + len into UMEM frame        │ │
                          │  └───────────────────┬───────────────────────────┘ │
                          │                      │                             │
                          │                      ▼                             │
                          │  ┌───────────────────────────────────────────────┐ │
                          │  │       SpscRingBuffer<Packet, N>               │ │
                          │  │   acquire/release atomics, cache-line padded  │ │
                          │  └───────────────────┬───────────────────────────┘ │
                          │                      │                             │
                          │                      ▼                             │
                          │  ┌───────────────────────────────────────────────┐ │
                          │  │            Signal / Order Engine              │ │
                          │  │          (pop() or peek() on hot path)        │ │
                          │  └───────────────────────────────────────────────┘ │
                          └─────────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### AF_XDP over standard sockets
A `recvmmsg()` call on a standard UDP socket travels through the kernel's full network stack: driver interrupt, NAPI poll, sk_buff allocation, IP/UDP demux, socket buffer copy. AF_XDP short-circuits all of it. The XDP program redirects matched frames directly to a user-space memory region (UMEM) via `XDP_REDIRECT`. No `sk_buff` is allocated, no kernel locks are touched on the hot path. This eliminates the dominant latency contributor in software feed handlers.

`XDP_USE_NEED_WAKEUP` is set on the bind flags so that `sendto()` is only called when the kernel actually needs to be woken to drain the TX ring — avoiding unnecessary syscalls when the NIC polls autonomously.

### acquire/release over seq_cst for SPSC
Sequential consistency inserts a full memory barrier (`MFENCE` on x86) on every atomic operation. In a single-producer/single-consumer ring buffer, the ordering invariant is narrower: the producer only needs its writes visible to the consumer before the head index update (`store(release)`), and the consumer only needs to see a consistent head before reading data (`load(acquire)`). The acquire/release pair is sufficient and maps to zero additional barriers on x86's TSO memory model — the `MOV` instruction is already ordered. On ARM/POWER this still emits the minimum required fences rather than a full two-way barrier.

### Power-of-2 ring buffer capacity
The modulo operation needed to wrap ring indices (`index % N`) compiles to a division on arbitrary N. When N is a power of 2, it reduces to a single bitwise AND (`index & (N-1)`), which executes in one cycle. At 100+ million packets per second, this is a real difference. Capacity is enforced at compile time via `static_assert`.

### RAII for UMEM
`Umem` owns the mmap'd region and the `xsk_umem` handle. The destructor calls `xsk_umem__delete()` and `munmap()` unconditionally — no manual teardown, no double-free on exception during socket construction. The move constructor nulls the source's handle pointer, making it safe to move into containers or `std::optional`. The class is non-copyable because sharing a UMEM region between two owning objects would produce a double-free.

---

## Benchmarks

### SPSC Ring Buffer (microbenchmark)

Ring-buffer-only numbers — no networking, no XDP. Measured on a **GCP e2-standard-4 VM** (4 vCPUs, Intel Broadwell, 16 GB RAM). Build: `Release` (`-O3 -march=native -ffast-math`, LTO).

| Benchmark | Mean Latency | Notes |
|---|---|---|
| `SpscRingBuffer` push/pop round-trip | **7.99 ns** | Single item, cold head/tail |
| `SpscRingBuffer` peek (non-consuming) | **1.14 ns** | Head load + deref only |
| Throughput | **14.1 M items/sec** | Sustained producer→consumer |
| `size()` check | **0.395 ns** | Atomic load, no contention |

These numbers isolate the ring buffer data structure. End-to-end latency including the AF_XDP path is measured separately below.

---

### End-to-End AF_XDP Latency

**Platform:** GCP e2-standard-4 VM, Ubuntu 24.04, x86_64
**Path:** `SyntheticSender` (sender_ns / veth0) → veth pair → AF_XDP (veth1) → `SpscRingBuffer` → consumer pop
**Test:** 1000 packets, inject timestamp → ring pop, 0 dropped, 0 lost

| Percentile | Latency |
|---|---|
| min    |  18.36 µs |
| p50    |  79.70 µs |
| p99    | 160.42 µs |
| p99.9  | 209.72 µs |
| max    | 250.12 µs |

> These are VM numbers over a virtual ethernet pair (veth). The veth path traverses the kernel's virtual switching layer and shares CPU resources with other VM workloads, which accounts for the ~80 µs p50. On bare-metal x86_64 with a real NIC supporting XDP native mode (e.g. Intel i40e, Mellanox ConnectX), p50 is expected to drop to low single-digit microseconds — the XDP redirect bypasses the entire kernel network stack and DMA delivers frames directly into UMEM without a copy.

---

## Build

**Prerequisites (Ubuntu 24.04)**
```bash
sudo apt install cmake ninja-build libbpf-dev libxdp-dev
```

**Clone and build**
```bash
git clone https://github.com/hiteshjakhar29/ultrafast-feed.git
cd ultrafast-feed
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build
```

**Run tests**
```bash
# Ring buffer correctness tests (no privileges required)
ctest --test-dir build --output-on-failure

# UMEM and AF_XDP socket tests require CAP_NET_ADMIN
sudo ./build/tests/test_umem
sudo ./build/tests/test_xdp_socket
```

**Run benchmarks**
```bash
./build/bench/bench_spsc_ring_buffer --benchmark_format=console
./build/bench/bench_xdp_socket       --benchmark_format=console
```

**Debug build** (AddressSanitizer + UBSan enabled)
```bash
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-dbg
```

---

## Project Structure

| Path | Description |
|---|---|
| `include/ultrafast/spsc_ring_buffer.hpp` | Lock-free SPSC ring buffer template (`SpscRingBuffer<T, N>`) |
| `include/ultrafast/umem.hpp` | RAII wrapper for AF_XDP UMEM region; huge-page backed |
| `include/ultrafast/xdp_socket.hpp` | AF_XDP socket with burst RX/TX API (`rx_burst`, `tx_burst`, `tx_kick`) |
| `include/ultrafast/packet.hpp` | Non-owning zero-copy Ethernet/IPv4/UDP frame view |
| `src/umem.cpp` | UMEM lifecycle: mmap, `xsk_umem__create`, fill ring population, LIFO frame allocator |
| `src/xdp_socket.cpp` | Socket creation, `xsk_ring_cons__peek` RX path, completion ring drain |
| `cmake/FindDependencies.cmake` | pkg-config locator for libbpf and libxdp; builds `ultrafast_deps` INTERFACE target |
| `cmake/CompileOptions.cmake` | Per-build-type flags: Release (`-O3 -march=native` + LTO), Debug (ASan/UBSan) |
| `tests/` | Google Test suites — ring buffer correctness, UMEM, XDP socket |
| `bench/` | Google Benchmark suites — ring buffer latency/throughput, packet parse, UMEM ops |
| `docs/architecture.md` | Component design notes |

---

## Requirements

- Linux kernel ≥ 5.10 (AF_XDP with `XDP_USE_NEED_WAKEUP`)
- `CAP_NET_ADMIN` / `CAP_BPF` for socket creation and XDP program attachment
- `libbpf` ≥ 0.8, `libxdp` ≥ 1.2
- CMake ≥ 3.25, C++20 compiler (GCC 12+ or Clang 15+)

AF_XDP is a Linux kernel interface. This project does not build on macOS or Windows.

---

## Roadmap

- [ ] Bare-metal x86_64 benchmark numbers (dedicated server, CPU pinning, IRQ isolation)
- [ ] Multi-queue support via multiple `XdpSocket` instances pinned to separate RX queues
- [ ] BPF program for hardware-level flow steering by UDP port / multicast group
- [ ] NUMA-aware UMEM allocation for multi-socket servers
- [x] Integration test with synthetic market data feed over veth / network namespace
