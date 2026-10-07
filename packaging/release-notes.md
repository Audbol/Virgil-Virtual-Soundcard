## Downloads

| You have | Download | Then |
|---|---|---|
| **Windows 10/11** | `DSV-*-win64-setup.exe` | Run it. Open **DSV Control** from the Start menu or desktop. Pick *DSV Virtual Soundcard* as the ASIO device in your DAW. |
| Windows, no install | `DSV-*-windows-x64-portable.zip` | Unzip. Run `register-asio-driver.cmd` as administrator, then double-click `dsv-control.exe`. |
| **macOS 11+** | `DSV-*-macos.pkg` | Run it. Open **DSV Control** from Applications. Pick *DSV Virtual Soundcard* in System Settings › Sound. |
| macOS, no install | `DSV-*-macos-portable.zip` | Unzip, then follow `START-HERE.txt`. |
| **Ubuntu / Debian** | `dsv_*_amd64.deb` | `sudo apt install ./dsv_*_amd64.deb`, then run `dsv-control`. |
| **Fedora / RHEL** | `dsv-*.x86_64.rpm` | `sudo dnf install ./dsv-*.x86_64.rpm`, then run `dsv-control`. |
| Linux, no install | `DSV-*-linux-x86_64.tar.gz` | Extract, `./setup-alsa.sh`, then `./dsv-control`. |

**DSV Control** opens the control panel in your browser at http://127.0.0.1:8480. It shows:

- the clock state and level meters
- the AES67 streams on your network, with one-click subscribe
- the settings

**Dante:** DSV talks to Dante devices through Dante's **AES67 mode**. Enable it per device in Dante Controller › Device View › AES67 Config, then create a multicast AES67 flow on the device. DSV does not implement Audinate's proprietary native Dante protocol and is not affiliated with Audinate.

This is an early release. The installers are unsigned, so Windows SmartScreen and macOS Gatekeeper will ask you to confirm the first time you run them. The release has not yet been tested with real Dante hardware.
