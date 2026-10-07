#!/bin/sh
# Remove the DSV virtual soundcard. Run with sudo:
#   sudo "/Library/Application Support/DSV/uninstall.sh"
# Pass --purge to also delete the configuration and logs.
set -e
if [ "$(id -u)" != 0 ]; then
	echo "run with sudo" >&2
	exit 1
fi
launchctl bootout system/org.dsv.dsvd 2>/dev/null || \
	launchctl unload /Library/LaunchDaemons/org.dsv.dsvd.plist 2>/dev/null || true
rm -f /Library/LaunchDaemons/org.dsv.dsvd.plist
rm -f /usr/local/bin/dsvd /usr/local/bin/dsv-latency-probe
rm -rf /Library/Audio/Plug-Ins/HAL/DSVAudio.driver
rm -f "/Library/Application Support/DSV/dsv.conf.default" \
	"/Library/Application Support/DSV/dsv.conf.example" \
	"/Library/Application Support/DSV/README.md"
if [ "${1:-}" = "--purge" ]; then
	rm -rf "/Library/Application Support/DSV" /Library/Logs/DSV
fi
pkgutil --forget org.dsv.soundcard >/dev/null 2>&1 || true
killall coreaudiod 2>/dev/null || true
echo "DSV removed.${1:+ Configuration purged.}"
[ "${1:-}" = "--purge" ] || rm -f "/Library/Application Support/DSV/uninstall.sh"
