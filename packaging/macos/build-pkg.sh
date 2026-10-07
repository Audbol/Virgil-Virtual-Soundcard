#!/bin/sh
# Build Virgil-<version>-macos.pkg (universal arm64 + x86_64) into ./dist.
#   packaging/macos/build-pkg.sh
# Optional signing / notarisation (Apple Developer ID):
#   VIRGIL_CODESIGN_ID="Developer ID Application: ..."   signs virgild + the driver
#   VIRGIL_INSTALLER_ID="Developer ID Installer: ..."    signs the .pkg
#   VIRGIL_NOTARY_PROFILE=<notarytool keychain profile> notarises and staples
set -eu
cd "$(dirname "$0")/../.."
build=build-macos-release
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DVIRGIL_BUILD_TESTS=OFF \
	-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build "$build" -j
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")

stage=$(mktemp -d)
root="$stage/root"
support="$root/Library/Application Support/Virgil"
mkdir -p "$root/usr/local/bin" "$root/Library/Audio/Plug-Ins/HAL" \
	"$root/Library/LaunchDaemons" "$support" "$root/Applications"
cp -R "$build/Virgil Control.app" "$root/Applications/"
cp "$build/virgild" "$build/virgil-latency-probe" "$root/usr/local/bin/"
cp -R "$build/drivers/coreaudio/VirgilAudio.driver" "$root/Library/Audio/Plug-Ins/HAL/"
cp packaging/macos/org.virgil.virgild.plist "$root/Library/LaunchDaemons/"
cp packaging/virgil.conf "$support/virgil.conf.default"
cp config/virgil.conf.example README.md LICENSE "$support/"
install -m 755 packaging/macos/uninstall.sh "$support/uninstall.sh"
chmod 755 packaging/macos/scripts/*

if [ -n "${VIRGIL_CODESIGN_ID:-}" ]; then
	codesign --force --options runtime --timestamp -s "$VIRGIL_CODESIGN_ID" \
		"$root/usr/local/bin/virgild" "$root/usr/local/bin/virgil-latency-probe"
	codesign --force --options runtime --timestamp -s "$VIRGIL_CODESIGN_ID" \
		"$root/Library/Audio/Plug-Ins/HAL/VirgilAudio.driver" "$root/Applications/Virgil Control.app"
else
	# Apple silicon refuses to load unsigned code; ad-hoc sign at least.
	codesign --force -s - "$root/usr/local/bin/virgild" "$root/usr/local/bin/virgil-latency-probe"
	codesign --force -s - "$root/Library/Audio/Plug-Ins/HAL/VirgilAudio.driver"
	codesign --force -s - "$root/Applications/Virgil Control.app"
fi

pkgbuild --root "$root" --scripts packaging/macos/scripts \
	--identifier org.virgil.soundcard --version "$version" --install-location / \
	--ownership recommended "$stage/virgil-component.pkg"

mkdir -p dist
out="dist/Virgil-$version-macos.pkg"
productbuild --distribution packaging/macos/distribution.xml \
	--resources packaging/macos/resources --package-path "$stage" \
	${VIRGIL_INSTALLER_ID:+--sign "$VIRGIL_INSTALLER_ID"} "$out"

if [ -n "${VIRGIL_NOTARY_PROFILE:-}" ]; then
	xcrun notarytool submit "$out" --keychain-profile "$VIRGIL_NOTARY_PROFILE" --wait
	xcrun stapler staple "$out"
fi
# Portable zip: driver + daemon + Virgil Control.app in one folder.
port="$stage/Virgil-$version-macos"
mkdir -p "$port"
cp "$root/usr/local/bin/virgild" "$root/usr/local/bin/virgil-latency-probe" "$port/"
cp -R "$root/Applications/Virgil Control.app" "$root/Library/Audio/Plug-Ins/HAL/VirgilAudio.driver" "$port/"
cp packaging/virgil.conf "$port/virgil.conf"
cp README.md LICENSE config/virgil.conf.example packaging/portable/macos/START-HERE.txt "$port/"
install -m 755 packaging/portable/macos/install-driver.command "$port/"
ditto -c -k --sequesterRsrc --keepParent "$port" "dist/Virgil-$version-macos-portable.zip"

rm -rf "$stage"
ls -l "$out" "dist/Virgil-$version-macos-portable.zip"
