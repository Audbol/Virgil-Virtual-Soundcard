# Changelog

## 0.6.0
- **Stop and start Virgil** from Virgil Control (Windows, macOS, Linux), the tray / menu bar menu and the browser panel. Stopped, Virgil leaves the network completely (no Dante device, no network ports in use) and stays stopped across restarts until you start it again; the service keeps running so the controls keep working.
- **Dropout diagnostics.** Virgil now notices and explains dropouts: audio from the network that arrived too late for the receive latency or never arrived, apps that did not deliver their audio in time, the computer keeping Virgil's engine waiting, and clock trouble. Virgil Control shows how many dropouts the last hour had; *Dropouts…* opens the event list with a chart of the last hour, suggestions (for example the receive latency that would have covered the late audio) and *Download report*, a text file with the setup, the events and the recent log for bug reports.
- **Channel names.** Name channels in Virgil's settings or in Dante Controller (both stay in step, and the names are saved in the configuration). ASIO and Core Audio apps show them as channel names.
- macOS: the Virgil device now has the channel count, sample rate and latency set in Virgil. It used to keep 8 channels when Core Audio started before the Virgil service (as at boot) until Core Audio was restarted, so apps such as Live Professor saw 8 of 32 channels. It also follows later changes by itself.
- Virgil as the clock leader (*Become clock leader when no Dante device provides one*, e.g. on a network of its own) is now synchronised and routable in Dante Controller. It used a made-up clock identity that Dante Controller could not match to any device; it now uses the network adapter's hardware address, as Dante devices do.
- **Dante Controller on the same computer** (and other Dante software there, such as Dante Virtual Soundcard's services) no longer leaves Virgil "not routable". That software listens on the standard Dante control ports, so its requests to Virgil were answered back into Virgil itself. Virgil now notices and uses control ports 24440–24443, which Dante Controller finds by itself (`ports = auto`, the default; `standard` or a port number to choose).
- With *Clock: Local clock only* selected, Virgil Control and the log say so (Dante devices do not sync with Virgil then), instead of a plain "Free-running".
- Fixes from a review of the whole code:
  - Malformed Dante control packets from the network could restart Virgil's Dante engine; they are now refused.
  - Clock: a new clock leader is followed without a long relock, the path delay follows drift, and Virgil no longer keeps claiming the leader role after a better leader appears.
  - Audio from an app that stops sending no longer repeats old audio half a second later; NaN samples from an app become silence instead of a full-scale click.
  - Settings: comments after a value in the configuration file are handled, duplicate keys no longer add up, and saving keeps the file's permissions; renames from Dante Controller with names that are not allowed are refused.
  - Engine shutdown can no longer hang when a channel is renamed at the same moment; a closed browser connection no longer stops the service on Linux and macOS.
  - More packets and announcements from other devices on the network that could crash or keep restarting the Dante engine are refused (bad multicast flow requests, very long names, unsupported channel counts or encodings, more than 32 receive flows).
  - After a clock change, ALSA, ASIO and Core Audio apps follow the new timeline straight away (an ALSA app could stall until the next change); apps opening and closing the soundcard often no longer silence another app.
  - ALSA: rewinding and fast-forwarding a stream work; a Virgil restarted with another channel count or sample rate is refused cleanly (reopen the device) instead of corrupting the stream. ASIO: hosts that stop or reset the driver from inside its callback no longer risk a crash. Core Audio: starting and stopping in quick succession no longer risks a crash, and channel names reach apps when Virgil starts after them.
  - Virgil Control (all platforms): the window no longer freezes while settings are saved or Virgil is stopped, started or restarted; a slow reply no longer shows "service not running" (or notifies); buttons and menu items are disabled while they would do nothing; Settings opens once; 0 channels in one direction can be set; a configured but unplugged network adapter is kept. Linux: closing the window without a tray icon no longer crashes. Windows: correct size on high-DPI monitors. macOS: notifications when the service stops or the Dante clock is lost.
  - Control panel: saving settings no longer undoes a Stop or a rename made in Dante Controller meanwhile; restarting shows *Restarting…* instead of a red error; latencies are in milliseconds and the *Become clock leader* option is available.
- The drivers and the service must be the same version (the shared-memory layout changed); the installers update both.
- Packaging:
  - Linux: upgrading the package no longer re-enables or starts a virgild service you disabled; a fresh install still enables and starts it. `/etc/virgil/virgil.conf` is no longer a package-managed configuration file (Virgil rewrites it itself, so dpkg asked about it on upgrades); it is created from `/usr/share/virgil/virgil.conf.default` only when missing, and upgrades keep yours.
  - Raspberry Pi installer: a device name followed by a comment in the configuration (`name = Stage Left # FOH`) is kept as it is instead of being rewritten, and the installer says when Virgil was left stopped (press *Start* in Virgil Control) instead of "running".
  - Windows and macOS: the service log (`virgild.log`) is rotated at 5 MB, keeping one older file (`virgild.log.1`).

## 0.5.2
- **Unique device names on every platform.** Every installation used to start out named "Virgil", so two computers running Virgil conflicted in Dante Controller. Virgil now replaces the stock name with Virgil-<computer name> when it starts (and saves it, so the control panel shows it); a name you chose yourself is kept. Devices subscribed to the old "Virgil" channels need re-subscribing to the new name.
- Includes the macOS fixes of 0.5.1 (Quit, single instance, app menu).

## 0.5.1 (macOS only)
- Virgil Control: **Quit** now always quits. Only one copy runs (opening the app while the login item is running brings up the existing window instead of adding a second menu bar icon). While the window is open the app has a Dock icon and a menu bar with Quit (⌘Q), Settings (⌘,) and Close (⌘W), and copy and paste work in the settings fields.
- Device name: a Mac still on the stock name "Virgil" is renamed Virgil-<computer name> by the installer, so it no longer conflicts in Dante Controller with other computers running Virgil. A name you chose yourself is kept.

## 0.5.0
- **Native Virgil Control on macOS (menu bar app) and Linux / Raspberry Pi (GTK app with tray icon)**, with the same window as on Windows: clock state, meters, playing apps, settings.
- **Fewer dropouts from clock corrections.** Once locked, Virgil no longer jumps its clock by ~1 ms (and back) when software timestamps or the path delay are briefly disturbed; a jump needs five agreeing measurements. The path delay is measured from the cleanest samples and changes slowly. On Linux (Raspberry Pi), kernel receive timestamps are converted without preemption errors.
- Dante Controller: sample rate and encoding are reported (Device Config), the software version shows Virgil's version, and the link speed is reported where the operating system does not give one.
- Robustness from a review of the whole code: values in shared memory are no longer trusted by the service or the drivers; settings that could hang or crash the engine are rejected; a second Virgil no longer takes over a running one's soundcard; on Windows a change of channel counts with a DAW open no longer hides the soundcard; settings are saved atomically; command-line options survive engine restarts.
- Drivers: ALSA streams recover from clock changes instead of stalling; stopping and restarting playback no longer replays old audio (ALSA, ASIO); ASIO hosts are no longer reset on every settings change; Core Audio reconnects when Virgil restarts.
- Installers: the Raspberry Pi installer upgrades cleanly over an edited configuration; macOS: the package always installs to the system folders and the uninstaller removes Virgil Control; Windows: the configuration stays editable after saving from Virgil Control; Linux: `apt purge` removes Virgil's state and service account.

## 0.4.6 (Raspberry Pi only)
- Fixed "cannot write /etc/virgil/virgil.conf" when saving settings from the control panel on Linux / Raspberry Pi. The service now runs as a `virgil` system account (created by the package) that owns `/etc/virgil`. The same fix reaches the regular Linux packages with the next full release.

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
