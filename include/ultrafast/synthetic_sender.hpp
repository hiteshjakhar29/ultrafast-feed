#pragma once

#include <arpa/inet.h>    // htons, inet_pton
#include <fcntl.h>        // open, O_RDONLY, O_CLOEXEC
#include <netinet/in.h>   // sockaddr_in, IPPROTO_UDP
#include <sched.h>        // setns, CLONE_NEWNET
#include <sys/socket.h>   // socket, sendto, bind, AF_INET, SOCK_DGRAM
#include <time.h>         // clock_gettime, CLOCK_MONOTONIC
#include <unistd.h>       // close, usleep

#include <cerrno>
#include <cstdint>
#include <cstring>        // memcpy, memset, strerror
#include <stdexcept>
#include <string>

namespace ultrafast {

/// Header-only UDP packet injector for end-to-end AF_XDP testing.
///
/// Opens a SOCK_DGRAM socket bound to src_ip (which must be assigned to the
/// outgoing interface, e.g. veth0 = 10.11.0.1) so the kernel routes traffic
/// through the correct veth peer without requiring CAP_NET_RAW.
///
/// When ns_name is non-empty the constructor temporarily enters the named
/// network namespace (e.g. "sender_ns") via setns(2) to create and bind the
/// socket there, then restores the original namespace.  The socket retains a
/// reference to the namespace it was created in, so all subsequent sendto()
/// calls are routed through that namespace's network stack — ensuring packets
/// physically cross the veth pair instead of being short-circuited through
/// loopback.
///
/// Each datagram is 64 bytes:
///   [uint64_t inject_ns][0xAB × 56]
/// where inject_ns = CLOCK_MONOTONIC ns recorded immediately before sendto().
/// FeedHandler extracts it from FeedEvent::inject_ns for latency measurement.
class SyntheticSender {
public:
    /// @param src_ip   Source IP on the sending interface (e.g. "10.11.0.1")
    /// @param dst_ip   Destination IP on the receiving interface (e.g. "10.11.0.2")
    /// @param dst_port Destination UDP port (e.g. 9999)
    /// @param ns_name  Optional network namespace name (e.g. "sender_ns").
    ///                 If non-empty the socket is created inside that namespace.
    SyntheticSender(const std::string& src_ip,
                    const std::string& dst_ip,
                    uint16_t dst_port,
                    const std::string& ns_name = "")
    {
        // ── Optionally enter a network namespace ─────────────────────────────
        int saved_ns_fd = -1;

        if (!ns_name.empty()) {
            // Save a handle to the calling thread's current network namespace
            // so we can restore it after the socket is created.
            saved_ns_fd = ::open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
            if (saved_ns_fd < 0) {
                throw std::runtime_error(
                    "SyntheticSender: failed to open /proc/self/ns/net");
            }

            const std::string ns_path = "/var/run/netns/" + ns_name;
            int ns_fd = ::open(ns_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (ns_fd < 0) {
                ::close(saved_ns_fd);
                throw std::runtime_error(
                    "SyntheticSender: failed to open namespace: " + ns_name);
            }

            if (::setns(ns_fd, CLONE_NEWNET) != 0) {
                const int err = errno;
                ::close(ns_fd);
                ::close(saved_ns_fd);
                throw std::runtime_error(
                    "SyntheticSender: setns() failed for: " + ns_name +
                    " errno=" + std::to_string(err) +
                    " (" + ::strerror(err) + ")");
            }
            ::close(ns_fd);
            // We are now executing in ns_name's network namespace.
        }

        // ── Create and bind the socket (in the current namespace) ────────────
        sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_fd_ < 0) {
            if (saved_ns_fd >= 0) {
                ::setns(saved_ns_fd, CLONE_NEWNET);
                ::close(saved_ns_fd);
            }
            throw std::runtime_error("SyntheticSender: socket() failed");
        }

        // Bind to the source IP so the kernel routes egress through the
        // interface where src_ip is assigned (e.g. veth0 inside sender_ns).
        struct sockaddr_in src_addr{};
        src_addr.sin_family = AF_INET;
        src_addr.sin_port   = 0;  // kernel picks ephemeral source port
        if (::inet_pton(AF_INET, src_ip.c_str(), &src_addr.sin_addr) != 1) {
            ::close(sock_fd_);
            if (saved_ns_fd >= 0) {
                ::setns(saved_ns_fd, CLONE_NEWNET);
                ::close(saved_ns_fd);
            }
            throw std::runtime_error(
                "SyntheticSender: invalid src_ip: " + src_ip);
        }
        if (::bind(sock_fd_,
                   reinterpret_cast<const struct sockaddr*>(&src_addr),
                   sizeof(src_addr)) != 0) {
            ::close(sock_fd_);
            if (saved_ns_fd >= 0) {
                ::setns(saved_ns_fd, CLONE_NEWNET);
                ::close(saved_ns_fd);
            }
            throw std::runtime_error(
                "SyntheticSender: bind() to " + src_ip + " failed");
        }

        // Pre-build the destination address used by every sendto().
        dst_addr_ = {};
        dst_addr_.sin_family = AF_INET;
        dst_addr_.sin_port   = htons(dst_port);
        if (::inet_pton(AF_INET, dst_ip.c_str(), &dst_addr_.sin_addr) != 1) {
            ::close(sock_fd_);
            if (saved_ns_fd >= 0) {
                ::setns(saved_ns_fd, CLONE_NEWNET);
                ::close(saved_ns_fd);
            }
            throw std::runtime_error(
                "SyntheticSender: invalid dst_ip: " + dst_ip);
        }

        // ── Restore the original network namespace ────────────────────────────
        if (saved_ns_fd >= 0) {
            ::setns(saved_ns_fd, CLONE_NEWNET);
            ::close(saved_ns_fd);
        }
        // The socket fd retains its association with ns_name's network stack;
        // future sendto() calls are routed through that namespace.
    }

