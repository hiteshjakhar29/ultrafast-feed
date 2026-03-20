[![CI](https://github.com/hiteshjakhar29/ultrafast-feed/actions/workflows/ci.yml/badge.svg)](https://github.com/hiteshjakhar29/ultrafast-feed/actions/workflows/ci.yml)

# ultrafast-feed

C++20 market data feed handler built on AF_XDP kernel bypass. Raw Ethernet frames arrive from the NIC directly into a user-space memory region — no kernel TCP/IP stack, no `sk_buff` allocation, no socket buffer copies, no `recv()` system call on the hot path. Parsed packets are forwarded over a lock-free SPSC ring buffer to a bridge thread that writes `FeedEvent`s into a POSIX shared-memory ring consumed by [lattice-ipc](https://github.com/hiteshjakhar29/lattice-ipc).

In production HFT infrastructure, the dominant latency contributor in software feed handlers is the kernel network stack: driver interrupt, NAPI poll, `sk_buff` allocation, IP/UDP demux, socket buffer copy — 50–100 µs of overhead before userspace ever sees a byte. AF_XDP short-circuits all of it. This project demonstrates the full set of low-level primitives required to get there: kernel bypass via `XDP_REDIRECT`, a UMEM DMA region shared between kernel and userspace, a cache-conscious zero-copy packet view, and an acquire/release SPSC ring that maps to zero additional memory barriers on x86 TSO.

---

## Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  NIC                                                                        │
│   │  raw UDP/multicast                                                      │
│   ▼                                                                         │
│  AF_XDP socket  (XDP_REDIRECT — kernel bypass)                             │
│   │  zero-copy DMA into UMEM                                                │
│   ▼                                                                         │
│  FeedHandler::rx_burst()   →   SpscRingBuffer<FeedEvent, 65536>            │
│   │  acquire/release atomics, cache-line padded indices                     │
│   │                                                                         │
│   │  FeedEvent { inject_ns, receive_ns, src_ip, src_port, payload[64] }    │
│   ▼                                                                         │
│  ShmWriter<FeedEvent, 65536>                                                │
│   │  atomic release store — write_idx                                       │
│   │  /dev/shm/lattice_feed  (mmap'd POSIX shm, one cache-line per index)   │
└───║─────────────────────────────────────────────────────────────────────────┘
    ║  OS process boundary
┌───║─────────────────────────────────────────────────────────────────────────┐
│   ║  atomic acquire load — write_idx                                        │
│  ShmReader<FeedEvent, 65536>          lattice-ipc                           │
│   │                                                                         │
│   ├──────────────────────┐                                                  │
│   ▼                      ▼                                                  │
│  SignalEngine        AnomalyDetector                                        │
│  (OFI, VAMP, OBI,    (Welford Z-scores — spoofing,                         │
│   microprice, TFI)    layering, cancel-spike, burst)                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

This project is the packet capture and feed parsing layer. See [lattice-ipc](https://github.com/hiteshjakhar29/lattice-ipc) for the signal processing and anomaly detection layer downstream.

---

## Architecture

### 1 — AF_XDP + UMEM

AF_XDP is a Linux kernel interface that allows an XDP eBPF program attached to a NIC queue to redirect matched frames directly into a user-space memory region called UMEM, bypassing the entire kernel network stack.

A standard `recvmmsg()` call on a UDP socket traverses: driver interrupt → NAPI poll → `sk_buff` allocation → IP/UDP demux → socket buffer copy → `recvmmsg()` return. Every step crosses a kernel boundary or touches shared state. AF_XDP replaces all of it with a single `XDP_REDIRECT` action at the driver level. No `sk_buff` is allocated. No kernel locks are touched on the hot path.

UMEM is a contiguous memory region (`mmap`'d, huge-page-backed, pre-allocated at startup) divided into fixed-size frames. The kernel and userspace communicate through two ring pairs:

- **Fill ring**: userspace posts free frame addresses for the kernel to DMA into.
- **RX ring**: kernel posts completed frame descriptors (address + length) after DMA.
- **TX ring** / **Completion ring**: symmetric path for transmit (used by `SyntheticSender` in tests).

`XDP_USE_NEED_WAKEUP` is set on the bind flags so that the syscall to wake the kernel (`sendto`) is only issued when the NIC cannot poll autonomously — eliminating spurious syscalls on the hot path.

```
UMEM layout (pre-allocated at startup):
┌────────────────────────────────────────────────────────┐
│  frame[0]   frame[1]   frame[2]   ...   frame[N-1]     │
│  [2048 B]   [2048 B]   [2048 B]         [2048 B]       │
│                                                         │
│  Fill ring: userspace → kernel  (free frame addresses)  │
│  RX ring:   kernel → userspace  (received descriptors)  │
└────────────────────────────────────────────────────────┘
```

`Umem` is a RAII class. The destructor calls `xsk_umem__delete()` and `munmap()` unconditionally. The move constructor nulls the source handle, making it safe to move into containers or `std::optional`. The class is non-copyable: sharing a UMEM region between two owning objects would produce a double-free.

---

### 2 — Packet Parser

`Packet` is a non-owning zero-copy view into a UMEM frame. No allocation, no copy. Construction takes a raw pointer and length into the UMEM region and exposes a header-chain accessor API:

```
Packet p(frame_ptr, frame_len);
const ethhdr*  eth  = p.eth();   // Ethernet header at offset 0
const iphdr*   ip   = p.ip();    // IPv4 header at offset 14
const udphdr*  udp  = p.udp();   // UDP header at offset 14 + ip->ihl*4
std::span<const uint8_t> payload = p.payload();
```

Each accessor validates the offset against the frame length — no out-of-bounds access on malformed or undersized frames. Because `Packet` holds only a pointer and length into UMEM, the frame data is never copied. Parsing is a sequence of pointer arithmetic operations with bounds checks, completed in a handful of nanoseconds.

---

### 3 — SPSC Ring Buffer

`SpscRingBuffer<T, N>` is a header-only lock-free single-producer/single-consumer ring buffer. `T` must be trivially copyable; `N` must be a power of two (enforced by `static_assert`).

**Memory ordering:** The producer stores into the slot, then issues a `release` store on `tail_`. The consumer issues an `acquire` load on `tail_` before reading the slot. This acquire/release pair is sufficient and correct for SPSC — the consumer observes all writes that happened-before the `tail_` update. No additional fences. On x86 TSO, `release` store and `acquire` load both compile to plain `MOV` instructions — zero extra barriers in hardware.

**Cache-line layout:** `tail_` (producer index) and `head_` (consumer index) are each padded to their own 64-byte cache line via `alignas(64)`. This prevents false sharing: the producer core does not invalidate the consumer's cache line on every push, and vice versa.

**Power-of-2 wrap:** Index wrap is `(index + 1) & kMask` — a single bitwise AND instead of a division. `kMask = N - 1`. At 100+ million packets per second, the savings are real.

```cpp
// Producer (single thread only)
bool push(const T& item) noexcept {
    const size_t t      = tail_.load(relaxed);
    const size_t next_t = (t + 1) & kMask;
    if (next_t == head_.load(acquire)) return false;  // full
    buf_[t] = item;
    tail_.store(next_t, release);
    return true;
}

// Consumer (single thread only)
optional<T> pop() noexcept {
    const size_t h = head_.load(relaxed);
    if (h == tail_.load(acquire)) return nullopt;     // empty
    T item = buf_[h];
    head_.store((h + 1) & kMask, release);
    return item;
}
```

---

### 4 — FeedHandler

`FeedHandler` ties together `Umem`, `XdpSocket`, and `SpscRingBuffer<FeedEvent, 65536>` into a threaded RX loop.

**rx_burst loop:** The RX thread calls `xsk_ring_cons__peek()` to batch-consume up to 64 descriptors from the RX ring per iteration. For each descriptor it constructs a `Packet` (zero-copy view into UMEM), parses the Ethernet/IPv4/UDP chain, and pushes a `FeedEvent` (with `inject_ns`, `receive_ns`, `src_ip`, `src_port`, `payload[64]`) into the ring. After the batch, frame addresses are returned to the fill ring for the kernel to reuse.

**Batch release:** Frames are returned to the fill ring in bulk at the end of each `rx_burst()` call, not one-by-one. This reduces the number of ring pointer updates and keeps the fill ring well-stocked for subsequent DMA bursts.

**Network namespace isolation:** Integration tests run the `SyntheticSender` in a separate Linux network namespace (`sender_ns`) connected to the receiver via a `veth` pair. This gives deterministic, isolated packet delivery without interfering with the host network stack or requiring a physical NIC. The sender injects a `CLOCK_MONOTONIC` timestamp into each packet's payload; the receiver records `receive_ns` on pop. End-to-end latency = `receive_ns - inject_ns`.

```
sender_ns                    default ns
┌──────────────────┐         ┌──────────────────────────┐
│  SyntheticSender │         │  FeedHandler             │
│  (CLOCK_MONOTONIC│ ──────► │  XdpSocket (veth1)       │
│   timestamp)     │  veth   │  SpscRingBuffer          │
│  veth0           │         │  consumer pop → latency  │
└──────────────────┘         └──────────────────────────┘
```

---

## Benchmarks

### SPSC Ring Buffer

Ring-buffer-only numbers — no networking, no XDP. Measured on a **GCP e2-standard-4 VM** (4 vCPUs, Intel Broadwell, 16 GB RAM). Build: `Release` (`-O3 -march=native -ffast-math`, LTO).

| Benchmark | Mean Latency | Notes |
|---|---|---|
| `push` + `pop` round-trip | **7.99 ns** | Single item, cold head/tail |
| `peek` (non-consuming) | **1.14 ns** | Acquire load on `tail_` + pointer deref |
| `size()` check | **0.395 ns** | Two atomic loads, no contention |
| Throughput | **14.1 M items/s** | Sustained producer→consumer, separate threads |

The `peek` path (1.14 ns) does not advance `head_` — it is a pure read used by the consumer to inspect the front element without committing to consumption. The `size()` path (0.395 ns) is a single atomic load pair; the sub-nanosecond result reflects the compiler folding the two loads into a single cache line hit.

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

> These are VM numbers over a virtual ethernet pair (veth). The veth path traverses the kernel's virtual switching layer and shares CPU resources with other VM workloads, which accounts for the ~80 µs p50. On bare-metal x86_64 with a real NIC supporting XDP native mode (Intel i40e, Mellanox ConnectX-4/5/6), the XDP program intercepts frames at the driver level before any `sk_buff` is allocated, and DMA delivers frames directly into UMEM without a copy. p50 on bare metal is expected in the low single-digit microseconds range.

---

### Standard Socket vs AF_XDP Baseline Comparison

**Platform:** GCP e2-standard-4 VM, Ubuntu 24.04, x86_64
**Path:** `SyntheticSender` (sender_ns / veth0) → veth pair → receiver (veth1) → `SpscRingBuffer` → consumer pop
**Test:** 1000 packets per path, interleaved runs, inject timestamp → ring pop

| Metric  | Std Socket (µs) | AF_XDP (µs) | Speedup |
|---------|-----------------|-------------|---------|
| min     |  33.09          |  11.16      |  2.97×  |
| p50     |  89.64          |  82.57      |  1.09×  |
| p99     | 202.78          | 199.36      |  1.02×  |
| p99.9   | 269.02          | 245.59      |  1.10×  |
| max     | 352.81          | 335.88      |  1.05×  |

> **Why the speedup is modest on VM:** veth is a kernel virtual switching device. Both AF_XDP and standard sockets must traverse the kernel's virtual switching layer to cross the veth pair — hardware DMA is not involved regardless of socket type. The numbers converge because both paths share the same virtual NIC overhead. The 2.97× min speedup reflects cases where the AF_XDP path avoids allocator pressure.
>
> **On bare metal with XDP native mode**, standard sockets spend 50–100 µs in `sk_buff` allocation, IP/UDP demux, and socket buffer copy. AF_XDP bypasses all of it. The expected speedup is **10–50× at p50** — the delta that matters in co-located HFT.

---

## Design Decisions

### 1 — AF_XDP over standard sockets

A `recvmmsg()` call on a standard UDP socket travels through the full kernel network stack: driver interrupt, NAPI poll, `sk_buff` allocation, IP/UDP demux, socket buffer copy. AF_XDP short-circuits all of it. The XDP program attached to the NIC queue performs a single `XDP_REDIRECT` action, moving the frame descriptor into the UMEM RX ring without allocating a single `sk_buff`, acquiring a single kernel lock, or issuing a single additional syscall.

The tradeoff is complexity: UMEM lifecycle, fill ring management, and the BPF program load/attach path are all manual. The complexity is bounded and one-time — it does not appear on the hot path.

---

### 2 — acquire/release over seq_cst for SPSC

Sequential consistency (`seq_cst`) inserts a full memory barrier (`MFENCE` on x86) on every atomic store. For SPSC with exactly one producer and one consumer, the required invariant is narrower: the consumer must see all slot writes that happened-before the `tail_` update; the producer must see `head_` before checking fullness. `release` store on `tail_`, `acquire` load on `tail_` by the consumer — this is sufficient and maps to zero additional hardware barriers on x86 TSO. The `MOV` instruction is already store-ordered on TSO. On ARM and POWER, the acquire/release pair still emits the minimum required fences rather than a full two-way barrier.

Benchmarked delta between `seq_cst` and `acq/rel` on the SPSC hot path: ~4 ns per operation.

---

### 3 — Power-of-2 ring buffer capacity

Index wrap with arbitrary capacity requires integer division (`index % N`). When N is a power of two, this reduces to a bitwise AND (`index & (N - 1)`) — one cycle, no division unit. The SPSC push/pop path executes this operation twice per call. At 14 M items/second sustained throughput, eliminating the division matters. Capacity is enforced at compile time via `static_assert((N & (N - 1)) == 0)`.

---

### 4 — RAII for UMEM

`Umem` owns both the `mmap`'d memory region and the `xsk_umem` handle returned by `xsk_umem__create()`. The destructor calls `xsk_umem__delete()` followed by `munmap()` unconditionally — no manual teardown, no double-free on exception during `XdpSocket` construction (which holds a reference to the `Umem`). The move constructor nulls the source's handle pointer so that the moved-from destructor becomes a no-op. The class is non-copyable: two `Umem` instances owning the same region would both call `munmap()` on destruction.

This matters most during test teardown and error paths. `FeedHandler` declares `Umem umem_` before `XdpSocket socket_` — C++ destructs in reverse declaration order, ensuring the socket is destroyed (and the XDP program detached) before the UMEM region is unmapped.

---

### 5 — Network namespaces for deterministic testing

End-to-end latency tests run the `SyntheticSender` in a separate Linux network namespace connected to the receiver via a `veth` pair. This gives several properties that a loopback or real-NIC test cannot guarantee on a shared VM:

- **Isolation:** No ambient traffic from the host network stack contaminates the measurements.
- **Determinism:** The veth pair is the only path between sender and receiver — no routing decisions, no ARP timeouts (static ARP entries are installed at setup), no ICMP redirects.
- **Privilege scope:** Network namespace creation requires `CAP_NET_ADMIN`, but the AF_XDP socket can be bound to the veth interface inside the default namespace. No kernel module or hardware dependency.
- **Reproducibility:** The same topology runs identically in CI as it does locally — no NIC driver differences, no NUMA topology, no IRQ affinity.

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
# Ring buffer and feed handler correctness tests (no privileges required)
ctest --test-dir build --output-on-failure

# Run a single test suite by name
ctest --test-dir build -R spsc_ring_buffer --output-on-failure

# UMEM and AF_XDP socket tests require CAP_NET_ADMIN
sudo ./build/tests/test_umem
sudo ./build/tests/test_xdp_socket
```

**Run benchmarks**
```bash
./build/bench/bench_spsc_ring_buffer --benchmark_format=console
./build/bench/bench_xdp_socket       --benchmark_format=console
```

**Debug build** (AddressSanitizer + UBSan)
```bash
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-dbg
```

---

## Project Structure

| Path | Description |
|---|---|
| `include/ultrafast/spsc_ring_buffer.hpp` | Lock-free SPSC ring buffer template (`SpscRingBuffer<T, N>`) — acquire/release atomics, cache-line padded indices, power-of-2 enforced at compile time |
| `include/ultrafast/umem.hpp` | RAII wrapper for AF_XDP UMEM region; huge-page backed, non-copyable, move-safe |
| `include/ultrafast/xdp_socket.hpp` | AF_XDP socket — burst RX/TX API (`rx_burst`, `tx_burst`, `tx_kick`), `XDP_USE_NEED_WAKEUP` |
| `include/ultrafast/packet.hpp` | Non-owning zero-copy Ethernet/IPv4/UDP frame view — pointer + length into UMEM |
| `include/ultrafast/feed_event.hpp` | `FeedEvent` struct — byte-for-byte identical to `lattice::FeedEvent`, zero-conversion interop |
| `include/ultrafast/feed_handler.hpp` | `FeedHandler` — `Umem` + `XdpSocket` + `SpscRingBuffer<FeedEvent>` threaded RX loop |
| `include/ultrafast/synthetic_sender.hpp` | `SyntheticSender` — raw UDP sender with `CLOCK_MONOTONIC` timestamps for latency tests |
| `src/umem.cpp` | UMEM lifecycle: `mmap`, `xsk_umem__create`, fill ring population, LIFO frame allocator |
| `src/xdp_socket.cpp` | Socket creation, `xsk_ring_cons__peek` RX path, completion ring drain |
| `src/feed_handler.cpp` | `rx_loop()` — batch rx_burst, parse, push to ring, batch fill-ring replenishment |
| `cmake/FindDependencies.cmake` | pkg-config locator for libbpf and libxdp; builds `ultrafast_deps` INTERFACE target |
| `cmake/CompileOptions.cmake` | Per-build-type flags: Release (`-O3 -march=native -ffast-math` + LTO), Debug (ASan/UBSan) |
| `tests/` | Google Test suites — ring buffer correctness, UMEM, XDP socket, feed handler end-to-end |
| `bench/` | Google Benchmark suites — ring buffer latency/throughput, packet parse, UMEM ops |
| `tools/` | `SyntheticSender` driver and network namespace setup scripts |
| `docs/` | Architecture notes and pipeline integration diagram |

---

## Requirements

- **OS:** Linux kernel ≥ 5.10 (AF_XDP with `XDP_USE_NEED_WAKEUP`)
- **Privileges:** `CAP_NET_ADMIN` / `CAP_BPF` for XDP socket creation and program attachment; ring buffer and feed handler logic can be tested without privileges
- **Kernel interfaces:** `libbpf` ≥ 0.8, `libxdp` ≥ 1.2
- **Compiler:** GCC 12+ or Clang 15+ with C++20 support
- **Build system:** CMake ≥ 3.25, Ninja (recommended)
- **Dependencies:** GoogleTest 1.15.2 and Google Benchmark 1.9.1 — fetched automatically via `FetchContent`
- **Downstream:** [lattice-ipc](https://github.com/hiteshjakhar29/lattice-ipc) for signal computation and anomaly detection (optional — any `FeedEvent` consumer works)

AF_XDP is a Linux kernel interface. This project does not build on macOS or Windows.
