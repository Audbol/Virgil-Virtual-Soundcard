# Changelog

## 0.3.2
- Legal and attribution clean-up for public release: `NOTICE.md` with trademark and licence notices (also installed with Virgil); the device no longer announces a third-party vendor name on the network; wording no longer resembles other companies' product names.
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
