# DSV: a low-latency virtual network soundcard

DSV makes a computer's audio apps send and receive multichannel audio over the
network as **AES67** streams. AES67 is the interoperability standard that Dante
devices support in their **AES67 mode**. Apps see an ordinary soundcard on every
platform:

| OS      | Driver                         | Apps see it as                   |
|---------|--------------------------------|----------------------------------|
| Linux   | ALSA external PCM plugin       | `dsv` ALSA device (JACK, PipeWire, aplay, …) |
| macOS   | CoreAudio AudioServerPlugIn    | "DSV Virtual Soundcard" system device |
| Windows | ASIO driver (COM in-proc)      | "DSV Virtual Soundcard" ASIO device |

## About Dante

**DSV does not implement Audinate's native Dante protocol.** Native Dante
(its device discovery, routing and audio transport) is proprietary. The only
legitimate way to use it is Audinate's licensed SDKs. DSV is not affiliated
with Audinate.

DSV talks to Dante gear through **Dante's AES67 mode**:

1. In Dante Controller, open *Device View → AES67 Config* on a Dante device and
   enable AES67 mode, then reboot the device.
2. **Dante → DSV:** create a multicast transmit flow on the Dante device and tick
   "AES67". DSV discovers it via SAP. Run `dsvd --discover` to list it, then
   subscribe in `dsv.conf` with `sap_name = …`.
3. **DSV → Dante:** DSV announces its transmit flows via SAP. They appear in
   Dante Controller's routing grid as an AES67 device that Dante receivers can
   subscribe to.
4. Clocking: Dante devices in AES67 mode bridge their clock onto PTPv2
   domain 0, which DSV follows. DSV only acts as PTPv2 grandmaster if it hears
   no other master.

The usual Dante AES67 limits apply: 48 kHz, multicast, 1 ms packet time and up
to 8 channels per flow.

## Architecture

```
 apps ─► ALSA plugin ─┐                              ┌─► AES67 RTP multicast ─► network
 apps ─► CoreAudio  ──┼─► shared-memory soundcard ◄──┤        (L24, 125 µs…4 ms packets)
 apps ─► ASIO driver ─┘   (lock-free, per-frame      └─◄ AES67 RTP ◄─ Dante / AES67 devices
                           addressed by media clock)
                                    ▲
                               dsvd daemon ── PTPv2 slave/master, SAP announce/discover
```

* **One clock.** `dsvd` steers a media clock to the PTP grandmaster with a PI
  servo and publishes a `(host time, media frame, rate)` anchor in shared
  memory. Every driver runs its callbacks from that anchor: CoreAudio zero
  timestamps, ASIO buffer switches and the ALSA hw pointer. Apps therefore run
  in lock-step with the network. There is no resampling and no drift
  correction anywhere.
* **Rings indexed by absolute media frame.** Frame *t* lives in slot
  `t mod ring`. A received RTP packet is written at the slot of its own
  timestamp. That makes the ring a free jitter buffer that tolerates
  reordering. Playback from up to 8 clients is summed by the daemon, so several
  apps can play at once without a lock.
* **Real-time path.** The daemon tick and receive threads use SCHED_FIFO
  (Linux), time-constraint policy (macOS) or MMCSS "Pro Audio" (Windows). Each
  tick wakes on an absolute deadline with a short final spin. Memory is
  `mlock`ed, nothing allocates after start-up, and audio threads never lock.
  Packets are tagged DSCP EF/AF41 as AES67 recommends.

### Latency

| Path     | Latency                                                        |
|----------|----------------------------------------------------------------|
| Capture  | `latency_us` (network jitter buffer) + driver buffer            |
| Playback | `tx_lead_us` + one packet + driver buffer                       |

Example: `packet_time_us = 250` with `latency_us = 1000`, `tx_lead_us = 500`
and a 32-frame ASIO buffer gives about 1.7 ms in and 1.4 ms out at 48 kHz. All
drivers report these numbers to the host (`getLatencies`, CoreAudio safety
offsets, ALSA `delay`), so DAW latency compensation stays exact.

## Installing

Download the installer for your platform from the GitHub Releases page.

| Platform | Installer | What it sets up |
|---|---|---|
| Windows 10/11 x64 | `DSV-<ver>-win64-setup.exe` | `dsvd` as an auto-start Windows service with restart-on-failure, the ASIO driver (registered), a firewall rule for the network streams, Start-menu entries and an Add/Remove Programs entry |
| macOS 11+ (Apple silicon & Intel) | `DSV-<ver>-macos.pkg` | Core Audio device `DSVAudio.driver`, `dsvd` as a launch daemon, `/usr/local/bin/dsvd` |
| Debian / Ubuntu | `dsv_<ver>_amd64.deb` | `dsvd` systemd service (enabled and started), ALSA plugin with `dsv` / `dsv_hw` devices |
| Fedora / RHEL / openSUSE | `dsv-<ver>-1.x86_64.rpm` | same as the `.deb` |

After installing, set the network interface and flows in the configuration
file, then restart the service:

