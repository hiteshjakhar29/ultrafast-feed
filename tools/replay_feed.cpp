/// replay_feed — deterministic FeedEvent replay tool
///
/// Usage:
///   replay_feed <capture.pcap> [rate_multiplier]
///   replay_feed --generate <output.pcap> [count]
///
/// rate_multiplier: 1.0 = original speed (default), 2.0 = 2× faster, 0.5 = half speed.
///
/// --generate creates a synthetic capture file with evenly-spaced events for testing.

#include "ultrafast/pcap_reader.hpp"
#include "ultrafast/pcap_writer.hpp"

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>
#include <vector>

using namespace ultrafast;

static uint64_t clock_ns() noexcept {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static void usage(const char* prog) {
    std::printf(
        "Usage:\n"
        "  %s <capture.pcap> [rate]    replay capture at rate× speed (default 1.0)\n"
        "  %s --generate <out> [n]     generate synthetic capture with n events (default 100)\n",
        prog, prog);
}

// ── generate mode ─────────────────────────────────────────────────────────────

static int do_generate(const std::string& path, uint64_t count) {
    PcapWriter w(path);
    if (!w.is_open()) {
        std::fprintf(stderr, "error: cannot create %s\n", path.c_str());
        return 1;
    }

    // Synthetic events: 500 µs inter-event spacing, payload filled with seq byte
    const uint64_t base_ns = clock_ns();
    for (uint64_t i = 0; i < count; ++i) {
        FeedEvent ev{};
        ev.inject_ns   = base_ns + i * 500'000ULL;
        ev.receive_ns  = ev.inject_ns + 1'500ULL;
        ev.payload_len = static_cast<uint16_t>(FeedEvent::kMaxPayload);
        std::memset(ev.payload, static_cast<int>(i & 0xFF), FeedEvent::kMaxPayload);
        std::memcpy(ev.payload, &ev.inject_ns, sizeof(ev.inject_ns)); // first 8 bytes = ts
        if (!w.write_event(ev)) {
            std::fprintf(stderr, "error: write_event failed at i=%" PRIu64 "\n", i);
            return 1;
        }
    }

    std::printf("generated %" PRIu64 " events → %s\n", count, path.c_str());
    return 0;
}

// ── replay mode ───────────────────────────────────────────────────────────────

static int do_replay(const std::string& path, double rate) {
    PcapReader reader;
    if (!reader.open(path)) {
        std::fprintf(stderr, "error: cannot open or invalid capture: %s\n", path.c_str());
        return 1;
    }

    const uint64_t total = reader.total_events();
    std::printf("file           : %s\n",         path.c_str());
    std::printf("total events   : %" PRIu64 "\n", total);
    std::printf("rate           : %.2f×\n\n",     rate);

    if (total == 0) {
        std::printf("no events to replay\n");
        return 0;
    }

    uint64_t             count = 0;
    std::vector<int64_t> drifts;
    drifts.reserve(static_cast<std::size_t>(total));

    reader.replay_at_rate(rate,
        [&](const FeedEvent& /*ev*/, uint64_t scheduled_ns) {
            const int64_t drift =
                static_cast<int64_t>(clock_ns()) -
                static_cast<int64_t>(scheduled_ns);
            drifts.push_back(drift);
            ++count;
        });

    if (count == 0) {
        std::printf("no events replayed\n");
        return 0;
    }

    // ── statistics ─────────────────────────────────────────────────────────────

    std::sort(drifts.begin(), drifts.end());

    int64_t sum = 0;
    int64_t min_drift = drifts.front();
    int64_t max_drift = drifts.back();
    for (auto d : drifts) sum += d;
    const double mean_drift = static_cast<double>(sum) / static_cast<double>(count);

    const auto pct = [&](double p) {
        const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(count - 1));
        return drifts[idx];
    };

    std::printf("replay complete\n");
    std::printf("events replayed: %" PRIu64 " / %" PRIu64 "\n", count, total);
    std::printf("\ntiming accuracy (actual − scheduled delivery, nanoseconds):\n");
    std::printf("  mean  : %+.0f ns\n",       mean_drift);
    std::printf("  min   : %+" PRId64 " ns\n", min_drift);
    std::printf("  p50   : %+" PRId64 " ns\n", pct(0.50));
    std::printf("  p99   : %+" PRId64 " ns\n", pct(0.99));
    std::printf("  max   : %+" PRId64 " ns\n", max_drift);
    return 0;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (std::strcmp(argv[1], "--generate") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        const std::string path  = argv[2];
        const uint64_t    count = (argc >= 4) ? static_cast<uint64_t>(std::stoull(argv[3]))
                                              : 100ULL;
        return do_generate(path, count);
    }

    const std::string path = argv[1];
    const double      rate = (argc >= 3) ? std::stod(argv[2]) : 1.0;
    return do_replay(path, rate);
}
