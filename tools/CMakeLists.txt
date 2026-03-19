# tools/CMakeLists.txt — diagnostic / developer utilities
#
# These targets are NOT registered with CTest.  Run them manually:
#   sudo ./build/tools/test_sender

add_executable(test_sender test_sender.cpp)

# SyntheticSender is header-only; only needs the include path and compiler flags.
# ultrafast_core (and its libbpf/libxdp deps) are intentionally excluded here
# so this binary can be built and run without libxdp installed.
target_include_directories(test_sender PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_sender PRIVATE
    ultrafast_compile_options
)

# ── replay_feed ────────────────────────────────────────────────────────────────
# PcapWriter/PcapReader are header-only; no libxdp dependency.
add_executable(replay_feed replay_feed.cpp)
target_include_directories(replay_feed PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(replay_feed PRIVATE
    ultrafast_compile_options
)
