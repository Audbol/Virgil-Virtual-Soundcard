# Notices

## Independent project

Virgil (Virtual Interface Routing Gateway for Inferno-based Low-latency audio)
is an independent, non-commercial open-source project. It is **not affiliated
with, authorized, sponsored, endorsed or certified by Audinate Pty Ltd** or any
other company named below.

Virgil contains no Audinate software, firmware, SDKs or documentation. Its
compatibility with Dante networks comes from
[Inferno](https://github.com/teodly/inferno), an independent open-source
interoperability project, vendored with small patches in `third_party/inferno`
(the patches are listed in `third_party/inferno/README.md`).

For interoperability only, Virgil's device announcements carry the fixed
8-byte identifier field that the network protocol requires and that Dante
Controller checks before it will show a device's details and channels. That
value is a technical protocol constant; it is not a statement that Virgil is
made, sold or endorsed by Audinate, and Virgil identifies itself everywhere
else (device name, manufacturer and model fields) as Virgil / Inferno.

Virgil is provided "as is", without warranty of any kind (see sections 15 and
16 of the GNU General Public License in `LICENSE`). Do not rely on it where a
failure could cause harm or loss, such as safety announcements or live
broadcasts without a backup.

## Trademarks

Trademarks are used only to describe what Virgil works with (nominative use).
Their use does not imply any affiliation with or endorsement by their owners.

- **Dante®** and **Audinate®** are registered trademarks of Audinate Pty Ltd.
  "Dante Controller" is a product of Audinate.
- **ASIO** is a trademark and software of Steinberg Media Technologies GmbH.
- **Windows** is a trademark of Microsoft Corporation. **macOS** and
  **Core Audio** are trademarks of Apple Inc. **Linux** is a registered
  trademark of Linus Torvalds.
- All other names are the property of their respective owners.

"Virgil", the Virgil logo (laurel wreath and meter-bar flame) and its artwork
in `branding/` belong to this project.

## Licences

- **Virgil**: GNU General Public License version 3 (`LICENSE`).
- **Inferno** (`third_party/inferno/inferno_aoip`): GPLv3-or-later or
  AGPLv3-or-later, used here under the GPLv3
  (`third_party/inferno/LICENSE`, `third_party/inferno/LICENSE.GPL`).
- **searchfire** (`third_party/inferno/searchfire`): MIT or Apache-2.0.
- **usrvclock shim** (`third_party/inferno/usrvclock-rs`): MIT or Apache-2.0.
- Rust crates linked into `virgild` are listed with their licences in
  `bridge/Cargo.lock`; all are under permissive or GPL-compatible licences.
- **ASIO SDK**: the Windows ASIO driver is built against the Steinberg ASIO
  SDK, which is **not** included in this repository; the build downloads it
  from Steinberg. The SDK is offered by Steinberg under the GPLv3 (as an
  alternative to its proprietary licence), which is the licence Virgil's ASIO
  driver is distributed under. ASIO is a trademark and software of Steinberg
  Media Technologies GmbH.
