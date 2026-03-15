#pragma once

#include <arpa/inet.h>   // ntohs, ntohl
#include <cstddef>
#include <cstdint>

namespace ultrafast {

// ─────────────────────────────────────────────────────────────────────────────
// Wire-format structs (packed, network / big-endian byte order)
// ─────────────────────────────────────────────────────────────────────────────

/// Ethernet II header — 14 bytes, no VLAN tag.
struct EthHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ether_type;   ///< BE: 0x0800 = IPv4, 0x86DD = IPv6, 0x8100 = VLAN
} __attribute__((packed));
static_assert(sizeof(EthHeader) == 14, "EthHeader size mismatch");

/// IPv4 header — 20 bytes (no options assumed for HFT paths).
struct IpHeader {
    uint8_t  version_ihl;     ///< version[7:4] | IHL[3:0] in 32-bit words
    uint8_t  dscp_ecn;
    uint16_t total_length;    ///< BE: includes header + payload
    uint16_t identification;  ///< BE
    uint16_t flags_fragment;  ///< BE: flags[15:13] | fragment_offset[12:0]
    uint8_t  ttl;
    uint8_t  protocol;        ///< 6 = TCP, 17 = UDP
    uint16_t checksum;        ///< BE
    uint32_t src_ip;          ///< BE
    uint32_t dst_ip;          ///< BE
} __attribute__((packed));
static_assert(sizeof(IpHeader) == 20, "IpHeader size mismatch");

/// UDP header — 8 bytes.
struct UdpHeader {
    uint16_t src_port;  ///< BE
    uint16_t dst_port;  ///< BE
    uint16_t length;    ///< BE: includes 8-byte UDP header
    uint16_t checksum;  ///< BE
} __attribute__((packed));
static_assert(sizeof(UdpHeader) == 8, "UdpHeader size mismatch");

// ─────────────────────────────────────────────────────────────────────────────
// Non-owning packet view (pointer + length into UMEM region)
// ─────────────────────────────────────────────────────────────────────────────

/// Non-owning view of a raw Ethernet frame stored inside a UMEM region.
///
/// All accessor methods return nullptr / 0 if the frame is shorter than
/// the required header chain — no undefined behaviour on malformed frames.
struct Packet {
    const uint8_t* data = nullptr;  ///< Pointer into UMEM (not owned)
    std::size_t    len  = 0;        ///< Total frame byte count

    // ── Layer-2 ──────────────────────────────────────────────────────────────

    [[nodiscard]] const EthHeader* eth() const noexcept {
        if (len < sizeof(EthHeader)) return nullptr;
        return reinterpret_cast<const EthHeader*>(data);
    }

    // ── Layer-3 ──────────────────────────────────────────────────────────────

    [[nodiscard]] const IpHeader* ip() const noexcept {
        if (len < sizeof(EthHeader) + sizeof(IpHeader)) return nullptr;
        return reinterpret_cast<const IpHeader*>(data + sizeof(EthHeader));
    }

    // ── Layer-4 ──────────────────────────────────────────────────────────────

    [[nodiscard]] const UdpHeader* udp() const noexcept {
        const auto* ip_hdr = ip();
        if (!ip_hdr || ip_hdr->protocol != 17) return nullptr;
        const std::size_t ip_hlen =
            static_cast<std::size_t>(ip_hdr->version_ihl & 0x0FU) * 4U;
        const std::size_t udp_off = sizeof(EthHeader) + ip_hlen;
        if (len < udp_off + sizeof(UdpHeader)) return nullptr;
        return reinterpret_cast<const UdpHeader*>(data + udp_off);
    }

    // ── Application payload ───────────────────────────────────────────────────

    [[nodiscard]] const uint8_t* payload() const noexcept {
        const auto* udp_hdr = udp();
        if (!udp_hdr) return nullptr;
        const auto* ip_hdr = ip();
        const std::size_t ip_hlen =
            static_cast<std::size_t>(ip_hdr->version_ihl & 0x0FU) * 4U;
        return data + sizeof(EthHeader) + ip_hlen + sizeof(UdpHeader);
    }

    [[nodiscard]] std::size_t payload_len() const noexcept {
        const auto* udp_hdr = udp();
        if (!udp_hdr) return 0;
        const auto udp_total = static_cast<std::size_t>(ntohs(udp_hdr->length));
        if (udp_total < sizeof(UdpHeader)) return 0;
        return udp_total - sizeof(UdpHeader);
    }

    // ── Convenience ───────────────────────────────────────────────────────────

    [[nodiscard]] bool is_udp() const noexcept { return udp() != nullptr; }
    [[nodiscard]] bool is_ipv4() const noexcept { return ip() != nullptr; }

    [[nodiscard]] uint16_t ether_type_host() const noexcept {
        const auto* e = eth();
        return e ? ntohs(e->ether_type) : 0;
    }
};

} // namespace ultrafast
