#!/bin/sh
# ============================================================================
#  Virgil @VERSION@ installer for Raspberry Pi
#
#  You opened this file in a text editor. To INSTALL Virgil instead:
#
#    1. Close this window without saving.
#    2. Open Terminal (the black ">_" icon in the top bar).
#    3. Type or paste this line and press Enter:
#
#         bash ~/Downloads/Virgil-@VERSION@-raspberry-pi-installer.sh
#
#  (If you saved the file somewhere else, use that folder instead of
#  ~/Downloads.) Options: --name "Stage Left"  --interface eth0
# ============================================================================
#
# What it does: installs Virgil and its dependencies (64-bit Raspberry Pi OS
# or other Debian-based Linux), sets the Dante device name (unique per Pi:
# Virgil-<hostname>) and the wired network port, and starts the virgild
# service. Running it again upgrades Virgil and keeps your settings. The
# Virgil .deb package is stored, base64-encoded, at the end of this file.
set -eu

VERSION=@VERSION@
CONF=/etc/virgil/virgil.conf
name=""
iface=""
stage2=0
step="starting"
died=0

say() { printf '%s\n' "$*"; }
die() { died=1; printf '\nVirgil installer: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
	--name) [ $# -ge 2 ] || die "--name needs a value"; name=$2; shift 2 ;;
	--interface) [ $# -ge 2 ] || die "--interface needs a value"; iface=$2; shift 2 ;;
	--stage2) stage2=1; shift ;;  # internal: the part that runs as root
	-h | --help) sed -n '3,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown option: $1 (see --help)" ;;
	esac
done

if [ "$stage2" = 0 ]; then
	# Runs as the user who started it: gets root rights for the real work,
	# keeps a log, and keeps the window open at the end, so nothing vanishes
	# when the installer was started from the file manager.
	set +e  # report failures of the root part instead of vanishing
	case "$0" in /*) self=$0 ;; *) self=$PWD/$0 ;; esac
	log=${HOME:-/tmp}/virgil-install.log
	[ -w "$(dirname "$log")" ] || log=/tmp/virgil-install.log
	st=$(mktemp)
	if [ "$(id -u)" = 0 ]; then
		runner=""
	elif command -v sudo >/dev/null 2>&1; then
		runner=sudo
		say "Virgil needs administrator rights to install. If asked, enter your password."
	else
		runner=none
	fi
	if [ "$runner" = none ]; then
		say "Virgil installer: please run this as root (sudo is not installed)." | tee "$log"
		echo 1 >"$st"
	else
		{ $runner sh "$self" --stage2 ${name:+--name} ${name:+"$name"} ${iface:+--interface} ${iface:+"$iface"} </dev/null
		  echo $? >"$st"; } 2>&1 | tee "$log"
	fi
	rc=$(cat "$st" 2>/dev/null)
	[ -n "$rc" ] || rc=1
	rm -f "$st"
	if [ "$rc" != 0 ]; then
		say ""
		say "The installation did not finish. Everything shown above is saved in"
		say "  $log"
		say "Please send that file along when reporting the problem."
	fi
	if [ -t 0 ] && [ -t 1 ]; then
		say ""
		printf 'Press Enter to close this window. '
		read -r _ || true
	fi
	exit "$rc"
fi

# ---- stage 2: as root --------------------------------------------------------
cleanup() {
	rc=$?
	[ -n "${tmp:-}" ] && rm -rf "$tmp"
	if [ "$rc" != 0 ] && [ "$died" = 0 ]; then
		printf '\nVirgil installer: stopped unexpectedly while %s (exit code %s).\n' "$step" "$rc" >&2
	fi
}
trap cleanup EXIT

say "Virgil $VERSION installer"
say ""

step="checking the system"
command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1 ||
	die "this installer needs a Debian-based system (Raspberry Pi OS, Debian, Ubuntu)"

# The package is attached after the marker line at the end of this file.
step="unpacking the package"
tmp=$(mktemp -d)
line=$(awk '/^__VIRGIL_PACKAGE_BELOW__$/ { print NR + 1; exit }' "$0")
[ -n "$line" ] || die "this file is incomplete; download it again"
tail -n +"$line" "$0" | base64 -d >"$tmp/virgil.deb" 2>/dev/null ||
	die "this file is damaged; download it again"
chmod 644 "$tmp/virgil.deb"
dpkg-deb -I "$tmp/virgil.deb" >/dev/null 2>&1 || die "this file is damaged; download it again"

want=$(dpkg-deb -f "$tmp/virgil.deb" Architecture)
have=$(dpkg --print-architecture)
if [ "$want" != "$have" ]; then
	if [ "$want" = arm64 ] && [ "$have" = armhf ]; then
		die "this Pi runs the 32-bit Raspberry Pi OS, and Virgil needs the 64-bit one.
Use Raspberry Pi Imager to install 'Raspberry Pi OS (64-bit)' (Pi 3, 4, 5 or Zero 2 W)."
	fi
	die "this installer is for $want systems, but this one is $have"
fi

step="installing the package"
say "1/4  Installing Virgil and the libraries it needs..."
# The package's own dependencies (ALSA, the C++ runtime) come from apt.
_apt_ok=0
_apt_dir="$tmp/apt"
mkdir -p "$_apt_dir"
if DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$tmp/virgil.deb" >"$_apt_dir/log" 2>&1; then
	_apt_ok=1
else
	say "     refreshing the package lists and trying again..."
	if DEBIAN_FRONTEND=noninteractive apt-get update -q >>"$_apt_dir/log" 2>&1 &&
		DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$tmp/virgil.deb" >>"$_apt_dir/log" 2>&1; then
		_apt_ok=1
	fi
fi
if [ "$_apt_ok" != 1 ]; then
	tail -n 20 "$_apt_dir/log" >&2
	die "installing the package failed (see above). Is the Pi online?"
fi

step="configuring the device name and network port"
say "2/4  Choosing the Dante device name and network port..."
[ -f "$CONF" ] || die "$CONF is missing after installing; please report this"

conf_get() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" | head -n 1; }
conf_set() {
	# Replace the first "key = ..." line; values never contain '|'.
	if grep -q "^[[:space:]]*$1[[:space:]]*=" "$CONF"; then
		sed -i "0,/^[[:space:]]*$1[[:space:]]*=.*/s||$1 = $2|" "$CONF"
	else
		sed -i "/^\[device\]/a $1 = $2" "$CONF"
	fi
}

