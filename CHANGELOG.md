# Changelog

## Unreleased
- **Suggested transmit latency.** Virgil records how late its Dante sending thread wakes up while audio goes out, and after 30 minutes of sending suggests the transmit latency this computer needs: the send delay + the latest wakeup seen + 1 ms of margin, rounded up to 0.5 ms (at least the minimum, at most 40 ms). Settings in every Virgil Control app and the web panel show it next to *Transmit latency* (*can be lowered to 3 ms*, *should be raised to 6 ms (2 late sends)*, or *the transmit latency of 4 ms is right*), with *Apply suggestion*, which fills in the field; before that, the progress (*measuring, 12 of 30 min*). It never suggests keeping or lowering a setting that dropped audio, and only suggests going down by 0.5 ms or more. *Dropouts and diagnostics* lists a suggestion to lower or raise among its hints (the hint about dropped sends names the same value), the report has a line with the figures, and `/api/diagnostics` has `tx_latency_suggestion`. Only time with a Dante receiver subscribed counts; seconds with a clock step are left out. The observation survives settings changes (including the transmit latency itself) and starts over with *Clear*, a new mixing tick or a service restart. Inferno counts the wakeups that saw a running flow (`running_wakeups`) for it.
- **0.5 ms mixing tick.** Settings (all Virgil Control apps and the web panel) gain *Mixing tick*: *1 ms (default)* or *0.5 ms (lower latency, more CPU)* (`tick_us`). At 0.5 ms the send delay is 1.5 ms and the transmit latency can go down to 2.5 ms; the latency presets and the minimum shown follow the tick, and the apps check it before saving. It roughly doubles the engine's CPU use (about 3 % → 7 % of one core at 8×8 and 5 % → 9 % at 64×64 on a 2.1 GHz VM) and halves the drivers' default playback margin (two ticks; `tx_lead_us` sets it). Hand-edited ticks from 125 to 4000 us keep working and are listed as such. The loopback and desktop audio tests run at both ticks.
- macOS: the audio threads' time-constraint policy keeps its computation at 100 us or more, so ticks shorter than 0.5 ms (hand-edited) no longer fail to get real-time scheduling.
- Windows: with a tick under 1 ms on a system without high-resolution timers (before Windows 10 1803) the log says the engine will wake late.
- The "Audio engine started" event names the mixing tick when it is not 1 ms.
- **Audio interface inputs to Dante, and Dante to its outputs** (desktop audio in Virgil Control). A new source type, *Audio interface inputs*, sends inputs of an audio interface (or any recording device) to Virgil's transmit channels one to one, with low latency: inputs *first*.. → transmit channels *channel*.., up to 64 (`Scarlett 18i20 inputs 1-8 → TX 1-8`), with gain, mute, a level meter and status (`Capturing 8 inputs (48 kHz, clock +12 ppm)`). The output can likewise be *Audio interface outputs*: receive channels *channel*.. → the interface's outputs *first output*.., the others silent (`Playing 8 channels on …`). The window shows each device's input / output count and says when a choice does not fit (`The device has 8 inputs: choose others`); a device that is unplugged is picked up again when it comes back, and *Default* follows the system's default input. The resampler bridges for interfaces keep only a few milliseconds (about twice the device's period plus one 5 ms block) instead of 40–50 ms. Linux: any PulseAudio / PipeWire source that is not a monitor, all of its channels by index, 5 ms fragments; about 23 ms from input to Virgil's transmit time in the test, about 20 ms from Dante to an interface output plus Virgil's receive latency (with PipeWire use the interface's *Pro Audio* profile to see every channel). macOS: any macOS version, read directly with an IOProc (128-frame buffer asked for), interleaved or not; asks once for the microphone permission (it covers every audio input) and says so if it was refused. Windows: the interface's ASIO driver or WASAPI.
- Desktop audio engine: lanes, bridges, mixing and peaks work with any channel count (up to 64) for interfaces; the backend interface gains `inputs()`, `default_input()`, `open_input_capture()`, a first channel for `open_render()`, `Device::channels` and `Stream::period_s()`.

