**0.2.2:** the Windows installer can now update the ASIO driver while a DAW has it loaded. Previously it failed with "Error opening file for writing".

**0.2.1:** fixes the ASIO driver on Windows. DAWs such as REAPER showed Virgil with no inputs or outputs ("device closed"), because the driver asked Windows for more access to the shared soundcard than ordinary, non-administrator apps get.

**Virgil** (formerly DSV) is now a native Dante device. It appears in Dante Controller with its own transmit and receive channels, and you route it there like any other Dante device. AES67 mode is no longer needed or used. Dante support comes from [Inferno](https://github.com/teodly/inferno), an independent open-source implementation of the protocol.

## Downloads

| You have | Download | Then |
|---|---|---|
| **Windows 10/11** | `Virgil-*-win64-setup.exe` | Run it. Open **Virgil Control** from the Start menu or desktop and pick your Dante network interface. Choose *Virgil* as the ASIO device in your DAW. |
| Windows, no install | `Virgil-*-windows-x64-portable.zip` | Unzip. Run `register-asio-driver.cmd` as administrator, then double-click `virgil-control.exe`. |
| **macOS 11+** | `Virgil-*-macos.pkg` | Run it. Open **Virgil Control** from Applications. Pick *Virgil Virtual Soundcard* in System Settings › Sound. |
| macOS, no install | `Virgil-*-macos-portable.zip` | Unzip, then follow `START-HERE.txt`. |
| **Ubuntu / Debian** | `virgil_*_amd64.deb` | `sudo apt install ./virgil_*_amd64.deb`, then run `virgil-control`. |
| **Fedora / RHEL** | `virgil-*.x86_64.rpm` | `sudo dnf install ./virgil-*.x86_64.rpm`, then run `virgil-control`. |
| Linux, no install | `Virgil-*-linux-x86_64.tar.gz` | Extract, `./setup-alsa.sh`, then `./virgil-control`. |

Then, in **Dante Controller**, subscribe Virgil's receive channels to your Dante transmitters (and other devices to Virgil's transmit channels).

**Upgrading from DSV 0.1:** Virgil installs alongside DSV under new names. Uninstall DSV first, because both use the PTP ports. Old `dsv.conf` files are not migrated; the settings that still apply are name, interface, sample rate, channels and latency.

**Unofficial:** Virgil is not affiliated with, authorized or approved by Audinate. It has been tested between Virgil devices with an open-source Dante controller, **but not yet against Audinate hardware or Dante Controller itself**. The installers are unsigned, so Windows SmartScreen and macOS Gatekeeper will ask you to confirm the first time you run them. Licensed under the GPLv3.
