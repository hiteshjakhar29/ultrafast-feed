# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Prerequisites (Ubuntu 24.04)**
```bash
sudo apt install cmake ninja-build libbpf-dev libxdp-dev
```

**Configure and build**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build

# Debug build (AddressSanitizer + UBSan enabled)
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-dbg
```

**Run tests**
```bash
# Ring buffer and feed handler tests (no privileges required)
ctest --test-dir build --output-on-failure

# Run a single test suite by name
ctest --test-dir build -R spsc_ring_buffer --output-on-failure

# UMEM and AF_XDP socket tests require CAP_NET_ADMIN
sudo ./build/tests/test_umem
sudo ./build/tests/test_xdp_socket

# Run a test binary directly (useful with gdb/asan output)
./build/tests/test_spsc_ring_buffer
./build/tests/test_feed_handler
```

**Run benchmarks**
```bash
./build/bench/bench_spsc_ring_buffer --benchmark_format=console
./build/bench/bench_xdp_socket       --benchmark_format=console
```

## Architecture

The data path is: **NIC → XDP program (eBPF, kernel) → UMEM fill ring → RX ring → `XdpSocket::rx_burst()` → `SpscRingBuffer<Packet, N>` → feed handler thread**

### Key classes

| Class | Location | Role |
|---|---|---|
| `Umem` | `include/ultrafast/umem.hpp` / `src/umem.cpp` | RAII wrapper for the mmap'd DMA region shared with the kernel. Non-copyable; move-safe (nulls handle on move). Owns a LIFO frame allocator for the fill ring. |
| `XdpSocket` | `include/ultrafast/xdp_socket.hpp` / `src/xdp_socket.cpp` | AF_XDP socket bound to a single NIC queue. Exposes `rx_burst()` / `tx_burst()` / `tx_kick()`. Uses `XDP_USE_NEED_WAKEUP` to avoid spurious syscalls. |
| `SpscRingBuffer<T, N>` | `include/ultrafast/spsc_ring_buffer.hpp` | Header-only lock-free SPSC queue. `T` must be trivially copyable; `N` must be a power of 2 (enforced via `static_assert`). Uses acquire/release atomics with cache-line-padded indices to prevent false sharing. |
| `Packet` | `include/ultrafast/packet.hpp` | Non-owning zero-copy view into a UMEM frame. Provides `eth()` / `ip()` / `udp()` / `payload()` accessors — no allocation, no copy. |
| `FeedHandler` | `include/ultrafast/feed_handler.hpp` / `src/feed_handler.cpp` | Ties together `XdpSocket` and `SpscRingBuffer`; runs the RX poll loop and dispatches parsed `Packet`s to the consumer. |

### CMake structure

- `cmake/FindDependencies.cmake` — pkg-config locator for libbpf and libxdp; creates `ultrafast_deps` INTERFACE target.
- `cmake/CompileOptions.cmake` — creates `ultrafast_compile_options` INTERFACE target. Release: `-O3 -march=native -ffast-math` + LTO. Debug: `-O0 -g3 -fsanitize=address,undefined`. Frame pointer is always kept for `perf` profiling.
- `src/CMakeLists.txt` — builds `ultrafast_core` static library from `umem.cpp`, `xdp_socket.cpp`, `feed_handler.cpp`.
- `tests/CMakeLists.txt` — one executable per test suite (`test_spsc_ring_buffer`, `test_umem`, `test_xdp_socket`, `test_feed_handler`); all registered with CTest. Benchmark executables are intentionally excluded from CTest.
- GoogleTest (v1.15.2) and Google Benchmark (v1.9.1) are fetched via `FetchContent`; system GTest is used if available.

### Critical design constraints

- **Linux-only**: AF_XDP, libbpf, and libxdp are Linux kernel interfaces (kernel ≥ 5.10 required).
- **`-march=native`**: Release binaries are not portable across CPU generations.
- **`CAP_NET_ADMIN` / `CAP_BPF`**: Required for AF_XDP socket creation and XDP program attachment. Ring buffer and feed handler logic can be tested without privileges.
- **Single-queue per `XdpSocket`**: For multi-queue capture, create one `XdpSocket` per RX queue sharing the same `Umem`.
- **SPSC contract**: `push()` must be called from the producer thread only; `pop()` / `peek()` from the consumer thread only. `empty()` / `full()` / `size()` are approximate under concurrent use.

### Untracked files (in-progress additions)

The following files exist locally but are not yet committed: `include/ultrafast/feed_event.hpp`, `include/ultrafast/feed_handler.hpp`, `include/ultrafast/synthetic_sender.hpp`, `src/feed_handler.cpp`, `tests/test_feed_handler.cpp`, and the `tools/` directory.
