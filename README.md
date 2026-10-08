<p align="center"><img src="branding/virgil-256.png" width="160" alt="Virgil logo: a flame of audio meter bars inside a laurel wreath"></p>

# Virgil: a low-latency virtual soundcard for Dante® networks

<p align="center"><em><b>V</b>irtual <b>I</b>nterface <b>R</b>outing <b>G</b>ateway for <b>I</b>nferno-based <b>L</b>ow-latency audio</em></p>

Virgil lets a computer join a **Dante network** as an audio device. In Dante
Controller it shows up next to your other gear, with its own transmit and
receive channels. You
route it like any other device, and your audio apps see an ordinary soundcard:

| OS      | Driver                         | Apps see it as                   |
|---------|--------------------------------|----------------------------------|
| Linux   | ALSA external PCM plugin       | `virgil` ALSA device (JACK, PipeWire, aplay, …) |
| macOS   | CoreAudio AudioServerPlugIn    | "Virgil Virtual Soundcard" system device |
| Windows | ASIO driver (COM in-proc)      | "Virgil" ASIO device |

Virgil (Dante's guide through the *Inferno*) is built on
[**Inferno**](https://github.com/teodly/inferno), an independent open-source
implementation of the Dante protocol by Teodor Woźniak and contributors. Inferno
is vendored in `third_party/inferno` with a few patches; see its README.

> **Independent project.** Virgil is not affiliated with, authorized,
> endorsed or certified by Audinate, and contains no Audinate software,
> firmware or documentation. Dante® is a registered trademark of Audinate Pty
> Ltd., used here only to say which networks Virgil works with. Virgil is free
> software under the GPLv3 (see `LICENSE`) and comes with no warranty. See
> [`NOTICE.md`](NOTICE.md) for all trademarks and third-party licences.

## Using it with Dante Controller

1. Install Virgil and start it (the installers run it as a service).
2. In **Virgil Control**, pick the network interface your Dante network is on.
3. In **Dante Controller**, the computer appears as a device called **Virgil**
   (rename it in Settings). Its receive channels appear in the routing grid.
   Subscribe them to any Dante transmitter, and subscribe other devices to
   Virgil's transmit channels. Virgil remembers subscriptions across restarts.
4. Play into and record from the Virgil soundcard in your apps.

Clocking: Virgil follows the Dante clock master (PTPv1, as Dante uses). If no
other Dante device provides a clock, for example when Virgil machines only
talk to each other, set `master_capable = true` in `[ptp]` on one of them.

Latency: Virgil's **receive latency** (`latency_us`, default 4 ms) works like
the device latency in Dante Controller. Its **transmit latency**
(`tx_latency_us`, default 4 ms): Virgil timestamps its audio that far ahead, so receivers get it in
time whatever their own latency (even 0.25 ms on hardware). Playback through
Virgil therefore reaches the network `tx_latency_us` plus the receiver's
latency after the app plays it. Lower
both on a clean, wired gigabit network. Raise them if you hear dropouts.

Network ports (allow them through the firewall; the Windows installer does):
UDP 319/320 (PTP), 5353 (mDNS), 4400, 4455, 8700 and 8800 (Dante control),
and the audio flows. Virgil needs administrator/root rights for the PTP
ports.

## Architecture

```
 apps ─► ALSA plugin ─┐                               ┌─► Inferno ─► Dante flows ─► network
 apps ─► CoreAudio  ──┼─► shared-memory soundcard ◄───┤   (discovery, control, audio)
 apps ─► ASIO driver ─┘   (lock-free, per-frame        └─◄ Inferno ◄─ Dante devices
                           addressed by media clock)
                                    ▲
                    virgild daemon ── PTPv1 follower (or master), mixer, control panel
```

* **One clock.** `virgild` steers a media clock to the Dante clock master with a
  PI servo. It publishes a `(host time, media frame, rate)` anchor in shared
  memory. Every driver runs its callbacks from that anchor: CoreAudio zero
  timestamps, ASIO buffer switches and the ALSA hw pointer. Inferno gets the
  same clock in-process. Apps therefore run in lock-step with the network.
  There is no resampling and no drift correction anywhere.
* **Rings indexed by absolute media frame.** Frame *t* lives in slot
  `t mod ring`, in the apps' rings and in the 32-bit rings Inferno reads and
  writes. Received audio lands at the slot of its own timestamp plus the
  latency, which makes the ring a free jitter buffer. Playback from up to 8
  clients is summed by the daemon, so several apps can play at once without
  a lock.
* **Real-time path.** The daemon tick uses SCHED_FIFO (Linux), the
  time-constraint policy (macOS) or MMCSS "Pro Audio" (Windows). It wakes on an
  absolute deadline with a short final spin. Memory is `mlock`ed, nothing
  allocates after start-up, and audio threads never lock.

## Installing

Download from the repository's **Releases** page. Each platform has an
installer and a portable archive:

| Platform | Installer | Portable (no install) |
|---|---|---|
| Windows 10/11 x64 | `Virgil-<ver>-win64-setup.exe` | `Virgil-<ver>-windows-x64-portable.zip` |
| macOS 11+ (Apple silicon & Intel) | `Virgil-<ver>-macos.pkg` | `Virgil-<ver>-macos-portable.zip` |
| Debian 12+ / Ubuntu 22.04+ | `virgil_<ver>_amd64.deb` | `Virgil-<ver>-linux-x86_64.tar.gz` |
| Fedora / RHEL / openSUSE | `virgil-<ver>-1.x86_64.rpm` | same tarball |
| Raspberry Pi (64-bit Raspberry Pi OS 12+, other arm64 Linux) | `Virgil-<ver>-raspberry-pi-installer.sh` (one step), or `virgil_<ver>_arm64.deb` | `Virgil-<ver>-linux-arm64.tar.gz` |

**Raspberry Pi:** a Pi 4 or 5 on wired Ethernet works best; a Pi 3 or Zero 2 W
needs a USB Ethernet adapter, and Wi-Fi is not suitable for Dante. Use the
64-bit Raspberry Pi OS (32-bit is not supported).

The easiest way is the one-file installer. Download
`Virgil-<ver>-raspberry-pi-installer.sh` on the Pi, open **Terminal** (the
`>_` icon in the top bar; double-clicking the file only opens it in a text
editor) and run:

```sh
bash ~/Downloads/Virgil-*-raspberry-pi-installer.sh     # optional: --name "Stage Left" --interface eth0
```

Or, once the repository is public, in one line straight from GitHub:

```sh
curl -fsSL https://raw.githubusercontent.com/Audbol/DSV-Dante-Soundcard-Virtual/HEAD/packaging/raspberry-pi/get-virgil.sh | sh
```

The installer installs Virgil and its dependencies, names the device
`Virgil-<hostname>` (Dante names must be unique), picks the wired network
port, keeps the CPU at full speed for low latency, starts the service, and
prints how to reach the control panel: http://127.0.0.1:8480/ on the Pi, or
from another computer through `ssh -L 8480:127.0.0.1:8480 pi@raspberrypi.local`.
It also asks whether Virgil should be the default ALSA device; say yes and
REAPER (Audio system **ALSA**, device **default**) and other ALSA programs use
Virgil without further setup. Desktop sound through PipeWire is not affected.
Change it later with `--default-device yes` or `--default-device no`.
Running it again upgrades Virgil and keeps your settings. A Pi has no
hardware timestamping, so expect a little more clock jitter than on a
desktop; raise the latencies if you hear dropouts.

## Virgil Control

**Windows:** Virgil Control is a native app that lives in the notification
area (system tray) and starts with Windows. Hover the icon for the clock
status; click it for the window with level meters for every channel to and
from the network (input on top, output below), the clock state and the apps
that are playing. Right-click
for *Settings…*, *Restart audio engine*, *Open log*, *Start with Windows*
and *Quit*. Closing the window keeps the tray icon; Virgil itself (the
service) runs either way. It warns with a notification when the service
stops or the Dante clock is lost.

**macOS and Linux:** Virgil Control opens the same controls in your browser
(http://127.0.0.1:8480/ while `virgild` runs). It shows:

- **Status:** clock state (locked, clock master or free-running) with its
  offset, format and latencies, and which apps are connected and playing.
- **Meters:** per-channel peak meters for what arrives from the network
  (what apps record) and, below it, what apps send to the network.
- **Settings:** device name, network interface, sample rate, channel counts,
  receive and transmit latency and clock source. An advanced editor gives you
  the raw configuration file.

Routing is done in Dante Controller (or any Dante routing tool).

**Apply** validates the configuration, saves it and restarts the audio
engine inside `virgild`. Apps stay connected when the sample rate and channel
counts are unchanged. If those change, ALSA apps reconnect by themselves
and ASIO hosts get a driver reset request. On macOS, restart Core Audio
after changing them.

The panel only listens on 127.0.0.1. It refuses requests addressed to other
host names (DNS rebinding) and cross-site form posts. Set `control_port = 0`
in `[device]` to turn it off.

Configuration file and log locations, if you prefer editing by hand:

| Platform | Configuration | Log |
|---|---|---|
| Windows | `C:\ProgramData\Virgil\virgil.conf` | `C:\ProgramData\Virgil\virgild.log` |
| macOS | `/Library/Application Support/Virgil/virgil.conf` | `/Library/Logs/Virgil/virgild.log` |
| Linux | `/etc/virgil/virgil.conf` | `journalctl -u virgild` |

Upgrades keep your configuration. Uninstalling also keeps it:

- **Windows:** use Add/Remove Programs.
- **macOS:** run `sudo "/Library/Application Support/Virgil/uninstall.sh"`. Add `--purge` to delete the configuration too.
- **Linux:** use `apt remove virgil` or `dnf remove virgil`. `apt purge` deletes the configuration.

With no interface set, the service uses the first active network interface. It
waits for the network if the service starts at boot before the network is up.

## Building

Requirements: CMake ≥ 3.16, a C++17 compiler and a Rust toolchain (stable,
via [rustup](https://rustup.rs)); Cargo builds the vendored Inferno. Linux
also needs `libasound2-dev`.

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

* **ASIO:** download the Steinberg ASIO SDK yourself (it is not
  redistributed). Then add `-DASIO_SDK_DIR=C:/path/to/asiosdk`.
* **Cross-compiling Windows from Linux:** `rustup target add
  x86_64-pc-windows-gnu`, then
  `cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DASIO_SDK_DIR=…`
* **Cross-compiling for Raspberry Pi / arm64 Linux:** run
  `.github/setup-arm64-cross.sh` once on an x86-64 Ubuntu host, then
  `cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake`.
  `VIRGIL_ARCH=arm64 packaging/linux/build-packages.sh deb` (or `portable`)
  builds the release files.
* **macOS universal build:** `rustup target add aarch64-apple-darwin
  x86_64-apple-darwin` and configure with
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.

### Building the installers and portable archives

| Output | Command | Needs |
|---|---|---|
| `.deb` + `.rpm` | `packaging/linux/build-packages.sh` | `dpkg-dev`, `rpm` |
| Linux tarball | `packaging/linux/build-packages.sh portable` | — |
| Raspberry Pi `.deb` + tarball | `VIRGIL_ARCH=arm64 packaging/linux/build-packages.sh deb` (and `portable`) | see cross-compiling above |
| Raspberry Pi one-file installer | `packaging/raspberry-pi/build-installer.sh dist/virgil_<ver>_arm64.deb` | the `.deb` above |
| Windows installer + zip | `ASIO_SDK_DIR=… packaging/windows/build-installer.sh` | `g++-mingw-w64-x86-64`, `nsis`, `zip` (runs on Linux) |
| macOS `.pkg` + zip | `packaging/macos/build-pkg.sh` | Xcode command-line tools |

Output goes to `dist/`. The **Release** GitHub workflow builds them and
publishes a GitHub Release, with `SHA256SUMS` and the notes from
`packaging/release-notes.md`, when you push a tag such as `v0.2.0` or run it by
hand with a version. Run by hand, its **platforms** choice (all, windows,
macos, linux, raspberry-pi) builds and releases only that platform, for a fix
that only concerns one; the release notes then say so. The **CI** workflow
builds and tests every platform on each push.

* **macOS signing and notarisation:** set `VIRGIL_CODESIGN_ID`,
  `VIRGIL_INSTALLER_ID` and `VIRGIL_NOTARY_PROFILE`, or the matching repository
  secrets in CI. Unsigned packages still install, but Gatekeeper asks the
  user to approve them in System Settings → Privacy & Security.
* **Windows signing:** the installer is unsigned, so SmartScreen warns on
  first run. Sign it with `signtool` if you have a certificate.

## Running

```sh
virgild --status                     # clock state, counters, connected apps
virgild -c my.conf -v                # run in the foreground with debug output
```

Configuration reference: [`config/virgil.conf.example`](config/virgil.conf.example).

* **Linux:** use `-D virgil` (automatic format conversion) or `-D virgil_hw`
  (raw), e.g. `jackd -d alsa -d virgil_hw -p 64`.
* **REAPER on Linux / Raspberry Pi:** Options → Preferences → Audio → Device:
  Audio system **ALSA**, input and output device **virgil_hw** (or
  **default** if the Pi installer made Virgil the default), sample rate
  48000, block size 128, 2–3 periods, 8 in / 8 out.

## Status and testing

| Component | How it was verified |
|---|---|
| Dante (Inferno) path | `tests/e2e_netns.sh`: two `virgild` instances in separate network namespaces, one as clock master. The [netaudio](https://pypi.org/project/netaudio/) Dante CLI, standing in for Dante Controller, discovers both and subscribes B's receive channels to A's transmit channels. A 1 kHz tone played into A arrives at B sample-accurately, 168 frames (3.5 ms) later, with no dropouts. A probe asks for the clock status the way Dante Controller does. In use on Windows 11 with Dante Controller, a Behringer WING (clock leader) and an AVIO USB adapter |
| PTPv1 clock | Unit tests: message encoding, master/follower lock, servo in simulation. Across namespaces the follower locks within about 10 µs |
| Engine, config | Unit tests; loopback through the Dante rings is bit-exact (24-bit) |
| ALSA driver | Real `aplay`/`arecord` through the daemon, 64-frame periods |
| Linux packages | `.deb` installed in a container; portable tarball used as a user would |
| Windows installer | In use on Windows 11; also run silently under Wine in development |
| ASIO driver | In use with REAPER on Windows 11; test host under Wine |
| Raspberry Pi (arm64) | Cross-built; unit and loopback tests run under QEMU in CI. **Not yet run on a real Pi** |
| CoreAudio driver, macOS `.pkg` | Built by CI on macOS; **not yet run on a real Mac** |

Known limitations:

* Timestamps are software-only (kernel receive timestamps on Linux). Expect
  tens of µs of clock jitter. That is fine for audio, but Dante Controller
  may show the clock as less stable than hardware.
* Dante features Inferno does not implement are missing: AES67 mode, Dante
  Domain Manager, device locking, encrypted control, and changing latency
  or sample rate from Dante Controller (set them in Virgil instead).
* Sample rate and channel counts come from `virgil.conf`. Changing them
  restarts the audio engine; on macOS also restart `coreaudiod`.
* The Windows service creates a `Global\` shared-memory section that every
  user session can reach. A `virgild` started by hand from a non-elevated
  console falls back to `Local\`, which only that session can reach.
* On macOS, the HAL plug-in may run inside coreaudiod's sandbox, and that may
  block opening `virgild`'s POSIX shared memory. This has not been tested.
