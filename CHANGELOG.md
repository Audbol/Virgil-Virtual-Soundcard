# Changelog

## 0.4.5 (Raspberry Pi only)
- The Pi installer asks whether Virgil should be the default ALSA device, so REAPER (Audio system ALSA, device "default") and other ALSA programs use it straight away. `--default-device yes|no` sets it without asking; uninstalling Virgil removes it.

## 0.4.4
- Linux / Raspberry Pi: fixed "Dante device failed to start" when Virgil runs as a service. Inferno crashed looking for a home folder the service account does not have; the service now keeps its state (Dante subscriptions, device identity) in `/var/lib/virgil`, and Inferno falls back to a temporary folder instead of crashing.
- The engine error now gives the actual reason the Dante device did not start.
- Linux: Virgil Control no longer starts a second copy of Virgil next to the service when the panel is briefly unreachable (that copy would block the Dante ports).

## 0.4.3
- Raspberry Pi installer is now a plain text file: opened by mistake in a text editor (which is what double-clicking it does), it shows how to run it instead of a "not valid UTF-8" error.

## 0.4.2
- Raspberry Pi installer: the window no longer closes the moment it finishes or fails. It waits for Enter, says which step went wrong, and saves its output to `~/virgil-install.log`.

## 0.4.1
- One-step Raspberry Pi installer (`Virgil-<ver>-raspberry-pi-installer.sh`): installs Virgil, gives the device a unique Dante name, picks the wired network port, keeps the CPU at full speed for low latency and starts the service. `get-virgil.sh` fetches and runs it in one line once the repository is public.

## 0.4.0
- Releases for macOS, Linux (.deb, .rpm, tarball) and, new, **Raspberry Pi** and other 64-bit ARM Linux (.deb and tarball) alongside Windows.
- Dante Controller's clock status shows Virgil's sync state and clock leader (it was red with no details).
- Virgil Control: the meters for audio arriving from the network are now above the meters for audio sent to it, in the Windows app and in the browser panel.
- Linux packages are built to install on Debian 12 and Ubuntu 22.04 or newer.

## 0.3.4
- Fixed network output going silent for good while input kept working. After a long hiccup (or a moment where the engine was mid-update) the ASIO driver could stop feeding Virgil, or Virgil could mute the app, until the app reopened the driver. The driver now realigns itself, and the engine never mutes a connected app.
- After *Restart engine* or a settings change, connected apps realign to the new clock straight away.
- The log notes when an app stops sending audio and when it resumes.

## 0.3.3
- Fixed Virgil showing up in Dante Controller without details and missing from the routing page (0.3.2 had changed a protocol identifier field that Dante Controller checks).

## 0.3.2
- Legal and attribution clean-up for public release: `NOTICE.md` with trademark and licence notices (also installed with Virgil); wording no longer resembles other companies' product names.
- Virgil Control (Windows) has an *About Virgil* entry in the tray menu.

## 0.3.1
- Full name: **V**irtual **I**nterface **R**outing **G**ateway for **I**nferno-based **L**ow-latency audio.

## 0.3.0
- New native Virgil Control for Windows: notification-area icon that starts with Windows, a window with level meters for every channel, the clock state and the apps that are playing (ASIO hosts appear by name), a settings dialog, and notifications when the service stops or the network clock is lost.

## 0.2.8
- The log file is written immediately (Windows held the last lines in a buffer).
- A crash inside the network stack is logged and the audio engine restarts automatically.

## 0.2.7
- Fixed transmit getting stuck after Virgil locks to the network clock: receivers got only the first channel, new subscriptions were refused and restarting the engine hung.

## 0.2.6
- Outgoing audio is timestamped one transmit latency ahead, so receivers with very low latency settings no longer drop it.

## 0.2.5
- Logo and app icons.

## 0.2.4
- Automatic network interface choice skips virtual adapters (Hyper-V/WSL, VPN, ...) and prefers wired Ethernet.
- Clock: uses the least-delayed of every four sync messages, removing ±1 ms clock jumps on Windows.
- Network-stack messages are logged from a separate thread and repeats are collapsed.

## 0.2.3
- Windows: the service opts out of timer throttling; transmit runs in the "Pro Audio" scheduling class.
- A single late clock packet no longer steps the clock; the log names every clock source heard.

## 0.2.2
- The Windows installer can update the ASIO driver while a DAW has it loaded.

## 0.2.1
- Fixed the ASIO driver showing no channels ("device closed") for non-administrator apps.

## 0.2.0
- Renamed from DSV to Virgil. Virgil now joins Dante networks directly through Inferno instead of using AES67; licensed under the GPLv3.

## 0.1.0
- First release (as DSV): AES67 virtual soundcard with ASIO, Core Audio and ALSA drivers, installers and a web control panel.
