#!/bin/sh
# Build DSV-<version>-macos.pkg (universal arm64 + x86_64) into ./dist.
#   packaging/macos/build-pkg.sh
# Optional signing / notarisation (Apple Developer ID):
#   DSV_CODESIGN_ID="Developer ID Application: ..."   signs dsvd + the driver
#   DSV_INSTALLER_ID="Developer ID Installer: ..."    signs the .pkg
#   DSV_NOTARY_PROFILE=<notarytool keychain profile> notarises and staples
set -eu
cd "$(dirname "$0")/../.."
build=build-macos-release
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DDSV_BUILD_TESTS=OFF \
	-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build "$build" -j
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")

stage=$(mktemp -d)
root="$stage/root"
support="$root/Library/Application Support/DSV"
mkdir -p "$root/usr/local/bin" "$root/Library/Audio/Plug-Ins/HAL" \
	"$root/Library/LaunchDaemons" "$support" "$root/Applications"
cp -R "$build/DSV Control.app" "$root/Applications/"
cp "$build/dsvd" "$build/dsv-latency-probe" "$root/usr/local/bin/"
cp -R "$build/drivers/coreaudio/DSVAudio.driver" "$root/Library/Audio/Plug-Ins/HAL/"
cp packaging/macos/org.dsv.dsvd.plist "$root/Library/LaunchDaemons/"
cp packaging/dsv.conf "$support/dsv.conf.default"
cp config/dsv.conf.example README.md "$support/"
install -m 755 packaging/macos/uninstall.sh "$support/uninstall.sh"
chmod 755 packaging/macos/scripts/*

if [ -n "${DSV_CODESIGN_ID:-}" ]; then
	codesign --force --options runtime --timestamp -s "$DSV_CODESIGN_ID" \
		"$root/usr/local/bin/dsvd" "$root/usr/local/bin/dsv-latency-probe"
	codesign --force --options runtime --timestamp -s "$DSV_CODESIGN_ID" \
		"$root/Library/Audio/Plug-Ins/HAL/DSVAudio.driver" "$root/Applications/DSV Control.app"
else
	# Apple silicon refuses to load unsigned code; ad-hoc sign at least.
	codesign --force -s - "$root/usr/local/bin/dsvd" "$root/usr/local/bin/dsv-latency-probe"
	codesign --force -s - "$root/Library/Audio/Plug-Ins/HAL/DSVAudio.driver"
	codesign --force -s - "$root/Applications/DSV Control.app"
fi

pkgbuild --root "$root" --scripts packaging/macos/scripts \
	--identifier org.dsv.soundcard --version "$version" --install-location / \
	--ownership recommended "$stage/dsv-component.pkg"

mkdir -p dist
out="dist/DSV-$version-macos.pkg"
productbuild --distribution packaging/macos/distribution.xml \
	--resources packaging/macos/resources --package-path "$stage" \
	${DSV_INSTALLER_ID:+--sign "$DSV_INSTALLER_ID"} "$out"

if [ -n "${DSV_NOTARY_PROFILE:-}" ]; then
	xcrun notarytool submit "$out" --keychain-profile "$DSV_NOTARY_PROFILE" --wait
	xcrun stapler staple "$out"
fi
# Portable zip: driver + daemon + DSV Control.app in one folder.
port="$stage/DSV-$version-macos"
mkdir -p "$port"
cp "$root/usr/local/bin/dsvd" "$root/usr/local/bin/dsv-latency-probe" "$port/"
cp -R "$root/Applications/DSV Control.app" "$root/Library/Audio/Plug-Ins/HAL/DSVAudio.driver" "$port/"
cp packaging/dsv.conf "$port/dsv.conf"
cp README.md config/dsv.conf.example packaging/portable/macos/START-HERE.txt "$port/"
install -m 755 packaging/portable/macos/install-driver.command "$port/"
ditto -c -k --sequesterRsrc --keepParent "$port" "dist/DSV-$version-macos-portable.zip"

rm -rf "$stage"
ls -l "$out" "dist/DSV-$version-macos-portable.zip"