| Platform | Configuration | Restart | Log |
|---|---|---|---|
| Windows | `C:\ProgramData\DSV\dsv.conf` (Start menu → DSV → Edit configuration) | Start menu → DSV → Restart DSV service | `C:\ProgramData\DSV\dsvd.log` |
| macOS | `/Library/Application Support/DSV/dsv.conf` | `sudo launchctl kickstart -k system/org.dsv.dsvd` | `/Library/Logs/DSV/dsvd.log` |
| Linux | `/etc/dsv/dsv.conf` | `sudo systemctl restart dsvd` | `journalctl -u dsvd` |

Upgrades keep your configuration. Uninstalling also keeps it:

- **Windows:** use Add/Remove Programs.
- **macOS:** run `sudo "/Library/Application Support/DSV/uninstall.sh"`. Add `--purge` to delete the configuration too.
- **Linux:** use `apt remove dsv` or `dnf remove dsv`. `apt purge` deletes the configuration.

With no interface set, the service uses the first active network interface. It
waits for the network if the service starts at boot before the network is up.

## Building

Requirements: CMake ≥ 3.16 and a C++17 compiler. Linux also needs
`libasound2-dev`.

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

* **ASIO:** download the Steinberg ASIO SDK yourself (it is not
  redistributed). Then add `-DASIO_SDK_DIR=C:/path/to/asiosdk`.
* **Cross-compiling Windows from Linux:**
  `cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DASIO_SDK_DIR=…`

### Building the installers

| Installer | Command | Needs |
|---|---|---|
| `.deb` + `.rpm` | `packaging/linux/build-packages.sh` | `dpkg-dev`, `rpm` |
| Windows `.exe` | `ASIO_SDK_DIR=… packaging/windows/build-installer.sh` | `g++-mingw-w64-x86-64`, `nsis` (runs on Linux) |
| macOS `.pkg` | `packaging/macos/build-pkg.sh` | Xcode command-line tools |

The output goes to `dist/`. The `Installers` GitHub workflow builds all of
them, and attaches them to a release when you push a tag such as `v0.1.0`.

* **macOS signing and notarisation:** set `DSV_CODESIGN_ID`,
  `DSV_INSTALLER_ID` and `DSV_NOTARY_PROFILE`, or the matching repository
  secrets in CI. Unsigned packages still install, but Gatekeeper asks the
  user to approve them in System Settings → Privacy & Security.
* **Windows signing:** the installer is unsigned, so SmartScreen warns on
  first run. Sign it with `signtool` if you have a certificate.

## Running

```sh
dsvd --status                     # clock state, packet counters, connected apps
dsvd --discover -i eth0           # list AES67 / Dante AES67 flows on the network
dsvd -c my.conf -v                # run in the foreground with debug output
```

Configuration reference: [`config/dsv.conf.example`](config/dsv.conf.example).

* **Linux:** use `-D dsv` (automatic format conversion) or `-D dsv_hw`
  (raw), e.g. `jackd -d alsa -d dsv_hw -p 64`.

## Status and testing

| Component | How it was verified |
|---|---|
| AES67 engine, RTP, SDP, SAP, config | Unit tests; end-to-end UDP loopback is sample-accurate (error ≤ 6e-8, i.e. 24-bit quantisation) on Linux and on Windows (Wine) |
| ALSA driver | Real `aplay`/`arecord` through the daemon in loopback: 3.000 s tone, 0 discontinuities with 64-frame periods |
| Linux `.deb` | Installed with `dpkg` in a container. The system ALSA config lists the `dsv` devices and a 3 s tone passes through. Config edits survive reinstall; `purge` removes everything |
| Windows installer | Run silently under Wine. Checked: files, ASIO registration, uninstall entry, service creation and auto-start, a clean service stop, config kept on upgrade, and removal on uninstall. Wine's service handling is unreliable, so **this needs a run on real Windows** |
| macOS `.pkg` | **Not yet built.** The scripts are syntax-checked only. The `Installers` workflow builds it on macOS |
| ASIO driver | Built against a stand-in for the documented SDK interface. A test host (`tests/asio_host_test.cpp`) under Wine got exactly 750 buffer switches in 2 s and a glitch-free round trip. Not yet run in a real DAW or against the real SDK headers |
| CoreAudio driver | **Not yet compiled on macOS.** Only syntax-checked against stub headers. CI builds it on macOS; it needs a real Mac to validate |
| PTPv2 slave/master | Servo checked in simulation (80 ppm drift, 20 µs jitter → within 30 µs). **Not yet tested against real Dante/AES67 hardware** |
| Dante AES67 interop | Parses real Dante AES67 SDP. **Not yet tested with Dante devices** |

Known limitations:

* Timestamps are software-only (kernel receive timestamps on Linux). Expect
  tens of µs of PTP jitter, which is enough for audio but not for SMPTE 2110
  strictness.
* The sample rate and channel count come from `dsv.conf`. Changing them means
  restarting `dsvd`; on macOS also restart `coreaudiod`.
* The Windows service creates a `Global\` shared-memory section that every
  user session can reach. A `dsvd` started by hand from a non-elevated
  console falls back to `Local\`, which only that session can reach.
* On macOS, the HAL plug-in may run inside coreaudiod's sandbox, and that may
  block opening `dsvd`'s POSIX shared memory. This has not been tested.
