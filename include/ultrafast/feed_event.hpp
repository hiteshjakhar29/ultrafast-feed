#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ultrafast {

/// An owned, copyable snapshot of a received UDP market-data packet.
///
/// Produced by FeedHandler's RX thread and pushed into SpscRingBuffer.
/// payload[0..7] encodes the sender's inject_ns (CLOCK_MONOTONIC, little-endian),
/// written by SyntheticSender immediately before sendto(). FeedHandler copies
/// it into inject_ns so the consumer can compute end-to-end latency without
/// touching the raw payload.
struct FeedEvent {
    uint64_t inject_ns   = 0;  ///< CLOCK_MONOTONIC ns written by sender (from payload[0..7])
    uint64_t receive_ns  = 0;  ///< CLOCK_MONOTONIC ns recorded in rx_burst loop
    uint32_t src_ip      = 0;  ///< Network byte order; use ntohl() + inet_ntoa() to print
    uint16_t src_port    = 0;  ///< Host byte order
    uint16_t dst_port    = 0;  ///< Host byte order
    uint16_t payload_len = 0;  ///< Number of valid bytes in payload[]
    uint8_t  _pad[6]     = {}; ///< Explicit padding for predictable struct layout

    static constexpr std::size_t kMaxPayload = 64;
    uint8_t payload[kMaxPayload] = {};
};

static_assert(std::is_trivially_copyable_v<FeedEvent>,
    "FeedEvent must be trivially copyable for SpscRingBuffer");

} // namespace ultrafast
