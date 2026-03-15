# ultrafast-feed Architecture

## Overview

ultrafast-feed is a C++20 HFT market data feed handler using AF\_XDP for
kernel-bypass networking on Linux.  It achieves sub-microsecond packet
processing by bypassing the kernel network stack entirely, delivering raw
Ethernet frames from the NIC directly to user space via shared memory.

## Component map

```
NIC ──► XDP programme (eBPF, kernel) ──► UMEM fill ring
                                              │
                                         RX ring (user)
                                              │
                                         XdpSocket::rx_burst()
                                              │
                                         SpscRingBuffer<Packet, N>
                                              │
                                         Feed handler thread
```

## Key classes

| Class | File | Role |
|---|---|---|
| `Umem` | `include/ultrafast/umem.hpp` | RAII wrapper around the shared DMA region |
| `XdpSocket` | `include/ultrafast/xdp_socket.hpp` | AF\_XDP socket: RX/TX burst API |
| `SpscRingBuffer<T,N>` | `include/ultrafast/spsc_ring_buffer.hpp` | Lock-free SPSC queue for inter-thread communication |
| `Packet` | `include/ultrafast/packet.hpp` | Non-owning Eth/IP/UDP frame view |

## Build

```bash
# Prerequisites (Ubuntu 24.04)
sudo apt install cmake ninja-build libbpf-dev libxdp-dev doxygen

# Configure + build
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/bench/bench_spsc_ring_buffer --benchmark_format=console
./build/bench/bench_xdp_socket       --benchmark_format=console

# AF_XDP tests require CAP_NET_ADMIN
sudo ./build/tests/test_umem
sudo ./build/tests/test_xdp_socket
```

## Design constraints

- **Linux-only**: AF\_XDP, libbpf, and libxdp are Linux kernel interfaces.
- **`-march=native`**: Release builds target the host CPU (AVX-512 on GCP C3).
  The resulting binary is not portable to older instance families.
- **CAP\_NET\_ADMIN**: Opening AF\_XDP sockets requires elevated privileges or
  the `CAP_NET_ADMIN` / `CAP_BPF` capabilities.
- **Single-queue**: Each `XdpSocket` is bound to one NIC queue.  For
  multi-queue capture, create one `XdpSocket` per queue sharing the same
  `Umem`.
