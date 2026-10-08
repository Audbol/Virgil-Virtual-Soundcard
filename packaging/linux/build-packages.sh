#!/bin/sh
# Build .deb and .rpm packages into ./dist.
#   packaging/linux/build-packages.sh [deb|rpm|portable|all]
#   VIRGIL_ARCH=arm64 packaging/linux/build-packages.sh [deb|portable]
# The ALSA plugin directory differs between distro families, so each package
# type gets its own configure. VIRGIL_ARCH=arm64 cross-builds for 64-bit ARM
# (Raspberry Pi OS 64-bit and other arm64 distributions); see
# .github/setup-arm64-cross.sh for the host packages it needs.
set -eu
cd "$(dirname "$0")/../.."
what=${1:-all}
arch=${VIRGIL_ARCH:-}
mkdir -p dist

cross=""
suffix=""
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)
tarch=x86_64
strip=strip
if [ "$arch" = arm64 ]; then
	cross="-DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/aarch64-linux-gnu.cmake"
	suffix=-arm64
	multiarch=aarch64-linux-gnu
	tarch=arm64
	strip=aarch64-linux-gnu-strip
	export DEB_HOST_ARCH=arm64  # dpkg-shlibdeps resolves arm64 libraries
elif [ -n "$arch" ]; then
	echo "unsupported VIRGIL_ARCH: $arch" >&2
	exit 1
fi

if [ "$what" = deb ] || [ "$what" = all ]; then
	cmake -S . -B build-deb$suffix $cross -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DVIRGIL_BUILD_TESTS=OFF -DVIRGIL_ALSA_PLUGIN_DIR="/usr/lib/$multiarch/alsa-lib"
	cmake --build build-deb$suffix -j
	(cd build-deb$suffix && cpack -G DEB)
	cp build-deb$suffix/*.deb dist/
fi

if { [ "$what" = rpm ] || [ "$what" = all ]; } && [ -z "$arch" ]; then
	cmake -S . -B build-rpm -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DVIRGIL_BUILD_TESTS=OFF -DVIRGIL_ALSA_PLUGIN_DIR=/usr/lib64/alsa-lib
	cmake --build build-rpm -j
	(cd build-rpm && cpack -G RPM)
	cp build-rpm/*.rpm dist/
fi
if [ "$what" = portable ]; then
	# Self-contained tarball. libstdc++/libgcc are linked statically so it
	# runs on any distribution with this build host's glibc or newer.
	static="-static-libstdc++ -static-libgcc"
	b=build-portable$suffix
	cmake -S . -B $b $cross -DCMAKE_BUILD_TYPE=Release -DVIRGIL_BUILD_TESTS=OFF \
		-DCMAKE_EXE_LINKER_FLAGS="$static" -DCMAKE_MODULE_LINKER_FLAGS="$static"
	cmake --build $b -j
	version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' $b/CMakeCache.txt)
	name="Virgil-$version-linux-$tarch"
	port="$b/$name"
	rm -rf "$port" && mkdir -p "$port"
	cp $b/virgild $b/virgil-control $b/virgil-latency-probe \
		$b/drivers/alsa/libasound_module_pcm_virgil.so "$port/"
	$strip "$port/virgild" "$port/virgil-control" "$port/virgil-latency-probe" "$port/libasound_module_pcm_virgil.so"
	cp packaging/virgil.conf "$port/virgil.conf"
	cp README.md LICENSE NOTICE.md config/virgil.conf.example packaging/portable/linux/START-HERE.txt "$port/"
	install -m 755 packaging/portable/linux/setup-alsa.sh "$port/"
	tar -C $b -czf "dist/$name.tar.gz" "$name"
fi
ls -l dist
