# tests/CMakeLists.txt
#
# Each logical unit gets its own test executable so CTest reports granular
# pass/fail and individual tests can be run in isolation:
#
#   ctest -R spsc_ring_buffer   # run only the ring buffer tests
#   ./tests/test_umem           # run umem tests directly (useful with gdb)

foreach(_test spsc_ring_buffer umem xdp_socket)
    add_executable(test_${_test} test_${_test}.cpp)

    target_link_libraries(test_${_test} PRIVATE
        ultrafast_core            # brings include dirs, libbpf, libxdp
        GTest::gtest_main         # provides main(); initialises GTest
        ultrafast_compile_options # HFT flags + ASan in Debug
    )

    # Register with CTest.  Benchmark executables are intentionally NOT added
    # here because ctest timeouts would kill long-running benchmark runs.
    add_test(NAME ${_test} COMMAND test_${_test})

    # Allow the test to find all ultrafast headers even if ultrafast_core
    # somehow doesn't propagate them (belt-and-suspenders).
    target_include_directories(test_${_test} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
endforeach()
