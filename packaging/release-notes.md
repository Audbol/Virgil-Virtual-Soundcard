**Virgil** (**V**irtual **I**nterface **R**outing **G**ateway for **I**nferno-based **L**ow-latency audio) is a free, open-source virtual soundcard for Dante® networks. It appears as a device that Dante-compatible routing software can route, and your audio apps see an ordinary soundcard (ASIO, Core Audio, ALSA). Network compatibility comes from [Inferno](https://github.com/teodly/inferno), an independent open-source project. See [CHANGELOG.md](CHANGELOG.md) for what changed.

## Downloads

| You have | Download | Then |
|---|---|---|
| **Windows 10/11** | `Virgil-*-win64-setup.exe` | Run it. **Virgil Control** opens and stays in the notification area; pick your network interface in Settings. Choose *Virgil* as the ASIO device in your DAW. |
| Windows, no install | `Virgil-*-windows-x64-portable.zip` | Unzip. Run `register-asio-driver.cmd` as administrator, then double-click `virgil-control.exe`. |
| **macOS 11+** | `Virgil-*-macos.pkg` | Run it. Open **Virgil Control** from Applications. Pick *Virgil Virtual Soundcard* in System Settings › Sound. |
| macOS, no install | `Virgil-*-macos-portable.zip` | Unzip, then follow `START-HERE.txt`. |
| **Ubuntu / Debian** | `virgil_*_amd64.deb` | `sudo apt install ./virgil_*_amd64.deb`, then run `virgil-control`. |
| **Fedora / RHEL** | `virgil-*.x86_64.rpm` | `sudo dnf install ./virgil-*.x86_64.rpm`, then run `virgil-control`. |
| Linux, no install | `Virgil-*-linux-x86_64.tar.gz` | Extract, `./setup-alsa.sh`, then `./virgil-control`. |

Then, in your routing software, subscribe Virgil's receive channels to your transmitters (and other devices to Virgil's transmit channels).

**Independent project, no warranty.** Virgil is not affiliated with, authorized, endorsed or certified by Audinate. It contains no Audinate software, firmware or documentation. The installers are unsigned, so Windows SmartScreen and macOS Gatekeeper will ask you to confirm the first time you run them. Licensed under the GPLv3; see `NOTICE.md` for trademarks and third-party licences.

Dante® is a registered trademark of Audinate Pty Ltd. ASIO is a trademark and software of Steinberg Media Technologies GmbH.
