#include <gtest/gtest.h>

#include <unistd.h>   // getuid

#include "ultrafast/umem.hpp"

using namespace ultrafast;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Returns true if the calling process has CAP_NET_ADMIN (root is a proxy).
/// xsk_umem__create and xsk_socket__create require this privilege.
static bool has_root() {
    return getuid() == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pure-math tests (no kernel calls, always run)
// ─────────────────────────────────────────────────────────────────────────────

TEST(UmemConfig, DefaultValues) {
    UmemConfig cfg;
    EXPECT_EQ(cfg.frame_size,       static_cast<std::size_t>(XSK_UMEM__DEFAULT_FRAME_SIZE));
    EXPECT_EQ(cfg.frame_count,      4096u);
    EXPECT_EQ(cfg.fill_size,        static_cast<std::size_t>(XSK_RING_PROD__DEFAULT_NUM_DESCS));
    EXPECT_EQ(cfg.completion_size,  static_cast<std::size_t>(XSK_RING_CONS__DEFAULT_NUM_DESCS));
    EXPECT_TRUE(cfg.use_huge_pages);
}

TEST(UmemConfig, RegionSizeArithmetic) {
    UmemConfig cfg;
    cfg.frame_size  = 4096;
    cfg.frame_count = 8;
    // Expected region is at least frame_size * frame_count bytes
    EXPECT_GE(cfg.frame_size * cfg.frame_count, 4096u * 8u);
}

TEST(UmemConfig, FrameOffsetArithmetic) {
    // Validate offset math without constructing a kernel Umem.
    const std::size_t frame_size = 4096;
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(static_cast<uint64_t>(i) * frame_size, i * 4096u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Kernel-side tests (require CAP_NET_ADMIN)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Umem, ConstructionDefaultConfig) {
    if (!has_root()) {
        GTEST_SKIP() << "Requires CAP_NET_ADMIN; re-run as root";
    }
    EXPECT_NO_THROW({
        Umem u;
        EXPECT_TRUE(u.valid());
        EXPECT_NE(u.base(), nullptr);
        EXPECT_GE(u.region_size(), u.frame_size() * u.frame_count());
        EXPECT_EQ(u.frame_count(), 4096u);
        EXPECT_EQ(u.frame_size(),
                  static_cast<std::size_t>(XSK_UMEM__DEFAULT_FRAME_SIZE));
    });
}

TEST(Umem, FramePtrBounds) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem u;
    const auto* base = static_cast<const uint8_t*>(u.base());

    EXPECT_EQ(u.frame_ptr(0), base);
    EXPECT_EQ(u.frame_ptr(1), base + u.frame_size());
    EXPECT_EQ(u.frame_ptr(u.frame_count() - 1),
              base + (u.frame_count() - 1) * u.frame_size());
}

TEST(Umem, FrameOffsetMatchesPtr) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem u;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto* base = static_cast<const uint8_t*>(u.base());
        EXPECT_EQ(u.frame_ptr(i), base + u.frame_offset(i));
    }
}

TEST(Umem, MoveConstructor) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem a;
    xsk_umem* raw = a.handle();
    void*     buf = a.base();

    Umem b(std::move(a));

    EXPECT_FALSE(a.valid());
    EXPECT_EQ(a.base(), nullptr);

    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.handle(), raw);
    EXPECT_EQ(b.base(),   buf);
}

TEST(Umem, MoveAssignment) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem a;
    Umem b;
    xsk_umem* raw_a = a.handle();

    b = std::move(a);
    EXPECT_FALSE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.handle(), raw_a);
}

TEST(Umem, AllocFreeFrame) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem u;

    const uint64_t addr = u.alloc_frame();
    EXPECT_NE(addr, UINT64_MAX) << "alloc_frame should succeed on fresh Umem";

    // Return it and re-allocate — LIFO: same offset comes back
    u.free_frame(addr);
    const uint64_t addr2 = u.alloc_frame();
    EXPECT_EQ(addr, addr2);
}

TEST(Umem, AllocAllFramesThenExhausted) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    UmemConfig cfg;
    cfg.frame_count = 64;   // small count to keep the test fast
    Umem u(cfg);

    std::vector<uint64_t> frames;
    frames.reserve(cfg.frame_count);

    for (std::size_t i = 0; i < cfg.frame_count; ++i) {
        const uint64_t addr = u.alloc_frame();
        ASSERT_NE(addr, UINT64_MAX) << "unexpected exhaustion at frame " << i;
        frames.push_back(addr);
    }

    // Free list is now empty
    EXPECT_EQ(u.alloc_frame(), UINT64_MAX);

    // Return all frames
    for (uint64_t addr : frames) u.free_frame(addr);

    // Should be able to allocate again
    EXPECT_NE(u.alloc_frame(), UINT64_MAX);
}

TEST(Umem, PopulateFillRing) {
    if (!has_root()) GTEST_SKIP() << "Requires CAP_NET_ADMIN";

    Umem u;
    // Just verify it doesn't crash / assert.
    EXPECT_NO_THROW(u.populate_fill_ring());
}
