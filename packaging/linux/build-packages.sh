#!/bin/sh
# Build .deb and .rpm packages into ./dist.
#   packaging/linux/build-packages.sh [deb|rpm|all]
# The ALSA plugin directory differs between distro families, so each package
# type gets its own configure.
set -eu
cd "$(dirname "$0")/../.."
what=${1:-all}
mkdir -p dist

if [ "$what" = deb ] || [ "$what" = all ]; then
	cmake -S . -B build-deb -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DDSV_BUILD_TESTS=OFF -DDSV_ALSA_PLUGIN_DIR="/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)/alsa-lib"
	cmake --build build-deb -j
	(cd build-deb && cpack -G DEB)
	cp build-deb/*.deb dist/
fi

if [ "$what" = rpm ] || [ "$what" = all ]; then
	cmake -S . -B build-rpm -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DDSV_BUILD_TESTS=OFF -DDSV_ALSA_PLUGIN_DIR=/usr/lib64/alsa-lib
	cmake --build build-rpm -j
	(cd build-rpm && cpack -G RPM)
	cp build-rpm/*.rpm dist/
fi
ls -l dist
