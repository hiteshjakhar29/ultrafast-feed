# End-to-End Pipeline: ultrafast-feed → lattice-ipc

This document describes the complete data path from raw Ethernet frames on the NIC to trading signals and anomaly alerts, spanning both the [ultrafast-feed](https://github.com/hiteshjakhar29/ultrafast-feed) and [lattice-ipc](https://github.com/hiteshjakhar29/lattice-ipc) projects.

---

## Full Pipeline Diagram

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  Hardware / Kernel                                                           │
│                                                                              │
│  ┌────────┐  DMA   ┌──────────┐  XDP hook  ┌──────────────────────────┐    │
│  │  NIC   │───────►│ RX Queue │───────────►│  XDP Program (eBPF)      │    │
│  └────────┘        └──────────┘            │  XDP_REDIRECT → UMEM     │    │
│                                            └─────────────┬────────────┘    │
└──────────────────────────────────────────────────────────│─────────────────┘
                                                           │ AF_XDP (zero-copy)
┌──────────────────────────────────────────────────────────│─────────────────┐
│  Process A — ultrafast-feed                              │                 │
│                                                          ▼                 │
│                                             ┌────────────────────────┐     │
│                                             │  UMEM Region (mmap'd)  │     │
│                                             │  frame[0..N-1]         │     │
│                                             └────────────┬───────────┘     │
│                                                          │                 │
│                                             ┌────────────▼───────────┐     │
│                                             │  XdpSocket::rx_burst() │     │
│                                             │  Packet (zero-copy view│     │
│                                             │  eth/ip/udp/payload)   │     │
│                                             └────────────┬───────────┘     │
│                                                          │                 │
│                                             ┌────────────▼───────────┐     │
│                                             │  FeedHandler           │     │
│                                             │  parse → FeedEvent     │     │
│                                             │  {inject_ns,           │     │
│                                             │   receive_ns,          │     │
│                                             │   payload[64]}         │     │
│                                             └────────────┬───────────┘     │
│                                                          │                 │
│                                             ┌────────────▼───────────┐     │
│                                             │  SpscRingBuffer        │     │
│                                             │  <FeedEvent, 65536>    │     │
│                                             │  (intra-process)       │     │
│                                             └────────────┬───────────┘     │
│                                                          │                 │
│                         ┌────────────────────────────────▼───────────┐     │
│                         │  Bridge Thread                              │     │
│                         │  ring.pop() → shm_writer.try_write()       │     │
│                         └────────────────────────────────┬───────────┘     │
│                                                          │                 │
│                                             ┌────────────▼───────────┐     │
│                                             │  ShmWriter             │     │
│                                             │  <FeedEvent, 65536>    │     │
│                                             │  atomic release store  │     │
│                                             └────────────┬───────────┘     │
└──────────────────────────────────────────────────────────│─────────────────┘
                                                           │
                                          /dev/shm/lattice_feed  (POSIX shm)
                                                           │
┌──────────────────────────────────────────────────────────│─────────────────┐
│  Process B — lattice-ipc                                 │                 │
│                                                          ▼                 │
│                                             ┌────────────────────────┐     │
│                                             │  ShmReader             │     │
│                                             │  <FeedEvent, 65536>    │     │
│                                             │  atomic acquire load   │     │
│                                             └────────┬───────┬───────┘     │
│                                                      │       │             │
│                              ┌───────────────────────▼┐     ┌▼──────────────────────┐ │
│                              │  SignalEngine          │     │  AnomalyDetector      │ │
│                              │  OrderBook (bid/ask    │     │  PendingOrder hash    │ │
│                              │  maps + BBO cache)     │     │  table + WelfordStats │ │
│                              │  OBI, microprice,      │     │  Z-score on cancel    │ │
│                              │  spread computation    │     │  latency distribution │ │
│                              └───────────┬────────────┘     └──────────┬────────────┘ │
│                                          │                              │             │
│                              ┌───────────▼────────────┐     ┌──────────▼────────────┐ │
│                              │  SignalSnapshot        │     │  SpoofAlert           │ │
│                              │  {mid, microprice,     │     │  {order_id, z_score,  │ │
│                              │   obi, spread,         │     │   placed_ns,          │ │
│                              │   timestamp_ns}        │     │   cancelled_ns}       │ │
│                              └───────────┬────────────┘     └──────────┬────────────┘ │
│                                          └──────────────┬───────────────┘             │
│                                                         ▼                             │
│                                       downstream risk / strategy / surveillance       │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Stage-by-Stage Breakdown

### Stage 1 — Kernel Bypass Packet Capture (ultrafast-feed)

The XDP program attached to the NIC's RX queue inspects each incoming frame and redirects matching UDP packets directly to a user-space memory region (UMEM) via `XDP_REDIRECT`. No `sk_buff` is allocated; the kernel network stack is bypassed entirely.

`XdpSocket::rx_burst()` drains the RX ring and yields non-owning `Packet` views into UMEM frames. `FeedHandler` parses each frame into a `FeedEvent` and pushes it onto the intra-process `SpscRingBuffer`.

### Stage 2 — Bridge: SpscRingBuffer → ShmWriter

A dedicated bridge thread pops `FeedEvent`s from the lock-free ring and forwards them into the shared memory segment via `ShmWriter::try_write()`. This is the only process-boundary crossing in the hot path.

`FeedEvent` is byte-for-byte identical in both projects — no serialisation or conversion occurs.

### Stage 3 — Signal Processing and Anomaly Detection (lattice-ipc)

`ShmReader::try_read()` polls the shared memory ring on an atomic acquire load. Each `FeedEvent` is dispatched to:

- **`SignalEngine`**: maintains a live order book, updates the BBO cache, and emits a `SignalSnapshot` with OBI, microprice, and spread on every best-bid/offer change.
- **`AnomalyDetector`**: tracks large orders in a hash table and uses Welford's online algorithm to compute a Z-score on cancel-latency distribution. Orders cancelled abnormally quickly relative to the observed mean emit a `SpoofAlert`.

---

## Connecting the Two Projects

### FeedEvent definition (shared layout)

Both projects use the same `FeedEvent` struct. In ultrafast-feed it lives at `include/ultrafast/feed_event.hpp`; in lattice-ipc at `include/lattice/feed_event.hpp`. They are binary-compatible — no conversion needed.

```cpp
struct FeedEvent {
    uint64_t inject_ns;      // stamped by SyntheticSender just before sendto()
    uint64_t receive_ns;     // stamped by FeedHandler after ring pop
    uint8_t  payload[64];    // raw UDP payload (order book event encoding)
};
```

### Bridge thread: ring → shm

```cpp
#include "ultrafast/feed_handler.hpp"
#include "lattice/shm/shm_writer.hpp"
#include "lattice/feed_event.hpp"

// FeedHandler running AF_XDP rx_loop in its own thread.
ultrafast::FeedHandler feed(feed_cfg);
feed.start();

// ShmWriter opens /dev/shm/lattice_feed and writes FeedEvents.
lattice::ShmWriter<lattice::FeedEvent, 65536> writer("lattice_feed");

// Bridge: drain the intra-process ring into shared memory.
while (running) {
    if (auto ev = feed.pop()) {
        // FeedEvent layouts are identical — memcpy-safe cast.
        writer.try_write(*reinterpret_cast<const lattice::FeedEvent*>(&*ev));
    }
}

feed.stop();
```

### Consumer: shm → SignalEngine + AnomalyDetector

```cpp
#include "lattice/shm/shm_reader.hpp"
#include "lattice/signals/signal_engine.hpp"
#include "lattice/anomaly/anomaly_detector.hpp"
#include "lattice/feed_event.hpp"

lattice::ShmReader<lattice::FeedEvent, 65536> reader("lattice_feed");
lattice::SignalEngine                         signals;
lattice::AnomalyDetector                      anomaly(anomaly_cfg);

while (running) {
    if (auto ev = reader.try_read()) {
        if (auto snap = signals.on_event(*ev)) {
            // BBO changed — snap.mid, snap.microprice, snap.obi, snap.spread
            on_signal(*snap);
        }
        if (auto alert = anomaly.on_event(*ev)) {
            // Spoofing candidate detected
            on_spoof_alert(*alert);
        }
    }
}
```

---

## Latency Budget (GCP VM / veth, indicative)

| Stage | p50 latency | Notes |
|---|---|---|
| NIC → UMEM (AF_XDP, bare-metal) | ~1–5 µs | XDP native mode, DMA direct to UMEM |
| NIC → UMEM (AF_XDP, VM/veth) | ~80 µs | Virtual switching layer overhead |
| SpscRingBuffer push/pop | ~8 ns | Intra-process, cache-hot |
| ShmWriter::try_write | ~22 ns | Atomic release store, POSIX shm |
| ShmReader::try_read | ~22 ns | Atomic acquire load |
| SignalEngine (BBO change) | ~53 ns | OBI + microprice recompute |
| AnomalyDetector (ADD+CANCEL) | ~2.6 µs | Hash lookup + Welford + sqrt |

The dominant cost in production is the NIC-to-UMEM stage. On bare-metal with XDP native mode the full pipeline from wire to `SignalSnapshot` fits in approximately **5–10 µs** at p50.

---

## Repository Links

| Project | Responsibility | Repo |
|---|---|---|
| ultrafast-feed | AF_XDP kernel bypass, feed parsing, intra-process SPSC ring | https://github.com/hiteshjakhar29/ultrafast-feed |
| lattice-ipc | SHM IPC, signal engine, spoofing detection | https://github.com/hiteshjakhar29/lattice-ipc |
