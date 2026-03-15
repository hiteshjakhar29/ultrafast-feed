#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <xdp/xsk.h>   // xsk_umem, xsk_ring_prod, xsk_ring_cons,
                        // XSK_UMEM__DEFAULT_FRAME_SIZE,
                        // XSK_RING_PROD__DEFAULT_NUM_DESCS,
                        // XSK_RING_CONS__DEFAULT_NUM_DESCS

namespace ultrafast {

/// Configuration for the AF_XDP UMEM region.
struct UmemConfig {
    /// Frame size in bytes.  Must be a power of 2, minimum 2048.
    std::size_t frame_size      = XSK_UMEM__DEFAULT_FRAME_SIZE;      // 4096
    /// Total number of frames.  Region size = frame_size * frame_count.
    std::size_t frame_count     = 4096;
    /// Fill ring capacity (power of 2).
    std::size_t fill_size       = XSK_RING_PROD__DEFAULT_NUM_DESCS;  // 2048
    /// Completion ring capacity (power of 2).
    std::size_t completion_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;  // 2048
    /// Attempt to back the region with 2 MiB huge pages (MAP_HUGETLB).
    /// Falls back silently to 4 KiB pages if huge pages are unavailable.
    bool use_huge_pages = true;
};

/// RAII wrapper around xsk_umem and its mmap'd memory region.
///
/// Lifecycle:
///   1. Construct — allocates mmap region (huge-page with fallback) and calls
///      xsk_umem__create().  Throws std::runtime_error on failure.
///   2. populate_fill_ring() — pre-loads the fill ring so the kernel has frames
///      for RX.  Call once before attaching the first XdpSocket.
///   3. alloc_frame() / free_frame() — simple LIFO frame allocator for TX.
///
/// Non-copyable; movable.
class Umem {
public:
    explicit Umem(const UmemConfig& cfg = {});
    ~Umem() noexcept;

    Umem(Umem&& other) noexcept;
    Umem& operator=(Umem&& other) noexcept;

    Umem(const Umem&)            = delete;
    Umem& operator=(const Umem&) = delete;

    // ── Accessors ─────────────────────────────────────────────────────────────

    [[nodiscard]] xsk_umem*   handle()       const noexcept { return umem_;              }
    [[nodiscard]] void*       base()         const noexcept { return region_;            }
    [[nodiscard]] std::size_t region_size()  const noexcept { return region_size_;       }
    [[nodiscard]] std::size_t frame_size()   const noexcept { return cfg_.frame_size;    }
    [[nodiscard]] std::size_t frame_count()  const noexcept { return cfg_.frame_count;   }
    [[nodiscard]] bool        valid()        const noexcept { return umem_ != nullptr;   }
    [[nodiscard]] bool        used_huge_pages() const noexcept { return huge_pages_;     }

    [[nodiscard]] xsk_ring_prod& fill_ring()        noexcept { return fill_ring_;   }
    [[nodiscard]] xsk_ring_cons& completion_ring()  noexcept { return comp_ring_;   }

    [[nodiscard]] std::span<uint8_t> region_span() noexcept {
        return {static_cast<uint8_t*>(region_), region_size_};
    }

    // ── Frame addressing ──────────────────────────────────────────────────────

    /// Byte offset of frame idx from the start of the UMEM region.
    [[nodiscard]] uint64_t frame_offset(std::size_t idx) const noexcept {
        return static_cast<uint64_t>(idx) * static_cast<uint64_t>(cfg_.frame_size);
    }

    /// Host pointer to the start of frame idx.
    [[nodiscard]] uint8_t* frame_ptr(std::size_t idx) noexcept {
        return static_cast<uint8_t*>(region_) + frame_offset(idx);
    }

    // ── Fill ring management ──────────────────────────────────────────────────

    /// Pre-populate the fill ring with min(fill_size, frame_count) frames.
    /// Must be called once after construction, before receiving packets.
    void populate_fill_ring() noexcept;

    // ── LIFO frame allocator (for TX) ─────────────────────────────────────────

    /// Returns a UMEM frame offset ready for TX, or UINT64_MAX if none are free.
    [[nodiscard]] uint64_t alloc_frame() noexcept;

    /// Return a UMEM frame offset to the free pool after TX completion.
    void free_frame(uint64_t offset) noexcept;

private:
    void allocate_region();
    void deallocate_region() noexcept;
    void create_umem();

    UmemConfig    cfg_;
    void*         region_      = nullptr;
    std::size_t   region_size_ = 0;
    bool          huge_pages_  = false;
    xsk_umem*     umem_        = nullptr;
    xsk_ring_prod fill_ring_{};
    xsk_ring_cons comp_ring_{};

    // LIFO free-list for alloc_frame / free_frame
    std::vector<uint64_t> free_frames_;
};

} // namespace ultrafast
