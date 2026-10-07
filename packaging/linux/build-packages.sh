#!/bin/sh
# Build .deb and .rpm packages into ./dist.
#   packaging/linux/build-packages.sh [deb|rpm|portable|all]
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
if [ "$what" = portable ]; then
	# Self-contained tarball. libstdc++/libgcc are linked statically so it
	# runs on any distribution with this build host's glibc or newer.
	static="-static-libstdc++ -static-libgcc"
	cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DDSV_BUILD_TESTS=OFF \
		-DCMAKE_EXE_LINKER_FLAGS="$static" -DCMAKE_MODULE_LINKER_FLAGS="$static"
	cmake --build build-portable -j
	version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' build-portable/CMakeCache.txt)
	name="DSV-$version-linux-x86_64"
	port="build-portable/$name"
	rm -rf "$port" && mkdir -p "$port"
	cp build-portable/dsvd build-portable/dsv-control build-portable/dsv-latency-probe \
		build-portable/drivers/alsa/libasound_module_pcm_dsv.so "$port/"
	strip "$port/dsvd" "$port/dsv-control" "$port/dsv-latency-probe" "$port/libasound_module_pcm_dsv.so"
	cp packaging/dsv.conf "$port/dsv.conf"
	cp README.md config/dsv.conf.example packaging/portable/linux/START-HERE.txt "$port/"
	install -m 755 packaging/portable/linux/setup-alsa.sh "$port/"
	tar -C build-portable -czf "dist/$name.tar.gz" "$name"
fi
ls -l dist
