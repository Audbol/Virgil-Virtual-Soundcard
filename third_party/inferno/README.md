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
