# Cross-compile for 64-bit ARM Linux (Raspberry Pi OS 64-bit, Ubuntu/Debian
# arm64) from an x86-64 Debian/Ubuntu host. Needs g++-aarch64-linux-gnu,
# libasound2-dev:arm64 (multiarch) and `rustup target add
# aarch64-unknown-linux-gnu`; .github/setup-arm64-cross.sh sets all of it up.
#   cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
# pkg-config: the arm64 .pc files from multiarch packages.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
set(CPACK_RPM_PACKAGE_ARCHITECTURE aarch64)
