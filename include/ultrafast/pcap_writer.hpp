#pragma once
// Header-only binary capture writer for FeedEvents.
// No heap allocation after construction; uses plain FILE* for portability.

#include "ultrafast/feed_event.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ultrafast {

/// Writes FeedEvents to a deterministic binary capture file.
///
/// File layout (all values little-endian):
///
///   Header (24 bytes):
///     [magic:8][version:4][_pad:4][count:8]
///
///   Per-event records (88 bytes each):
///     [inject_ns:8][receive_ns:8][payload_len:2][_pad:6][payload:64]
///
/// count is initially written as 0 and patched to the actual value
/// when close() (or the destructor) is called.
///
/// Usage:
///   PcapWriter w("capture.pcap");
///   w.write_event(ev);
///   // destructor patches count automatically
class PcapWriter {
public:
    static constexpr uint64_t kMagic      = 0x5546415354464545ULL; // "UFASTFEE"
    static constexpr uint32_t kVersion    = 1;
    static constexpr long     kCountOffset = 16; // byte offset of count in header
    static constexpr std::size_t kRecordSize = 88; // bytes per record

    explicit PcapWriter(const std::string& path) noexcept {
        file_ = std::fopen(path.c_str(), "wb");
        if (file_) write_header();
    }

    ~PcapWriter() noexcept { close(); }

    PcapWriter(const PcapWriter&)            = delete;
    PcapWriter& operator=(const PcapWriter&) = delete;

    [[nodiscard]] bool     is_open()        const noexcept { return file_ != nullptr; }
    [[nodiscard]] uint64_t events_written() const noexcept { return count_; }

    /// Append one event record to the file.
    /// Returns false on I/O error; does NOT close the file.
    [[nodiscard]] bool write_event(const FeedEvent& ev) noexcept {
        if (!file_) return false;

        static constexpr uint8_t kZeroPad[6] = {};

        if (std::fwrite(&ev.inject_ns,          sizeof(ev.inject_ns),   1, file_) != 1) return false;
        if (std::fwrite(&ev.receive_ns,         sizeof(ev.receive_ns),  1, file_) != 1) return false;
        if (std::fwrite(&ev.payload_len,        sizeof(ev.payload_len), 1, file_) != 1) return false;
        if (std::fwrite(kZeroPad,               sizeof(kZeroPad),       1, file_) != 1) return false;
        if (std::fwrite(ev.payload, FeedEvent::kMaxPayload,             1, file_) != 1) return false;

        ++count_;
        return true;
    }

    /// Patch the count field in the header, flush, and close the file.
    /// Safe to call more than once; subsequent calls are no-ops.
    void close() noexcept {
        if (!file_) return;
        std::fseek(file_, kCountOffset, SEEK_SET);
        std::fwrite(&count_, sizeof(count_), 1, file_);
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }

private:
    void write_header() noexcept {
        constexpr uint32_t version = kVersion;
        constexpr uint32_t pad     = 0;
        constexpr uint64_t count   = 0; // patched on close()
        std::fwrite(&kMagic,  sizeof(kMagic),   1, file_);
        std::fwrite(&version, sizeof(version),  1, file_);
        std::fwrite(&pad,     sizeof(pad),       1, file_);
        std::fwrite(&count,   sizeof(count),     1, file_);
    }

    FILE*    file_  = nullptr;
    uint64_t count_ = 0;
};

} // namespace ultrafast