## 0.6.4
Includes everything in 0.6.3 below (0.6.3 was a test build only).
- **No more browser in Virgil Control.** *Dropouts and diagnostics*, *Channel names* and *Edit configuration…* open as windows of the app on Windows, macOS and Linux: the last hour's dropouts per minute as a chart (with details on hover), hints, the event list, *Save report…* and *Clear*; channel names checked as you type (up to 128 each way); the configuration file with *Apply & restart*. The *Open control panel in browser* menu item is gone. The web control panel itself stays, for other computers (see below) and for this one at http://127.0.0.1:8480/.
- Settings in Virgil Control gain *Allow remote access* and its password, and show the address to open once it is on.
- Linux: installing the `.deb` or `.rpm` (and the Raspberry Pi installer) no longer seems to hang. The service starts in the background instead of the package manager waiting until the network counts as fully up, which could take minutes or never happen without a connection (seen on Fedora).
- **Remote access to the web control panel** (off by default). With *Allow remote access* and a password (8+ characters) in the panel's Settings, other computers can open `http://<Virgil computer>:8480/` and log in with that password. Five wrong passwords lock that address out for 10 s. Other computers never see the password and cannot change the remote access settings. Plain HTTP, so use it on a trusted network.
- **Desktop audio on Linux.** Virgil Control for Linux gets the *Desktop audio…* window of the Windows app (window footer and tray menu, or `virgil-control --desktop-audio`): sound from the desktop to Virgil's Dante transmit channels, and Dante receive channels to an output device, with no driver. Sources: everything an output device plays (its monitor; *Default* follows the sound settings), one app with all its streams (by name, e.g. *Firefox*, picked up as its streams come and go), or all apps except Virgil Control. Gain, mute, mono, level meters, status, the feedback warning, one *Desktop audio* playback slot and the drift-following resampler work as on Windows; about 60 ms of latency per direction. Works with PulseAudio and PipeWire (`pipewire-pulse`); the library is loaded at run time, so the packages get no new dependency. Settings are in `~/.config/virgil/desktop-audio.conf`.
- **Desktop audio on macOS.** Virgil Control for macOS gets the same *Desktop audio…* window (window footer and menu bar menu, or `Virgil Control --desktop-audio`), with no driver and no virtual cable: everything an output device plays (*Default* follows System Settings › Sound), one app with its helper processes (e.g. *Safari*, *Spotify*, picked up as its processes come and go), or all apps except Virgil Control and except apps that play on Virgil's own device; and Dante receive channels to an output device. Capturing uses Core Audio process taps and needs macOS 14.2 or later (older macOS: receive channels to an output device only, and the window says so); the first capture asks for permission to record this computer's audio (*Screen & System Audio Recording* in System Settings › Privacy & Security), and a source that stays silent while its app plays points there. Captured audio keeps playing as usual. Settings are in `~/Library/Application Support/Virgil/desktop-audio.conf`.
- The desktop audio engine is now shared code (`tools/control_common/desktop_audio_engine.*`) with the sound system behind a small backend interface (PulseAudio / PipeWire on Linux, Core Audio on macOS). The Windows app keeps its own engine.

## 0.6.3
- **Up to 128 channels each way** (was 64), in the settings of every Virgil Control app and the control panel. Virgil now accepts up to 64 transmit and 64 receive flows (was 32), so 128 channels work with devices that send few channels per flow (an RME Digiface uses 4). The meters get narrower above 64 channels so the window stays under about 850 points wide. The drivers and the service must be the same version (the shared-memory layout changed); install both from this release. Each channel costs memory: 128×128 at the default latencies uses about 38 MB of shared memory.
- **Follows network address changes.** When the computer's address changes or goes away while Virgil runs (DHCP renewal, Wi-Fi joining another network or a phone hotspot, an adapter unplugged), Virgil restarts its audio engine on the new address, or waits until the network is back, within a few seconds. It used to stay on the old address, and every send failed with "Can't assign requested address" until it was restarted by hand. The diagnostics event log records it.
- Fixed a crash of the Dante engine when a device re-requested its flows while Virgil was removing timed-out ones (seen with an RME Digiface Dante on macOS): receiving stopped on some channels, and every later subscription change failed until Virgil was restarted.

## 0.6.2
Includes everything in 0.6.1 below (0.6.1 was not released on its own).
- Virgil Control (all platforms): *Dropouts and diagnostics…* moved from the window's footer into Settings (and stays in the tray / menu bar menu), so the footer is less crowded.
- **Desktop audio (Windows).** Virgil Control can send sound from Windows programs to Virgil's Dante transmit channels, and play Dante receive channels on a Windows output device, with no driver and no extra software (*Desktop audio…* in the window and the tray menu). Sources: everything an output device plays (WASAPI loopback, *Default* follows Windows), one app with the processes it starts (`chrome.exe`, Windows 10 version 2004 or later), or all apps except Virgil itself (no echo when you also listen to Dante on the PC). Each source goes to a stereo pair or, mixed to mono, one transmit channel, with gain, mute, a level meter and status. Audio is converted to Virgil's sample rate and follows the drift between the sound hardware's clock and the Dante clock with a new adaptive resampler (about 100 dB signal-to-noise, ±0.5 % range), roughly 60–80 ms of latency. It uses a playback slot named *Desktop audio*, pauses while Virgil is stopped and reconnects when the service restarts. Settings are per user in `%APPDATA%\Virgil\desktop-audio.conf`.
- Windows: `desktop-audio-probe.exe` (in the portable zip) lists output devices and apps with audio, and records 5 s from a device or an app to a WAV file, to check capture on a PC without Virgil.

