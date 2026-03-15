add_library(ultrafast_core STATIC
    umem.cpp
    xdp_socket.cpp
    feed_handler.cpp
)

# PUBLIC: consumers of ultrafast_core automatically get ${PROJECT_SOURCE_DIR}/include
# on their include path, so they can write #include "ultrafast/umem.hpp" etc.
target_include_directories(ultrafast_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# PUBLIC: propagate libbpf + libxdp link requirements to all consumers.
target_link_libraries(ultrafast_core PUBLIC
    ultrafast_deps
    ultrafast_compile_options
)

set_target_properties(ultrafast_core PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
)