# Device name: Dante names must be unique on the network, so a fresh install
# gets Virgil-<hostname> instead of the default "Virgil".
cur_name=$(conf_get name)
if [ -n "$name" ]; then
	new_name=$name
elif [ -z "$cur_name" ] || [ "$cur_name" = Virgil ]; then
	host=$(hostname 2>/dev/null | tr -c 'A-Za-z0-9-\n' '-' | cut -c1-24)
	new_name="Virgil-${host:-pi}"
else
	new_name=$cur_name
fi
new_name=$(printf '%s' "$new_name" | tr -c 'A-Za-z0-9 _-' '-' | cut -c1-31)
[ "$new_name" = "$cur_name" ] || conf_set name "$new_name"

# Network port: the wired one. Wi-Fi drops the clock packets Dante needs.
wired_ports() {
	ip -o -4 addr show 2>/dev/null | awk '{ print $2 }' | grep -E '^(eth|end|enp|ens|enx|usb)' | awk '!seen[$0]++' || true
}
cur_iface=$(conf_get interface)
if [ -n "$iface" ]; then
	ip link show "$iface" >/dev/null 2>&1 || die "there is no network port called '$iface' (see: ip -br addr)"
	conf_set interface "$iface"
elif [ -z "$cur_iface" ]; then
	first=$(wired_ports | head -n 1)
	if [ -n "$first" ]; then
		conf_set interface "$first"
		iface=$first
	else
		say "     No wired network port with an address was found. Connect the Pi to"
		say "     the Dante network with an Ethernet cable (Wi-Fi is not suitable for"
		say "     Dante), then run this installer again, or pick the port later in"
		say "     the control panel."
	fi
else
	iface=$cur_iface
fi

step="setting up CPU tuning"
say "3/4  Tuning the Pi for low-latency audio..."
# Keep the CPU at full speed: frequency changes delay the audio threads.
cat >/etc/systemd/system/virgil-cpu-performance.service <<'UNIT'
[Unit]
Description=Virgil: keep the CPU at full speed for low-latency audio
Before=virgild.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [ -w "$g" ] && echo performance >"$g"; done; true'

[Install]
WantedBy=multi-user.target
UNIT

step="starting the service"
say "4/4  Starting Virgil..."
if [ -d /run/systemd/system ]; then
	systemctl daemon-reload
	systemctl enable --now virgil-cpu-performance.service >/dev/null 2>&1 || true
	systemctl enable virgild.service >/dev/null 2>&1 || true
	systemctl restart virgild.service || true
	sleep 3
	if systemctl is-active --quiet virgild.service; then
		started=1
	else
		started=0
	fi
else
	started=0
	say "     (no systemd here: start it with 'sudo virgild -c $CONF')"
fi

addr=""
[ -n "$iface" ] && addr=$(ip -o -4 addr show "$iface" 2>/dev/null | awk '{ sub(/\/.*/, "", $4); print $4; exit }')
host=$(hostname 2>/dev/null || echo raspberrypi)
user=${SUDO_USER:-pi}

say ""
if [ "$started" = 1 ]; then
	say "Virgil $VERSION is installed and running."
else
	say "Virgil $VERSION is installed, but the service is not running yet."
	[ -d /run/systemd/system ] && say "See why with:  journalctl -u virgild -n 30"
fi
say ""
say "  Dante name:     $new_name   (rename it in the control panel)"
say "  Network port:   ${iface:-not chosen yet}${addr:+ ($addr)}"
say "  Soundcard:      ALSA device 'virgil' (e.g. aplay -D virgil, or JACK/PipeWire)"
say ""
say "Next: in Dante Controller, route channels to and from '$new_name'."
say ""
say "Control panel (meters and settings):"
say "  on this Pi:     http://127.0.0.1:8480/"
say "  from a computer on the same network:"
say "                  ssh -L 8480:127.0.0.1:8480 $user@$host.local"
say "                  then open http://127.0.0.1:8480/ in that computer's browser"
say ""
say "Uninstall: sudo apt remove virgil   (sudo apt purge virgil also deletes the settings)"
exit 0
__VIRGIL_PACKAGE_BELOW__
