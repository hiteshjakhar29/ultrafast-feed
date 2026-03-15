#include "ultrafast/umem.hpp"

#include <sys/mman.h>   // mmap, munmap, MAP_*, PROT_*

#include <algorithm>    // std::min
#include <cerrno>
#include <cstring>      // strerror
#include <format>       // std::format (GCC 13 / Ubuntu 24.04)
#include <stdexcept>

namespace ultrafast {

namespace {

/// Align size up to the nearest multiple of align_to (must be power of 2).
constexpr std::size_t align_up(std::size_t size, std::size_t align_to) noexcept {
    return (size + align_to - 1U) & ~(align_to - 1U);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

Umem::Umem(const UmemConfig& cfg) : cfg_(cfg) {
    allocate_region();
    create_umem();
    // Pre-populate the LIFO free-list with all frame offsets.
    free_frames_.reserve(cfg_.frame_count);
    for (std::size_t i = 0; i < cfg_.frame_count; ++i) {
        free_frames_.push_back(frame_offset(i));
    }
}

Umem::~Umem() noexcept {
    // xsk_umem__delete must precede munmap: the kernel still holds a reference
    // to the ring metadata that overlaps the mmap'd region until delete returns.
    if (umem_) {
        xsk_umem__delete(umem_);
        umem_ = nullptr;
    }
    deallocate_region();
}

// ─────────────────────────────────────────────────────────────────────────────
// Move semantics
// ─────────────────────────────────────────────────────────────────────────────

Umem::Umem(Umem&& other) noexcept
    : cfg_(other.cfg_)
    , region_(other.region_)
    , region_size_(other.region_size_)
    , huge_pages_(other.huge_pages_)
    , umem_(other.umem_)
    , fill_ring_(other.fill_ring_)
    , comp_ring_(other.comp_ring_)
    , free_frames_(std::move(other.free_frames_))
{
    other.region_      = nullptr;
    other.region_size_ = 0;
    other.umem_        = nullptr;
}

Umem& Umem::operator=(Umem&& other) noexcept {
    if (this != &other) {
        if (umem_) xsk_umem__delete(umem_);
        deallocate_region();

        cfg_         = other.cfg_;
        region_      = other.region_;
        region_size_ = other.region_size_;
        huge_pages_  = other.huge_pages_;
        umem_        = other.umem_;
        fill_ring_   = other.fill_ring_;
        comp_ring_   = other.comp_ring_;
        free_frames_ = std::move(other.free_frames_);

        other.region_      = nullptr;
        other.region_size_ = 0;
        other.umem_        = nullptr;
    }
    return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void Umem::allocate_region() {
    static constexpr std::size_t kHugePage = 2UL << 20U;  // 2 MiB

    const std::size_t raw_size = cfg_.frame_size * cfg_.frame_count;

    if (cfg_.use_huge_pages) {
        // The region size must be a 2 MiB multiple for MAP_HUGETLB.
        const std::size_t aligned_size = align_up(raw_size, kHugePage);
        region_ = mmap(nullptr, aligned_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                       -1, 0);
        if (region_ != MAP_FAILED) {
            region_size_ = aligned_size;
            huge_pages_  = true;
            return;
        }
        // Huge pages unavailable (ENOMEM / not configured) — fall through.
        region_ = MAP_FAILED;
    }

    // Standard 4 KiB pages.
    region_size_ = align_up(raw_size, 4096UL);
    region_ = mmap(nullptr, region_size_,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    if (region_ == MAP_FAILED) {
        region_      = nullptr;
        region_size_ = 0;
        throw std::runtime_error(
            std::format("Umem: mmap({} bytes) failed: {}",
                        region_size_, strerror(errno)));
    }
    huge_pages_ = false;
}

void Umem::deallocate_region() noexcept {
    if (region_) {
        munmap(region_, region_size_);
        region_      = nullptr;
        region_size_ = 0;
    }
}

void Umem::create_umem() {
    // Zero-init first — forward-compatible with future struct extensions.
    struct xsk_umem_config xsk_cfg = {};
    xsk_cfg.fill_size       = static_cast<uint32_t>(cfg_.fill_size);
    xsk_cfg.comp_size       = static_cast<uint32_t>(cfg_.completion_size);
    xsk_cfg.frame_size      = static_cast<uint32_t>(cfg_.frame_size);
    xsk_cfg.frame_headroom  = 0;
    xsk_cfg.flags           = 0;

    const int ret = xsk_umem__create(
        &umem_,
        region_,
        static_cast<uint64_t>(region_size_),
        &fill_ring_,
        &comp_ring_,
        &xsk_cfg);

    if (ret != 0) {
        deallocate_region();
        throw std::runtime_error(
            std::format("xsk_umem__create failed: {}", strerror(-ret)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void Umem::populate_fill_ring() noexcept {
    // Pre-load min(fill_size, frame_count) frames into the fill ring so the
    // kernel has DMA buffers ready for the first batch of incoming packets.
    const uint32_t n = static_cast<uint32_t>(
        std::min(cfg_.fill_size, cfg_.frame_count));

    uint32_t idx = 0;
    const uint32_t reserved = xsk_ring_prod__reserve(&fill_ring_, n, &idx);

    for (uint32_t i = 0; i < reserved; ++i) {
        *xsk_ring_prod__fill_addr(&fill_ring_, idx + i) =
            static_cast<uint64_t>(i) * cfg_.frame_size;
    }

    xsk_ring_prod__submit(&fill_ring_, reserved);
}

uint64_t Umem::alloc_frame() noexcept {
    if (free_frames_.empty()) {
        return UINT64_MAX;  // sentinel: no frames available
    }
    const uint64_t offset = free_frames_.back();
    free_frames_.pop_back();
    return offset;
}

void Umem::free_frame(uint64_t offset) noexcept {
    free_frames_.push_back(offset);
}

} // namespace ultrafast
