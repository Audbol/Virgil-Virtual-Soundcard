#!/bin/sh
# Cross-build the Windows installer from Linux (MinGW-w64 + NSIS).
#   ASIO_SDK_DIR=/path/to/asiosdk packaging/windows/build-installer.sh
# The ASIO SDK is Steinberg's and is not redistributed with Virgil; download it
# from https://www.steinberg.net/asiosdk.
set -eu
cd "$(dirname "$0")/../.."
: "${ASIO_SDK_DIR:?set ASIO_SDK_DIR to the Steinberg ASIO SDK}"
cmake -S . -B build-win-release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
	-DCMAKE_BUILD_TYPE=Release -DVIRGIL_BUILD_TESTS=OFF -DASIO_SDK_DIR="$ASIO_SDK_DIR"
cmake --build build-win-release -j
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' build-win-release/CMakeCache.txt)
bin=build-win-release/bin
mkdir -p "$bin" dist
cp build-win-release/virgild.exe build-win-release/virgil-latency-probe.exe \
	build-win-release/virgil-control.exe build-win-release/drivers/asio/VirgilAsio.dll "$bin/"
x86_64-w64-mingw32-strip "$bin"/*.exe "$bin"/*.dll
makensis -V2 -DVERSION="$version" -DBIN_DIR="$PWD/$bin" -DSRC_DIR="$PWD" \
	-DOUT_FILE="$PWD/dist/Virgil-$version-win64-setup.exe" packaging/windows/virgil.nsi

# Portable zip: unzip, register the ASIO driver, double-click virgil-control.exe.
port="build-win-release/Virgil-$version-windows-x64"
rm -rf "$port" && mkdir -p "$port"
cp "$bin"/virgild.exe "$bin"/virgil-control.exe "$bin"/virgil-latency-probe.exe "$bin"/VirgilAsio.dll "$port/"
cp packaging/virgil.conf "$port/virgil.conf"
cp README.md LICENSE NOTICE.md config/virgil.conf.example packaging/portable/windows/* "$port/"
(cd build-win-release && rm -f "../dist/Virgil-$version-windows-x64-portable.zip" &&
	zip -qr "../dist/Virgil-$version-windows-x64-portable.zip" "Virgil-$version-windows-x64")
ls -l dist/Virgil-*-setup.exe dist/Virgil-*-portable.zip
