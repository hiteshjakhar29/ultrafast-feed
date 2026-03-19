#include "ultrafast/pcap_reader.hpp"
#include "ultrafast/pcap_writer.hpp"
#include "ultrafast/feed_event.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ultrafast;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string tmp_path() {
    static int n = 0;
    return "/tmp/test_replay_" + std::to_string(++n) + ".pcap";
}

static void rm(const std::string& p) { std::remove(p.c_str()); }

// ── PcapWriter ────────────────────────────────────────────────────────────────

TEST(PcapWriter, OpenCreatesFile) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        EXPECT_TRUE(w.is_open());
        EXPECT_EQ(w.events_written(), 0u);
    }
    FILE* f = std::fopen(p.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fclose(f);
    rm(p);
}

TEST(PcapWriter, HeaderMagicAndVersion) {
    const auto p = tmp_path();
    { PcapWriter w(p); }   // write empty file

    FILE* f = std::fopen(p.c_str(), "rb");
    ASSERT_NE(f, nullptr);

    uint64_t magic   = 0;
    uint32_t version = 0;
    ASSERT_EQ(std::fread(&magic,   8, 1, f), 1u);
    ASSERT_EQ(std::fread(&version, 4, 1, f), 1u);
    std::fclose(f);
    rm(p);

    EXPECT_EQ(magic,   PcapWriter::kMagic);
    EXPECT_EQ(version, PcapWriter::kVersion);
}

TEST(PcapWriter, CountPatchedOnClose) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        FeedEvent ev{};
        (void)w.write_event(ev);
        (void)w.write_event(ev);
        (void)w.write_event(ev);
        EXPECT_EQ(w.events_written(), 3u);
    }

    FILE* f = std::fopen(p.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, static_cast<long>(PcapWriter::kCountOffset), SEEK_SET);
    uint64_t count = 0;
    ASSERT_EQ(std::fread(&count, 8, 1, f), 1u);
    std::fclose(f);
    rm(p);

    EXPECT_EQ(count, 3u);
}

TEST(PcapWriter, FileSizeMatchesRecordCount) {
    const auto p = tmp_path();
    constexpr int kN = 7;
    {
        PcapWriter w(p);
        FeedEvent ev{};
        for (int i = 0; i < kN; ++i) (void)w.write_event(ev);
    }

    FILE* f = std::fopen(p.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fclose(f);
    rm(p);

    // 24-byte header + kN × 88-byte records
    EXPECT_EQ(sz, 24 + kN * static_cast<long>(PcapWriter::kRecordSize));
}

// ── PcapReader ────────────────────────────────────────────────────────────────

TEST(PcapReader, OpenFailsMissingFile) {
    PcapReader r;
    EXPECT_FALSE(r.open("/tmp/__no_such_file_xyz__.pcap"));
}

TEST(PcapReader, OpenFailsBadMagic) {
    const auto p = tmp_path();
    FILE* f = std::fopen(p.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    uint64_t bad = 0xDEADBEEFDEADBEEFULL;
    std::fwrite(&bad, 8, 1, f);
    std::fclose(f);

    PcapReader r;
    EXPECT_FALSE(r.open(p));
    rm(p);
}

TEST(PcapReader, TotalEventsFromHeader) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        FeedEvent ev{};
        for (int i = 0; i < 5; ++i) (void)w.write_event(ev);
    }

    PcapReader r;
    ASSERT_TRUE(r.open(p));
    EXPECT_EQ(r.total_events(), 5u);
    rm(p);
}

TEST(PcapReader, NextExhaustsAfterTotalEvents) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        FeedEvent ev{};
        for (int i = 0; i < 3; ++i) (void)w.write_event(ev);
    }

    PcapReader r;
    ASSERT_TRUE(r.open(p));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(r.next().has_value());
    EXPECT_FALSE(r.next().has_value());
    rm(p);
}

TEST(PcapReader, FieldsRoundTrip) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        FeedEvent ev{};
        ev.inject_ns   = 0xCAFEBABE12345678ULL;
        ev.receive_ns  = 0xDEADBEEF87654321ULL;
        ev.payload_len = 42;
        for (std::size_t i = 0; i < FeedEvent::kMaxPayload; ++i)
            ev.payload[i] = static_cast<uint8_t>(i);
        (void)w.write_event(ev);
    }

    PcapReader r;
    ASSERT_TRUE(r.open(p));
    auto ev = r.next();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->inject_ns,   0xCAFEBABE12345678ULL);
    EXPECT_EQ(ev->receive_ns,  0xDEADBEEF87654321ULL);
    EXPECT_EQ(ev->payload_len, 42u);
    for (std::size_t i = 0; i < FeedEvent::kMaxPayload; ++i)
        EXPECT_EQ(ev->payload[i], static_cast<uint8_t>(i)) << "at payload[" << i << "]";
    rm(p);
}

TEST(PcapReader, RewindResetsPosition) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        FeedEvent ev{};
        for (int i = 0; i < 4; ++i) {
            ev.inject_ns = static_cast<uint64_t>(i);
            (void)w.write_event(ev);
        }
    }

    PcapReader r;
    ASSERT_TRUE(r.open(p));
    for (int i = 0; i < 4; ++i) (void)r.next();
    EXPECT_FALSE(r.next().has_value());

    ASSERT_TRUE(r.rewind());
    EXPECT_EQ(r.events_read(), 0u);

    auto ev = r.next();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->inject_ns, 0u);   // first event after rewind
    rm(p);
}

// ── Determinism ───────────────────────────────────────────────────────────────

