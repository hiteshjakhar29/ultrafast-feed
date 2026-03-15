# cmake/CompileOptions.cmake
#
# Defines the ultrafast_compile_options INTERFACE target with all HFT-grade
# compiler and linker flags.  Every target in the project links this target
# so flags are applied uniformly without repeating them.

add_library(ultrafast_compile_options INTERFACE)

target_compile_options(ultrafast_compile_options INTERFACE
    # ── Always-on diagnostics ─────────────────────────────────────────────────
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter          # suppress noise during active development
    -Wno-interference-size         # GCC ABI note on hardware_destructive_interference_size

    # Keep frame pointer in all builds — lets `perf record` / `perf flamegraph`
    # produce useful stacks even in Release.
    -fno-omit-frame-pointer

    # ── Release: maximum throughput ───────────────────────────────────────────
    $<$<CONFIG:Release>:
        -O3
        -march=native              # use AVX-512 / AVX2 on the target GCP instance
        -mtune=native
        -DNDEBUG
        -ffast-math                # safe for integer-heavy feed parsing
        -funroll-loops
    >

    # ── RelWithDebInfo: optimised + debuggable ────────────────────────────────
    $<$<CONFIG:RelWithDebInfo>:
        -O2
        -march=native
        -mtune=native
        -g
        -DNDEBUG
    >

    # ── Debug: sanitisers + full symbols ─────────────────────────────────────
    $<$<CONFIG:Debug>:
        -O0
        -g3
        -fsanitize=address,undefined
        -fno-sanitize-recover=all  # abort on first UB — don't silently continue
    >
)

target_link_options(ultrafast_compile_options INTERFACE
    $<$<CONFIG:Debug>:
        -fsanitize=address,undefined
    >
)

# ── Link Time Optimisation (Release only) ────────────────────────────────────
# Use CMake's IPO machinery — it injects -flto and -fuse-linker-plugin
# for the active compiler (GCC 13 on Ubuntu 24.04) without double-specifying.
# LTO must NOT be combined with ASan (Debug) — the guard prevents that.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        message(STATUS "[ultrafast-feed] LTO enabled for Release build")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "[ultrafast-feed] LTO not available: ${_ipo_err}")
    endif()
endif()
