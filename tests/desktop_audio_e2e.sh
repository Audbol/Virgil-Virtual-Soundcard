#!/usr/bin/env bash
# Desktop audio end to end on Linux, with a private PulseAudio server (null
# sinks, nothing audible) and a free-running virgild:
#   1. everything the default output plays -> Virgil transmit channels 3 + 4
#   2. one app (a named paplay) -> transmit channel 1 (mono)
#   3. all apps (two players on two devices) -> channels 1 + 2
#   4. round trip device -> Virgil -> loop back -> receive channels -> another
#      device, with the latency of tone bursts
#   5. the same with Virgil's media clock 150 ppm fast (drift following)
#   6. an audio interface's inputs 2 + 3 (of a 4-channel pipe source this
#      script's harness feeds) -> transmit channels 5 + 6: exact values, latency
#   7. receive channels 2-5 -> outputs 3-6 of a 6-channel "interface" (a null
#      sink, read back with parec): exact values, silent other outputs, latency
#
# usage: tests/desktop_audio_e2e.sh BUILD_DIR
#   PULSEAUDIO=/path/to/pulseaudio   the server binary (default: from PATH)
#   PULSE_MODULES=/path/to/modules   its module folder, if not the built-in one
# Needs pactl, paplay and python3 (for the test tones). PipeWire users: the
# script starts its own PulseAudio and does not touch the session's server.
set -euo pipefail
build=$(cd "${1:?usage: desktop_audio_e2e.sh BUILD_DIR}" && pwd)
harness=$build/tests/virgil_desktop_audio_e2e
pulseaudio=${PULSEAUDIO:-pulseaudio}
work=$(mktemp -d)
pids=()
status=0

cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -f /dev/shm/virgil-da-e2e-$$
  echo "logs kept in $work"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR=$work/run HOME=$work
mkdir -m 700 -p "$XDG_RUNTIME_DIR"
unset PULSE_SERVER
cat > "$work/test.pa" <<EOF
load-module module-native-protocol-unix auth-anonymous=1
load-module module-null-sink sink_name=speakers rate=48000 channels=2 sink_properties=device.description=Speakers
load-module module-null-sink sink_name=headphones rate=48000 channels=2 sink_properties=device.description=Headphones
load-module module-pipe-source source_name=interface4 file=$work/interface4.fifo format=float32le rate=48000 channels=4 channel_map=aux0,aux1,aux2,aux3 source_properties=device.description=Test-Interface
load-module module-null-sink sink_name=interface6 format=float32le rate=48000 channels=6 channel_map=aux0,aux1,aux2,aux3,aux4,aux5 sink_properties=device.description=Test-Interface-Out
set-default-sink speakers
EOF
pa_args=(-n -F "$work/test.pa" --exit-idle-time=-1 --daemonize=no --system=false --disallow-exit
         --use-pid-file=no --log-target=stderr)
[ -n "${PULSE_MODULES:-}" ] && pa_args+=(-p "$PULSE_MODULES")
"$pulseaudio" "${pa_args[@]}" > "$work/pulseaudio.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do pactl info > /dev/null 2>&1 && break; sleep 0.1; done
pactl info > /dev/null || { echo "PulseAudio did not start (see $work/pulseaudio.log)"; exit 1; }

python3 -I - "$work" <<'EOF'
import array, math, sys, wave
def tone(name, freq, secs, bursts=False, rate=48000, amp=0.5):
    a = array.array('h')
    for n in range(int(secs * rate)):
        t = n / rate
        v = amp * math.sin(2 * math.pi * freq * t)
        if bursts and t % 1.0 >= 0.1:
            v = 0.0
        s = int(v * 32767)
        a.extend((s, s))
    w = wave.open(sys.argv[1] + '/' + name, 'wb')
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(a.tobytes()); w.close()
tone('tone1k.wav', 1000, 120)
tone('tone440.wav', 440, 120)
tone('bursts.wav', 1000, 120, bursts=True)
EOF

shm=/virgil-da-e2e-$$
cat > "$work/virgil.conf" <<EOF
[device]
name = DA-E2E
interface = 127.0.0.1
tx_channels = 8
rx_channels = 4
clock = free
control_port = 0
lock_memory = false
rt_priority = 0
shm_name = $shm
EOF
"$build/virgild" -c "$work/virgil.conf" --log "$work/virgild.log" > /dev/null 2>&1 &
pids+=($!)
sleep 1

play() { paplay "$@" & pids+=($!); }
stop_players() { pkill -P $$ paplay 2>/dev/null || true; sleep 0.3; }
run() {
  echo "== $1"; shift
  if "$@" > "$work/step.log" 2>&1; then grep RESULT "$work/step.log"; else
    grep RESULT "$work/step.log" || tail -20 "$work/step.log"; echo "   FAILED"; status=1; fi
  cat "$work/step.log" >> "$work/harness.log"
}

play "$work/tone1k.wav"
sleep 2.5  # a null sink takes up to its 2 s block to take a lower latency
run "default output device -> tx 3 + 4" "$harness" tx --shm "$shm" --source device:default --channel 3 --seconds 4
stop_players

play --client-name="Tone Player" "$work/tone440.wav"
sleep 2.5
run "one app (\"Tone Player\") -> tx 1, mono" "$harness" tx --shm "$shm" --source "app:Tone Player" --channel 1 --mono \
  --freq 440 --seconds 4
play -d headphones "$work/tone1k.wav"
sleep 2.5
run "all apps (440 Hz + 1 kHz) -> tx 1 + 2" "$harness" tx --shm "$shm" --source all --channel 1 --share 0.4 --seconds 4
stop_players

(sleep 4; exec paplay --latency-msec=20 "$work/bursts.wav") & pids+=($!)
run "round trip speakers -> Virgil -> headphones (latency)" "$harness" loop --source device:speakers \
  --out headphones --measure speakers --seconds 24
stop_players

play "$work/tone1k.wav"
VIRGIL_DA_DEBUG=1 run "drift: Virgil's clock +150 ppm, 60 s" "$harness" loop --source device:speakers --out headphones \
  --ppm 150 --seconds 60
grep -E "source 1|output:" "$work/step.log" | tail -2
stop_players

run "interface inputs 2 + 3 (of 4) -> tx 5 + 6" "$harness" inputs --shm "$shm" --fifo "$work/interface4.fifo" \
  --dev-ch 4 --source input:interface4 --first 2 --count 2 --channel 5 --seconds 12
run "rx 2-5 -> interface outputs 3-6 (of 6)" "$harness" outputs --out interface6 --dev-ch 6 --first 3 --count 4 \
  --channel 2 --seconds 12
[ $status = 0 ] && echo "all passed" || echo "some FAILED"
exit $status
