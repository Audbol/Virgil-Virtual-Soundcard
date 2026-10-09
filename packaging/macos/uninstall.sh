#!/bin/sh
# Remove the Virgil virtual soundcard. Run with sudo:
#   sudo "/Library/Application Support/Virgil/uninstall.sh"
# Pass --purge to also delete the configuration and logs.
set -e
if [ "$(id -u)" != 0 ]; then
	echo "run with sudo" >&2
	exit 1
fi
launchctl bootout system/org.virgil.virgild 2>/dev/null || \
	launchctl unload /Library/LaunchDaemons/org.virgil.virgild.plist 2>/dev/null || true
rm -f /Library/LaunchDaemons/org.virgil.virgild.plist
rm -f /usr/local/bin/virgild /usr/local/bin/virgil-latency-probe
rm -rf /Library/Audio/Plug-Ins/HAL/VirgilAudio.driver
rm -rf "/Applications/Virgil Control.app"
# Per-user "start at login" entries made by Virgil Control.
for home in /Users/*; do
	rm -f "$home/Library/LaunchAgents/org.virgil.control.plist" 2>/dev/null || true
done
rm -f "/Library/Application Support/Virgil/virgil.conf.default" \
	"/Library/Application Support/Virgil/virgil.conf.example" \
	"/Library/Application Support/Virgil/README.md" \
	"/Library/Application Support/Virgil/LICENSE" \
	"/Library/Application Support/Virgil/NOTICE.md"
if [ "${1:-}" = "--purge" ]; then
	rm -rf "/Library/Application Support/Virgil" /Library/Logs/Virgil
fi
pkgutil --forget org.virgil.soundcard >/dev/null 2>&1 || true
killall coreaudiod 2>/dev/null || true
echo "Virgil removed.${1:+ Configuration purged.}"
[ "${1:-}" = "--purge" ] || rm -f "/Library/Application Support/Virgil/uninstall.sh"
