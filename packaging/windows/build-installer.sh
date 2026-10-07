#!/bin/sh
# Cross-build the Windows installer from Linux (MinGW-w64 + NSIS).
#   ASIO_SDK_DIR=/path/to/asiosdk packaging/windows/build-installer.sh
# The ASIO SDK is Steinberg's and is not redistributed with DSV; download it
# from https://www.steinberg.net/asiosdk.
set -eu
cd "$(dirname "$0")/../.."
: "${ASIO_SDK_DIR:?set ASIO_SDK_DIR to the Steinberg ASIO SDK}"
cmake -S . -B build-win-release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
	-DCMAKE_BUILD_TYPE=Release -DDSV_BUILD_TESTS=OFF -DASIO_SDK_DIR="$ASIO_SDK_DIR"
cmake --build build-win-release -j
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' build-win-release/CMakeCache.txt)
bin=build-win-release/bin
mkdir -p "$bin" dist
cp build-win-release/dsvd.exe build-win-release/dsv-latency-probe.exe \
	build-win-release/dsv-control.exe build-win-release/drivers/asio/DSVAsio.dll "$bin/"
x86_64-w64-mingw32-strip "$bin"/*.exe "$bin"/*.dll
makensis -V2 -DVERSION="$version" -DBIN_DIR="$PWD/$bin" -DSRC_DIR="$PWD" \
	-DOUT_FILE="$PWD/dist/DSV-$version-win64-setup.exe" packaging/windows/dsv.nsi

# Portable zip: unzip, register the ASIO driver, double-click dsv-control.exe.
port="build-win-release/DSV-$version-windows-x64"
rm -rf "$port" && mkdir -p "$port"
cp "$bin"/dsvd.exe "$bin"/dsv-control.exe "$bin"/dsv-latency-probe.exe "$bin"/DSVAsio.dll "$port/"
cp packaging/dsv.conf "$port/dsv.conf"
cp README.md config/dsv.conf.example packaging/portable/windows/* "$port/"
(cd build-win-release && rm -f "../dist/DSV-$version-windows-x64-portable.zip" &&
	zip -qr "../dist/DSV-$version-windows-x64-portable.zip" "DSV-$version-windows-x64")
ls -l dist/DSV-*-setup.exe dist/DSV-*-portable.zip
