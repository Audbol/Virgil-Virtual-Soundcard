#!/bin/sh
# Make the one-file Raspberry Pi installer: installer.sh with the .deb attached.
#   packaging/raspberry-pi/build-installer.sh dist/virgil_<ver>_arm64.deb [OUT]
set -eu
deb=${1:?usage: build-installer.sh PACKAGE.deb [OUT]}
version=$(dpkg-deb -f "$deb" Version)
out=${2:-$(dirname "$deb")/Virgil-$version-raspberry-pi-installer.sh}
here=$(dirname "$0")
sed "s/@VERSION@/$version/g" "$here/installer.sh" >"$out"
# Base64 keeps the whole file valid text, so a text editor opens it (and
# shows the instructions at the top) instead of refusing it.
base64 "$deb" >>"$out"
chmod 755 "$out"
ls -l "$out"
