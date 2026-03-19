#pragma once
// Header-only binary capture reader / timing-accurate replayer for FeedEvents.

#include "ultrafast/feed_event.hpp"
#include "ultrafast/pcap_writer.hpp"  // for kMagic / kVersion / kCountOffset

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <time.h>  // nanosleep, clock_gettime

namespace ultrafast {

/// Reads FeedEvents back from a capture file produced by PcapWriter.
///
/// Typical use:
///   PcapReader r;
///   r.open("capture.pcap");
///   while (auto ev = r.next()) { ... }
///
/// For timing-accurate replay:
///   r.replay_at_rate(1.0, [](const FeedEvent& ev, uint64_t scheduled_ns) { ... });
///
/// The file format is described in pcap_writer.hpp.
class PcapReader {
public:
    PcapReader()  = default;
    ~PcapReader() noexcept { close(); }

    PcapReader(const PcapReader&)            = delete;
    PcapReader& operator=(const PcapReader&) = delete;

    /// Open and validate a capture file.
    /// Returns false if the file cannot be opened, or magic/version mismatch.
    [[nodiscard]] bool open(const std::string& path) noexcept {
        file_ = std::fopen(path.c_str(), "rb");
        if (!file_) return false;

        uint64_t magic   = 0;
        uint32_t version = 0;
        uint32_t pad     = 0;
        uint64_t count   = 0;

        if (std::fread(&magic,   sizeof(magic),   1, file_) != 1 ||
            std::fread(&version, sizeof(version), 1, file_) != 1 ||
            std::fread(&pad,     sizeof(pad),     1, file_) != 1 ||
            std::fread(&count,   sizeof(count),   1, file_) != 1)
        {
            close();
            return false;
        }

        if (magic != PcapWriter::kMagic || version != PcapWriter::kVersion) {
            close();
            return false;
        }

        total_events_ = count;
        events_read_  = 0;
        return true;
    }

    /// Read the next event from the file.
    /// Returns std::nullopt when the file is exhausted or not open.
    [[nodiscard]] std::optional<FeedEvent> next() noexcept {
        if (!file_ || events_read_ >= total_events_) return std::nullopt;

        FeedEvent ev{};
        uint8_t   pad[6]{};

        if (std::fread(&ev.inject_ns,   sizeof(ev.inject_ns),   1, file_) != 1 ||
            std::fread(&ev.receive_ns,  sizeof(ev.receive_ns),  1, file_) != 1 ||
            std::fread(&ev.payload_len, sizeof(ev.payload_len), 1, file_) != 1 ||
            std::fread(pad,             sizeof(pad),             1, file_) != 1 ||
            std::fread(ev.payload, FeedEvent::kMaxPayload,       1, file_) != 1)
        {
            return std::nullopt;
        }

        ++events_read_;
        return ev;
    }

    /// Rewind to the first event (repositions cursor past the 24-byte header).
    [[nodiscard]] bool rewind() noexcept {
        if (!file_) return false;
        events_read_ = 0;
        return std::fseek(file_, static_cast<long>(PcapWriter::kCountOffset + 8),
                          SEEK_SET) == 0;
    }

    /// Replay events at rate_multiplier × original speed.
    ///
    ///   1.0 = wall-clock matches original inter-event timing
    ///   2.0 = twice as fast (half the inter-event delays)
    ///   0.5 = half speed (double the inter-event delays)
    ///
    /// Automatically rewinds before starting so this can be called repeatedly.
    ///
    /// The callback receives:
    ///   (event, scheduled_ns)  — scheduled_ns is the CLOCK_MONOTONIC target
    ///                            delivery time for this event.
    void replay_at_rate(double rate_multiplier,
                        const std::function<void(const FeedEvent&, uint64_t)>& cb) {
        if (!file_ || total_events_ == 0) return;

        // Always start from the beginning of the event stream
        (void)rewind();

        auto first = next();
        if (!first) return;

        const uint64_t origin_inject_ns = first->inject_ns;
        const uint64_t replay_start_ns  = clock_ns();

        cb(*first, replay_start_ns);

        while (events_read_ < total_events_) {
            auto ev = next();
            if (!ev) break;

            // Offset from first event, scaled by rate
            const double offset_raw =
                static_cast<double>(ev->inject_ns - origin_inject_ns);
            const uint64_t offset_ns =
                static_cast<uint64_t>(offset_raw / rate_multiplier);

            const uint64_t target_ns = replay_start_ns + offset_ns;
            const uint64_t now_ns    = clock_ns();

            if (target_ns > now_ns) {
                const uint64_t sleep_ns = target_ns - now_ns;
                struct timespec ts{};
                ts.tv_sec  = static_cast<time_t>(sleep_ns / 1'000'000'000ULL);
                ts.tv_nsec = static_cast<long>  (sleep_ns % 1'000'000'000ULL);
                ::nanosleep(&ts, nullptr);
            }

            cb(*ev, target_ns);
        }
    }

    /// Number of events declared in the file header.
    [[nodiscard]] uint64_t total_events() const noexcept { return total_events_; }

    /// Number of events read since open() or the last rewind().
    [[nodiscard]] uint64_t events_read()  const noexcept { return events_read_; }

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

    void close() noexcept {
        if (file_) { std::fclose(file_); file_ = nullptr; }
        total_events_ = 0;
        events_read_  = 0;
    }

private:
    static uint64_t clock_ns() noexcept {
        struct timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    FILE*    file_         = nullptr;
    uint64_t total_events_ = 0;
    uint64_t events_read_  = 0;
};

} // namespace ultrafast
