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
