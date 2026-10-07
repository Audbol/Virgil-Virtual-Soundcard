# Vendored: Inferno

Unofficial, reverse-engineered implementation of the Dante protocol by
Teodor Woźniak and contributors: https://github.com/teodly/inferno
(also https://gitlab.com/lumifaza/inferno).

* Upstream commit: `9767558` (2026-09-08)
* License: GPLv3-or-later OR AGPLv3-or-later (Virgil uses it under GPLv3);
  `searchfire` is MIT OR Apache-2.0. See the LICENSE files in each crate.
* Inferno is not authorized or approved by Audinate, and neither is Virgil.

Vendored crates: `inferno_aoip` (protocol), `searchfire` (mDNS).

Changes for Virgil:

1. `usrvclock-rs/` is a new crate with the same API as upstream `usrvclock`.
   Upstream receives PTP clock overlays from an external daemon (a Statime
   fork) through a Unix socket, which is Linux-only. Virgil's daemon runs
   its own PTPv1 follower and publishes overlays in-process. This works on
   Windows and macOS too.
2. `inferno_aoip/src/util/os.rs`: the non-Unix branch of
   `set_current_thread_realtime` had a type mismatch and a missing import,
   so it did not compile for Windows.
3. `TX_SEND_DELAY_NS` setting (`settings.rs`, `mod.rs`, `flows_tx.rs`).
   Upstream hardcodes the transmit send latency to 0, so frame T is read
   from the external ring at media time T, before Virgil's mixer has
   written it. With the patch, frame T is read and sent at T + delay and
   its packet carries timestamp T, which is how Dante Virtual Soundcard
   behaves (receivers must use a latency >= the advertised TX latency).
   The default of 0 keeps the upstream behaviour.
4. `flows_tx.rs`: the "clock unavailable" error is not logged for the first
   clock check at startup, which misses normally.
