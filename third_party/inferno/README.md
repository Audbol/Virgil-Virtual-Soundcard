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
5. `TX_TIMESTAMP_OFFSET_NS` setting: the difference between a packet's
   timestamp and the media time of its samples (upstream: fixed -0.5 ms).
   Virgil sets it to its TX latency so packets are stamped later than they
   are sent and arrive in time for receivers with any latency setting
   (a WING at 1 ms dropped Virgil's packets otherwise).
6. `util/os.rs`: on Windows the real-time threads also join the MMCSS
   "Pro Audio" class.
7. `flows_tx.rs`: when the media clock steps backwards (e.g. the first lock
   to a Dante master, whose time counts from power-on), the transmitter's
   next command-processing time lay decades ahead, so it never again
   processed add-flow, set-channels or stop commands: receivers got only
   the first channel and stopping the engine hung. It is now clamped to one
   interval ahead.
8. `info_mcast_server.rs`: `set_clock_master()` lets the embedding program
   report the PTP clock leader. Inferno otherwise takes it from a
   `/tmp/clock-stats.*` file written by its statime fork, which Virgil does not
   use, so Dante Controller showed the sync status red with no clock details.
9. `state_storage.rs`: without a home directory (a systemd service with
   `DynamicUser=`, and no user database entry) the state directory lookup
   panicked and the device never started; it now falls back to the temp
   directory. Virgil's Linux service also sets `HOME` to its state directory.
10. `device_server/rx_stats.rs` (new) and `flows_rx.rs`: counters of received
    packets, packets that arrived after their receive latency, and gaps in a
    flow's timestamps (lost audio), for Virgil's dropout diagnostics.
11. `saved_settings.rs`, `settings.rs`: the embedding program can set channel
    names for the next start (`set_name_overrides`; they win over the saved
    ones and are saved), read the current names (`current_name`) and is told
    about renames from the network (`set_rename_hook`), so Virgil's
    configuration and Dante Controller show the same names.
12. Malformed control packets no longer panic: `flows_control_server.rs`,
    `arc_server.rs` and `proto_arc.rs::deserialize_items` check lengths and
    offsets before indexing, `flows_tx.rs` refuses flows whose channel numbers,
    channel count, frames per packet or sample size the transmit thread could
    not handle, and channel names that are not allowed are refused
    (`saved_settings.rs::valid_channel_name`).
13. Remote input that used to panic later: multicast flows that cannot be
    created are neither kept nor saved (`tx_multicasts.rs`), responses that
    would not fit a packet are answered with an error (`req_resp.rs`), remote
    names longer than 63 bytes are refused, advertised channels and bundles
    with unsupported channel counts, encodings or latencies are ignored
    (`mdns_client.rs`), receive flows are limited to the receiver's 32
    sockets (`channels_subscriber.rs`, `flows_rx.rs`), and the transmit thread
    ends when its transmitter is gone (`flows_tx.rs`).
14. `device_server/tx_stats.rs` (new) and `flows_tx.rs`: counters of the
    transmit thread's late wakeups (woke after the transmit latency, so
    audio was skipped), the samples skipped and the worst lag, for Virgil's
    dropout diagnostics. A flow counts only once it has sent a packet since
    its last (re)bootstrap, so the start of a flow (AddFlow, a resuscitated
    flow, the clock becoming available) is neither counted nor logged;
    clock jumps of a running flow are logged as such ("media clock jumped by
    X ms on a running flow") and the late-wakeup error line is limited to
    one per second. `channels_subscriber.rs`: a missing saved-state file
    (first start) is logged at info level instead of as a warning.
15. Dropout fixes and diagnostics (`flows_tx.rs`, `flows_rx.rs`,
    `tx_stats.rs`, `rx_stats.rs`, `util/os.rs`, `util/net.rs`,
    `channels_subscriber.rs`):
    * A running flow whose sending thread woke more than the transmit latency
      late keeps its packet phase and skips only the whole packets older than
      that (`late_skip_samples`), then sends the rest of the backlog;
      upstream restarted the flow at "now", dropping lag + 1 packet including
      audio receivers could still play. Flow start, resuscitation and clock
      jumps (> `DISCONTINUITY_THRESHOLD_SAMPLES`) still re-bootstrap.
    * A packet that cannot be sent (error or short write) is dropped and
      counted (`send_errors`) instead of retried in a tight loop, logged at
      most once a second; the 5 ms "blocked" guard no longer sleeps 2 ms.
    * `tx_stats` also records the worst lag of every wakeup and counts
      wakeups later than 0.5 / 1 / 2 / 4 ms (near misses), and the wakeups
      that saw a running flow (`running_wakeups`), so Virgil's transmit
      latency suggestion only counts time in which packets went out.
    * Windows: real-time threads join MMCSS "Pro Audio" at
      AVRT_PRIORITY_CRITICAL and their priority is not overridden by
      SetThreadPriority afterwards (that replaced the MMCSS boost with
      THREAD_PRIORITY_HIGHEST); without MMCSS they fall back to
      THREAD_PRIORITY_TIME_CRITICAL and the callers log a warning.
    * The receive loop drains the socket until WouldBlock (mio on Windows
      re-arms readiness only then): too-short packets are skipped and receive
      errors counted (`recv_errors`, at most 64 per wakeup) instead of ending
      the loop, which could leave a flow socket deaf until the flow timed out.
    * Flow sockets: 2 MB SO_RCVBUF on receive sockets (unicast and multicast);
      on Windows SIO_UDP_CONNRESET / SIO_UDP_NETRESET are disabled on all flow
      sockets so an ICMP port-unreachable does not fail later receives/sends.
    * The gap counter no longer counts reordering as loss: a hole is only
      reported once the stream is a receive latency past it (or a newer hole
      opens), and reordered packets at its edges shrink it first.
