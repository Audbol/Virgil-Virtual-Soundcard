#!/usr/bin/env bash
# End-to-end Dante test on one Linux machine (needs root and iproute2).
#
# Builds a private test network out of network namespaces:
#   vg-a (10.77.0.1)  virgild "VirgilA", clock master
#   vg-b (10.77.0.2)  virgild "VirgilB"
#   vg-c (10.77.0.3)  controller: a Dante controller CLI, e.g. netaudio
# joined by a bridge in vg-hub. Then subscribes B's receive channels 1-2 to A's
# transmit channels 1-2 and checks that a tone played into A arrives at B.
#
# usage: sudo tests/e2e_netns.sh BUILD_DIR [NETAUDIO_BIN]
# Without netaudio the network is left running for manual tests (Ctrl+C ends).
set -euo pipefail
build=$(cd "${1:?usage: e2e_netns.sh BUILD_DIR [NETAUDIO_BIN]}" && pwd)
netaudio=${2:-}
work=$(mktemp -d)
pids=()
status=0

cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  for n in vg-a vg-b vg-c vg-hub; do ip netns del "$n" 2>/dev/null || true; done
  rm -f /dev/shm/virgil-e2e-a /dev/shm/virgil-e2e-b
  echo "logs kept in $work"
}
trap cleanup EXIT

ip netns add vg-hub
ip -n vg-hub link add br0 type bridge mcast_snooping 0
ip -n vg-hub link set br0 up
i=1
for n in a b c; do
  ip netns add "vg-$n"
  ip link add "v$n" netns "vg-$n" type veth peer name "p$n" netns vg-hub
  ip -n vg-hub link set "p$n" master br0 up
  ip -n "vg-$n" link set lo up
  ip -n "vg-$n" addr add "10.77.0.$i/24" dev "v$n"
  ip -n "vg-$n" link set "v$n" up
  ip -n "vg-$n" route add 224.0.0.0/4 dev "v$n"
  i=$((i + 1))
done

conf() {  # name shm master
  cat >"$work/$1.conf" <<EOF
[device]
name = $1
shm_name = $2
sample_rate = 48000
tx_channels = 8
rx_channels = 8
latency_us = ${LAT_US:-4000}
tx_latency_us = ${TXLAT_US:-4000}
control_port = 0
lock_memory = false
[ptp]
master_capable = $3
EOF
}
conf VirgilA virgil-e2e-a true
conf VirgilB virgil-e2e-b false
# Separate Inferno state per device.
HOME="$work/home-a" ip netns exec vg-a "$build/virgild" -c "$work/VirgilA.conf" -i va \
  >"$work/a.log" 2>&1 &
pids+=($!)
HOME="$work/home-b" ip netns exec vg-b "$build/virgild" -c "$work/VirgilB.conf" -i vb \
  >"$work/b.log" 2>&1 &
pids+=($!)

echo "waiting for B to lock to A's clock..."
for _ in $(seq 60); do
  grep -q "following master" "$work/b.log" 2>/dev/null && break
  sleep 0.5
done
sleep 5
grep -E "ptp|locked|up on" "$work/a.log" "$work/b.log" | head -20 || true

if [ -z "$netaudio" ]; then
  echo "network up. Controller namespace: sudo ip netns exec vg-c <dante tool>"
  echo "logs: $work. Ctrl+C to stop."
  wait
  exit 0
fi

na() { HOME="$work/home-c" ip netns exec vg-c "$netaudio" "$@"; }
echo "--- devices seen from the controller"
for _ in $(seq 20); do
  out=$(na status 2>&1 || true)
  echo "$out" | grep -q VirgilB && echo "$out" | grep -q VirgilA && break
  sleep 1
done
echo "$out"

echo "--- subscribe VirgilB rx 1-2 <- VirgilA tx 1-2"
na subscription add --tx "1@VirgilA" --rx "1@VirgilB" || true
na subscription add --tx "2@VirgilA" --rx "2@VirgilB" || true
sleep 3
na subscription list 2>&1 || true

echo "--- tone A -> B"
VIRGIL_SHM_NAME=virgil-e2e-a ip netns exec vg-a "$build/tests/virgil_tone" play 10 &
tone=$!
sleep 1
VIRGIL_TONE_DUMP=${VIRGIL_TONE_DUMP:-} VIRGIL_SHM_NAME=virgil-e2e-b ip netns exec vg-b "$build/tests/virgil_tone" listen 8 ||
  status=$?

# Same path through the ALSA plugin, if it was built: aplay on A, arecord on B.
plugin="$build/drivers/alsa/libasound_module_pcm_virgil.so"
if [ -f "$plugin" ] && command -v aplay >/dev/null && command -v sox >/dev/null; then
  wait "$tone"
  echo "--- ALSA: aplay on A -> arecord on B"
  mkdir -p "$work/alsa"
  cat >"$work/alsa/.asoundrc" <<CONF
pcm_type.virgil { lib "$plugin" }
pcm.virgil_hw { type virgil }
pcm.virgil { type plug slave.pcm "virgil_hw" }
CONF
  sox -n -r 48000 -b 32 -e signed -c 8 "$work/tone.wav" synth 4 sine 440 gain -6
  HOME="$work/alsa" VIRGIL_SHM_NAME=virgil-e2e-b ip netns exec vg-b \
    arecord -q -D virgil -f S32_LE -r 48000 -c 8 -d 6 "$work/rec.wav" &
  rec=$!
  sleep 1
  HOME="$work/alsa" VIRGIL_SHM_NAME=virgil-e2e-a ip netns exec vg-a \
    aplay -q -D virgil "$work/tone.wav"
  wait "$rec"
  # Channel 1 is routed A -> B; expect the 440 Hz tone at -6 dBFS (RMS 0.354).
  sox "$work/rec.wav" -n remix 1 trim 1.5 2 stat 2>&1 | grep -E "RMS +amplitude|Rough +frequency"
fi

exit "$status"
