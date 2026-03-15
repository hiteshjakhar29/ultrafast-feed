// tools/test_sender.cpp
//
// Standalone diagnostic for SyntheticSender.
//
// Run as root (setns requires CAP_SYS_ADMIN):
//   sudo ./build/tools/test_sender
//
// In a parallel terminal, verify packets leave veth0:
//   sudo ip netns exec sender_ns tcpdump -i veth0 -n udp -c 5
//
// Prerequisites: sudo ./tools/setup_veth.sh create

#include "ultrafast/synthetic_sender.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

int main()
{
    std::printf("[test_sender] constructing SyntheticSender in sender_ns ...\n");
    std::fflush(stdout);

    ultrafast::SyntheticSender* sender = nullptr;
    try {
        sender = new ultrafast::SyntheticSender(
            "10.11.0.1",  // src_ip  — veth0 inside sender_ns
            "10.11.0.2",  // dst_ip  — veth1 in root ns
            9999,         // dst_port
            "sender_ns"   // network namespace
        );
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[test_sender] FATAL: constructor threw: %s\n", e.what());
        return 1;
    }
    std::printf("[test_sender] socket created successfully inside sender_ns\n");

    constexpr int kCount = 5;
    int sent = 0;
    for (int i = 0; i < kCount; ++i) {
        try {
            const uint64_t ts = sender->send_one();
            std::printf("[test_sender] packet %d sent  inject_ns=%llu\n",
                        i + 1, static_cast<unsigned long long>(ts));
            ++sent;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[test_sender] packet %d FAILED: %s\n", i + 1, e.what());
        }
        std::fflush(stdout);
    }

    std::printf("[test_sender] sent %d/%d packets\n", sent, kCount);
    delete sender;
    return (sent == kCount) ? 0 : 1;
}
