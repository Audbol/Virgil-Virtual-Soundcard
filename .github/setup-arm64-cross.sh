#!/bin/sh
# Prepare an x86-64 Ubuntu runner to cross-build Virgil for 64-bit ARM
# (Raspberry Pi OS 64-bit, Ubuntu/Debian arm64): the aarch64 compiler, the
# Rust target and arm64 ALSA libraries from ports.ubuntu.com (multiarch).
set -eu
. /etc/os-release
codename=$VERSION_CODENAME
# Existing sources serve amd64 only; arm64 comes from the ports archive.
if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
	sudo sed -i '/^Types:/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources
fi
if [ -f /etc/apt/sources.list ]; then
	sudo sed -i -E 's/^deb (http|https|mirror)/deb [arch=amd64] \1/' /etc/apt/sources.list
fi
for f in /etc/apt/sources.list.d/*.list; do
	[ -f "$f" ] && sudo sed -i -E 's/^deb (http|https)/deb [arch=amd64] \1/' "$f"
done
cat <<SRC | sudo tee /etc/apt/sources.list.d/arm64-ports.list >/dev/null
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports $codename main universe
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports $codename-updates main universe
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports $codename-security main universe
SRC
sudo dpkg --add-architecture arm64
"$(dirname "$0")/apt-install.sh" g++-aarch64-linux-gnu libasound2-dev:arm64 libstdc++6:arm64 dpkg-dev
rustup target add aarch64-unknown-linux-gnu
