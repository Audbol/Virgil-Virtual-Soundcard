## Downloads

| You have | Download | Then |
|---|---|---|
| **Windows 10/11** | `Virgil-*-win64-setup.exe` | Run it. Open **Virgil Control** from the Start menu or desktop. Pick *Virgil Virtual Soundcard* as the ASIO device in your DAW. |
| Windows, no install | `Virgil-*-windows-x64-portable.zip` | Unzip. Run `register-asio-driver.cmd` as administrator, then double-click `virgil-control.exe`. |
| **macOS 11+** | `Virgil-*-macos.pkg` | Run it. Open **Virgil Control** from Applications. Pick *Virgil Virtual Soundcard* in System Settings › Sound. |
| macOS, no install | `Virgil-*-macos-portable.zip` | Unzip, then follow `START-HERE.txt`. |
| **Ubuntu / Debian** | `virgil_*_amd64.deb` | `sudo apt install ./virgil_*_amd64.deb`, then run `virgil-control`. |
| **Fedora / RHEL** | `virgil-*.x86_64.rpm` | `sudo dnf install ./virgil-*.x86_64.rpm`, then run `virgil-control`. |
| Linux, no install | `Virgil-*-linux-x86_64.tar.gz` | Extract, `./setup-alsa.sh`, then `./virgil-control`. |

**Virgil Control** opens the control panel in your browser at http://127.0.0.1:8480. It shows:

- the clock state and level meters
- the AES67 streams on your network, with one-click subscribe
- the settings

**Dante:** Virgil talks to Dante devices through Dante's **AES67 mode**. Enable it per device in Dante Controller › Device View › AES67 Config, then create a multicast AES67 flow on the device. Virgil does not implement Audinate's proprietary native Dante protocol and is not affiliated with Audinate.

This is an early release. The installers are unsigned, so Windows SmartScreen and macOS Gatekeeper will ask you to confirm the first time you run them. The release has not yet been tested with real Dante hardware.
