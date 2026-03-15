# cmake/FindDependencies.cmake
#
# Locates libbpf and libxdp via pkg-config and creates the
# ultrafast_deps INTERFACE target consumed by every build target.
#
# Required system packages (Ubuntu 24.04):
#   sudo apt install libbpf-dev libxdp-dev

find_package(PkgConfig REQUIRED)

# On some GCP images the multiarch pkg-config path is missing from PKG_CONFIG_PATH.
# Prepend it so pkg_check_modules finds the .pc files under the multiarch prefix.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(ENV{PKG_CONFIG_PATH}
        "/usr/lib/x86_64-linux-gnu/pkgconfig:$ENV{PKG_CONFIG_PATH}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(ENV{PKG_CONFIG_PATH}
        "/usr/lib/aarch64-linux-gnu/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

# IMPORTED_TARGET creates a PkgConfig::<NAME> target that carries include dirs,
# compile flags, and link libraries as transitive INTERFACE properties — always
# prefer this over the raw LIBBPF_LIBRARIES / LIBXDP_LIBRARIES variables.
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
pkg_check_modules(LIBXDP REQUIRED IMPORTED_TARGET libxdp)

# Single INTERFACE target consumed by all downstream targets.
# Link order: libxdp before libbpf because libxdp depends on libbpf internally.
add_library(ultrafast_deps INTERFACE)
target_link_libraries(ultrafast_deps INTERFACE
    PkgConfig::LIBXDP
    PkgConfig::LIBBPF
)
