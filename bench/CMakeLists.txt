# bench/CMakeLists.txt
#
# Benchmark executables are NOT added to CTest (add_test is intentionally
# absent) because benchmarks must be run manually with flags such as
# --benchmark_min_time=2s or --benchmark_filter=BM_Spsc.
#
# Run a benchmark:
#   ./bench/bench_spsc_ring_buffer --benchmark_format=json
#   ./bench/bench_xdp_socket       --benchmark_repetitions=5

foreach(_bench spsc_ring_buffer xdp_socket)
    add_executable(bench_${_bench} bench_${_bench}.cpp)

    target_link_libraries(bench_${_bench} PRIVATE
        ultrafast_core              # brings include dirs, libbpf, libxdp
        benchmark::benchmark_main  # provides main(); parses --benchmark_* flags
        ultrafast_compile_options  # HFT flags (Release: -O3 -march=native)
    )

    target_include_directories(bench_${_bench} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
endforeach()
