cmake_minimum_required(VERSION 3.25)

project(ultrafast-feed
    VERSION      0.1.0
    DESCRIPTION  "Ultra-low-latency market data feed handler using AF_XDP"
    HOMEPAGE_URL "https://github.com/example/ultrafast-feed"
    LANGUAGES    C CXX
)

# ── Platform guard ────────────────────────────────────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ultrafast-feed requires Linux (AF_XDP / libbpf / libxdp are Linux-only)")
endif()

# ── Language standards ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # -std=c++20, not gnu++20

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ── Default build type ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "CMake build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()
message(STATUS "[ultrafast-feed] Build type: ${CMAKE_BUILD_TYPE}")

# ── Export compile commands (for clangd / IDE tooling) ───────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── CTest must be included before any subdirectory calls add_test() ──────────
include(CTest)
enable_testing()

# ── Custom CMake modules ──────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(FindDependencies)   # → defines ultrafast_deps   INTERFACE target
include(CompileOptions)     # → defines ultrafast_compile_options INTERFACE target

# ── FetchContent for third-party test/bench deps ─────────────────────────────
# MUST be declared + made available BEFORE add_subdirectory(tests) / add_subdirectory(bench)
# so that GTest::gtest_main and benchmark::benchmark_main targets exist.
include(FetchContent)

# ── GoogleTest ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    SYSTEM                                 # suppress warnings from GTest headers
    FIND_PACKAGE_ARGS NAMES GTest          # use system GTest if installed
)
set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)
set(INSTALL_GTEST           OFF CACHE BOOL "" FORCE)

# ── Google Benchmark ──────────────────────────────────────────────────────────
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    SYSTEM
)
# Disable benchmark's own tests — they'd pollute CTest output
set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest benchmark)

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(bench)
add_subdirectory(docs)
add_subdirectory(tools)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "[ultrafast-feed] libbpf  ${LIBBPF_VERSION}")
message(STATUS "[ultrafast-feed] libxdp  ${LIBXDP_VERSION}")
message(STATUS "[ultrafast-feed] Targets: ultrafast_core, test_*, bench_*")