TEST(ReplayDeterminism, TwoReplaysProduceIdenticalSequences) {
    const auto p = tmp_path();

    // Capture 100 events with distinctive fields
    {
        PcapWriter w(p);
        ASSERT_TRUE(w.is_open());
        for (int i = 0; i < 100; ++i) {
            FeedEvent ev{};
            ev.inject_ns   = 1'000'000ULL * static_cast<uint64_t>(i);
            ev.receive_ns  = ev.inject_ns + 500ULL;
            ev.payload_len = static_cast<uint16_t>(8 + i % 57);
            // Fill payload with a recognisable pattern per event
            std::memset(ev.payload, i & 0xFF, FeedEvent::kMaxPayload);
            std::memcpy(ev.payload, &ev.inject_ns, sizeof(ev.inject_ns));
            ASSERT_TRUE(w.write_event(ev));
        }
        EXPECT_EQ(w.events_written(), 100u);
    }

    std::vector<FeedEvent> replay1, replay2;
    replay1.reserve(100);
    replay2.reserve(100);

    // First replay
    {
        PcapReader r;
        ASSERT_TRUE(r.open(p));
        ASSERT_EQ(r.total_events(), 100u);
        while (auto ev = r.next()) replay1.push_back(*ev);
    }

    // Second independent replay from same file
    {
        PcapReader r;
        ASSERT_TRUE(r.open(p));
        ASSERT_EQ(r.total_events(), 100u);
        while (auto ev = r.next()) replay2.push_back(*ev);
    }

    ASSERT_EQ(replay1.size(), 100u);
    ASSERT_EQ(replay2.size(), 100u);

    for (std::size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(replay1[i].inject_ns,   replay2[i].inject_ns)   << "inject_ns at i="   << i;
        EXPECT_EQ(replay1[i].receive_ns,  replay2[i].receive_ns)  << "receive_ns at i="  << i;
        EXPECT_EQ(replay1[i].payload_len, replay2[i].payload_len) << "payload_len at i=" << i;
        EXPECT_EQ(std::memcmp(replay1[i].payload,
                              replay2[i].payload,
                              FeedEvent::kMaxPayload), 0)          << "payload at i="     << i;
    }

    rm(p);
}

TEST(ReplayDeterminism, RewindThenReplayMatchesFreshOpen) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        for (int i = 0; i < 10; ++i) {
            FeedEvent ev{};
            ev.inject_ns = static_cast<uint64_t>(i) * 100'000ULL;
            std::memset(ev.payload, i, FeedEvent::kMaxPayload);
            (void)w.write_event(ev);
        }
    }

    // Read once via fresh PcapReader
    std::vector<FeedEvent> fresh;
    {
        PcapReader r;
        ASSERT_TRUE(r.open(p));
        while (auto ev = r.next()) fresh.push_back(*ev);
    }

    // Read again via rewind on same instance
    std::vector<FeedEvent> rewound;
    {
        PcapReader r;
        ASSERT_TRUE(r.open(p));
        while (auto ev = r.next()) (void)ev; // exhaust
        ASSERT_TRUE(r.rewind());
        while (auto ev = r.next()) rewound.push_back(*ev);
    }

    ASSERT_EQ(fresh.size(), rewound.size());
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        EXPECT_EQ(fresh[i].inject_ns, rewound[i].inject_ns) << "at i=" << i;
        EXPECT_EQ(std::memcmp(fresh[i].payload, rewound[i].payload,
                              FeedEvent::kMaxPayload), 0)    << "at i=" << i;
    }

    rm(p);
}

// ── replay_at_rate ────────────────────────────────────────────────────────────

TEST(ReplayAtRate, InvokesCallbackForEveryEvent) {
    const auto p = tmp_path();
    {
        PcapWriter w(p);
        for (int i = 0; i < 8; ++i) {
            FeedEvent ev{};
            ev.inject_ns = static_cast<uint64_t>(i) * 1'000'000ULL; // 1 ms apart
            (void)w.write_event(ev);
        }
    }

    PcapReader r;
    ASSERT_TRUE(r.open(p));

    int  count = 0;
    bool scheduled_monotone = true;
    uint64_t prev_sched = 0;

    // Run at 1000× so the test completes in under 1 ms
    r.replay_at_rate(1000.0, [&](const FeedEvent&, uint64_t sched_ns) {
        if (sched_ns < prev_sched) scheduled_monotone = false;
        prev_sched = sched_ns;
        ++count;
    });

    EXPECT_EQ(count, 8);
    EXPECT_TRUE(scheduled_monotone);
    rm(p);
}

TEST(ReplayAtRate, RateMultiplierScalesDelays) {
    const auto p = tmp_path();
    // Two events 100 ms apart
    {
        PcapWriter w(p);
        FeedEvent ev{};
        ev.inject_ns = 0;
        (void)w.write_event(ev);
        ev.inject_ns = 100'000'000ULL; // 100 ms
        (void)w.write_event(ev);
    }

    PcapReader r;
    ASSERT_TRUE(r.open(p));

    uint64_t t0 = 0, t1 = 0;
    int idx = 0;
    // Run at 200× → inter-event delay becomes 500 µs
    r.replay_at_rate(200.0, [&](const FeedEvent&, uint64_t sched_ns) {
        if (idx == 0) t0 = sched_ns;
        if (idx == 1) t1 = sched_ns;
        ++idx;
    });

    // Expected gap ≈ 100 ms / 200 = 500 µs → 500'000 ns
    const uint64_t gap = t1 - t0;
    EXPECT_GE(gap, 400'000u);    // at least 400 µs
    EXPECT_LT(gap, 2'000'000u);  // at most 2 ms (generous for slow CI)
    rm(p);
}
