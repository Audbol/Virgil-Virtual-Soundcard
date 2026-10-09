**Virgil** (**V**irtual **I**nterface **R**outing **G**ateway for **I**nferno-based **L**ow-latency audio) is a free, open-source virtual soundcard for Dante® networks. It appears as a device that Dante-compatible routing software can route, and your audio apps see an ordinary soundcard (ASIO, Core Audio, ALSA). Network compatibility comes from [Inferno](https://github.com/teodly/inferno), an independent open-source project. See [CHANGELOG.md](CHANGELOG.md) for what changed.

## Downloads

| You have | Download | Then |
|---|---|---|
| **Windows 10/11** | `Virgil-*-win64-setup.exe` | Run it. **Virgil Control** opens and stays in the notification area; pick your network interface in Settings. Choose *Virgil* as the ASIO device in your DAW. |
| Windows, no install | `Virgil-*-windows-x64-portable.zip` | Unzip. Run `register-asio-driver.cmd` as administrator, then double-click `virgil-control.exe`. |
| **macOS 11+** (Apple silicon or Intel) | `Virgil-*-macos.pkg` | Open it and follow the installer. Pick *Virgil Virtual Soundcard* in Audio MIDI Setup or your app. |
| macOS, no install | `Virgil-*-macos-portable.zip` | Unzip, run `install-driver.command`, then open **Virgil Control**. |
| **Debian 12+ / Ubuntu 22.04+** | `virgil_*_amd64.deb` | `sudo apt install ./virgil_*_amd64.deb`, then open **Virgil Control** from the applications menu (it also sits in the tray). Apps use the ALSA device `virgil`. |
| **Fedora / RHEL / openSUSE** | `virgil-*.x86_64.rpm` | `sudo dnf install ./virgil-*.x86_64.rpm` (or `zypper install`). |
| Linux, no install | `Virgil-*-linux-x86_64.tar.gz` | Unpack, run `./setup-alsa.sh`, then `./virgil-control`. See `START-HERE.txt`. |
| **Raspberry Pi** (64-bit Raspberry Pi OS 12+) | `Virgil-*-raspberry-pi-installer.sh` | Download it on the Pi, open **Terminal** (don't double-click the file; that opens it in a text editor) and run `bash ~/Downloads/Virgil-*-raspberry-pi-installer.sh`. It installs Virgil, names the device `Virgil-<hostname>`, picks the wired network port, sets the CPU for low latency and starts the service, then tells you how to open the control panel. Use wired Ethernet. |
| Raspberry Pi, package only | `virgil_*_arm64.deb` | `sudo apt install ./virgil_*_arm64.deb` (no automatic setup). |
| Raspberry Pi / arm64, no install | `Virgil-*-linux-arm64.tar.gz` | As for the Linux tarball. |

Windows and the Raspberry Pi are in daily use. The macOS build (driver and Virgil Control) passes its automated tests but has not yet been run on a real Mac, so reports are welcome.

Then, in your routing software, subscribe Virgil's receive channels to your transmitters (and other devices to Virgil's transmit channels).

**Independent project, no warranty.** Virgil is not affiliated with, authorized, endorsed or certified by Audinate. It contains no Audinate software, firmware or documentation. The installers are unsigned, so Windows SmartScreen and macOS Gatekeeper will ask you to confirm the first time you run them. Licensed under the GPLv3; see `NOTICE.md` for trademarks and third-party licences.

Dante® is a registered trademark of Audinate Pty Ltd. ASIO is a trademark and software of Steinberg Media Technologies GmbH.