## 0.6.1
- **More accurate Dante clock.** Virgil's own clock-delay measurements no longer favour the ones a busy computer delayed (they biased the clock behind the leader), the frequency is learnt from the leader alone, the servo settles faster, and "locked" is reported once the clock really is close (about 30 s, filtered, not single readings). In simulations of Windows, macOS, Linux and Raspberry Pi timing the error to the leader drops 2-5x (Windows about 47 to 18 us, Raspberry Pi 30 to 9 us). Linux uses kernel send timestamps; macOS and Windows use kernel receive timestamps where available.
- Drivers: apps get one more tick (1 ms) to deliver their audio before it misses the network. ALSA wakes apps in step with the media clock (fewer underruns: 74 instead of 371 in a loaded test), drains completely, and rides out a failed clock read; ASIO no longer forces the host to resynchronise after a long stall when the clock did not change.
- When Virgil hears no Dante clock leader for 15 s, Virgil Control (all platforms) and the control panel say so — "No Dante clock leader found on <adapter>", or that Dante devices were found but none leads, or that the leader was lost — with the likely fixes (another network adapter, a firewall blocking UDP 319/320, *Become clock leader*), and notify once. It used to show only "Free-running", and the reason was only in the log.
- Windows: the default transmit latency is 6 ms (was 4 ms): Windows wakes Virgil's sending thread a few milliseconds late now and then, which dropped audio at 4 ms. Existing configurations keep their setting.
- ASIO: when a host cannot open Virgil ("Error initializing ASIO driver"), the driver's error message says why: Virgil is stopped, the service is not running or not responding, or the service and driver are different versions.
- ALSA: a playback or capture buffer under 10 ms prints a hint once (raise the app's buffer if you hear dropouts); failures to open say why.
- macOS: no "mlockall failed" warning in the log (macOS does not support it).
- Clock: computers with noisy network timestamps (Raspberry Pi, busy PCs) report *locked* to the Dante clock leader within a minute or two instead of staying "free-running" while following it. The clock itself was already in sync; only the lock test was too strict.
- Dropout diagnostics: audio received while Virgil and the sender were not yet on the same clock (for example in the moments before the leader appears) is no longer counted as hours-late network audio.
- macOS: Virgil Control's notifications (service stopped, Dante clock lost) use the current notification system, so the banners also appear on recent macOS versions. Virgil Control asks once for permission; clicking a banner opens the window.
- macOS: with the macOS firewall on, the installer allows incoming connections for virgild (Dante clock and control traffic, which the firewall could block); with it off nothing changes. The uninstaller removes the entry.
- Dropout diagnostics: audio *to* the network that was dropped because Virgil's sending thread woke up later than the transmit latency (a busy computer) is now counted and explained, with a suggestion for the transmit latency that would have covered it. Starting a new flow no longer logs a misleading "media clock jumped" error, and the first start no longer warns that no saved state could be loaded.
- Receiving: audio from the network that arrives up to about a millisecond after its receive latency still reaches the apps. Virgil hands each millisecond of received audio to the apps twice, the second time with whatever arrived late, at no extra latency.
- Dropout diagnostics: *network-late* now means received audio that really was lost because it came too late ("2.0 ms of audio from the network arrived too late and was lost"), and only then suggests a higher receive latency. Packets older than the receive latency that still played are only counted in the report and JSON, as close to the limit. The report and JSON also show how late the sending thread woke up (worst lag, wake-ups over 0.5/1/2/4 ms) when that stayed within the transmit latency.
- Dropout diagnostics: a stall of the computer counts once, as an episode with its length ("The computer stalled ~45 ms: Virgil's audio engine and apps waited"), instead of once per millisecond, and the apps that waited with it are not blamed for missing their deadline. A stall counts when it actually made audio to the network late (later than the send delay minus one tick, 1.5 ms at the defaults; it was 2 ms). On Windows the hint mentions LatencyMon.
- A stall of the computer longer than 50 ms no longer looks like a clock jump: Virgil skips the audio it could not process and carries on, so ALSA apps do not all report an xrun, ASIO hosts do not reset and Core Audio does not resynchronize. Real clock steps still realign everything.
- Windows: virgild runs in the High priority class, and its audio threads (and the ASIO driver's) opt out of power throttling (EcoQoS), which could run them slowly on efficiency cores. Failures are logged once.

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