    ~SyntheticSender() {
        if (sock_fd_ >= 0) {
            ::close(sock_fd_);
        }
    }

    SyntheticSender(const SyntheticSender&)            = delete;
    SyntheticSender& operator=(const SyntheticSender&) = delete;

    /// Send one 64-byte UDP datagram.
    /// Stamps clock_ns() into payload[0..7] immediately before sendto()
    /// to minimise the gap between the recorded timestamp and actual transmission.
    /// Returns the inject_ns written into the payload.
    uint64_t send_one() {
        alignas(8) uint8_t buf[64]{};
        const uint64_t ts = clock_ns();
        std::memcpy(buf, &ts, sizeof(ts));
        std::memset(buf + sizeof(ts), 0xAB, sizeof(buf) - sizeof(ts));
        const ssize_t ret = ::sendto(sock_fd_, buf, sizeof(buf), 0,
                 reinterpret_cast<const struct sockaddr*>(&dst_addr_),
                 sizeof(dst_addr_));
        if (ret < 0) {
            const int err = errno;
            throw std::runtime_error(
                std::string("SyntheticSender: sendto() failed: ") +
                "errno=" + std::to_string(err) +
                " (" + ::strerror(err) + ")");
        }
        return ts;
    }

    /// Send @p count packets with @p inter_us microseconds between each.
    void send_burst(int count, int inter_us = 500) {
        for (int i = 0; i < count; ++i) {
            send_one();
            if (inter_us > 0) {
                ::usleep(static_cast<unsigned>(inter_us));
            }
        }
    }

    /// CLOCK_MONOTONIC nanoseconds — same clock used by FeedHandler.
    static uint64_t clock_ns() noexcept {
        struct timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

private:
    int                sock_fd_  = -1;
    struct sockaddr_in dst_addr_ = {};
};

} // namespace ultrafast
