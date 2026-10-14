//! Virgil patch: transmit-side counters for dropout diagnostics, shared by
//! all flows. Read (and the lag maxima reset) with `take()`. Realtime-safe:
//! relaxed atomics only, no allocation.

use std::sync::atomic::{AtomicU64, Ordering};

static LATE_WAKEUPS: AtomicU64 = AtomicU64::new(0);
static DROPPED_SAMPLES: AtomicU64 = AtomicU64::new(0);
static MAX_LAG_SAMPLES: AtomicU64 = AtomicU64::new(0);
static CLOCK_JUMPS: AtomicU64 = AtomicU64::new(0);
static SEND_ERRORS: AtomicU64 = AtomicU64::new(0);
static MAX_WAKEUP_LAG_SAMPLES: AtomicU64 = AtomicU64::new(0);
static LAG_OVER_0_5MS: AtomicU64 = AtomicU64::new(0);
static LAG_OVER_1MS: AtomicU64 = AtomicU64::new(0);
static LAG_OVER_2MS: AtomicU64 = AtomicU64::new(0);
static LAG_OVER_4MS: AtomicU64 = AtomicU64::new(0);
static RUNNING_WAKEUPS: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Debug, Default)]
pub struct TxStats {
  /// Times the transmit thread woke up later than the transmit latency, so
  /// the audio it should have sent by then was skipped (dropout on the
  /// network). Counted once per wakeup, however many flows were late.
  pub late_wakeups: u64,
  /// Samples (per channel) that were skipped because of late wakeups.
  pub dropped_samples: u64,
  /// Largest late-wakeup lag since the previous take(), in samples.
  pub max_lag_samples: u64,
  /// Media clock jumps (forwards or backwards) seen by a running flow.
  pub clock_jumps: u64,
  /// Packets that could not be sent (send error or short write); each one
  /// is dropped, i.e. its audio is missing at the receivers.
  pub send_errors: u64,
  /// Largest lag of any wakeup (late or not) since the previous take(), in
  /// samples: how close the thread came to dropping audio.
  pub max_wakeup_lag_samples: u64,
  /// Wakeups (of running flows) whose lag exceeded 0.5 / 1 / 2 / 4 ms. The
  /// buckets are cumulative: a 3 ms wakeup counts in the first three.
  pub lag_over_0_5ms: u64,
  pub lag_over_1ms: u64,
  pub lag_over_2ms: u64,
  pub lag_over_4ms: u64,
  /// Wakeups that measured the lag of at least one running flow (packets
  /// were going out): tells "no lag" apart from "nothing was sent".
  pub running_wakeups: u64,
}

/// A running flow woke `lag_samples` late and skipped `dropped_samples`.
/// `first_in_wakeup` is false for further flows late in the same wakeup:
/// they only update the maximum.
pub(crate) fn late_wakeup(lag_samples: u64, dropped_samples: u64, first_in_wakeup: bool) {
  if first_in_wakeup {
    LATE_WAKEUPS.fetch_add(1, Ordering::Relaxed);
    DROPPED_SAMPLES.fetch_add(dropped_samples, Ordering::Relaxed);
  }
  MAX_LAG_SAMPLES.fetch_max(lag_samples, Ordering::Relaxed);
}

pub(crate) fn clock_jump() {
  CLOCK_JUMPS.fetch_add(1, Ordering::Relaxed);
}

pub(crate) fn send_error() {
  SEND_ERRORS.fetch_add(1, Ordering::Relaxed);
}

/// The worst lag of the running flows in one wakeup (call once per wakeup).
pub(crate) fn wakeup_lag(lag_samples: u64, sample_rate: u32) {
  MAX_WAKEUP_LAG_SAMPLES.fetch_max(lag_samples, Ordering::Relaxed);
  // lag in ms > x  <=>  lag * 2000 > x * 2 * sample_rate
  let half_ms = lag_samples.saturating_mul(2000);
  let sr = sample_rate as u64;
  for (threshold_half_ms, counter) in
    [(1u64, &LAG_OVER_0_5MS), (2, &LAG_OVER_1MS), (4, &LAG_OVER_2MS), (8, &LAG_OVER_4MS)]
  {
    if half_ms > threshold_half_ms * sr {
      counter.fetch_add(1, Ordering::Relaxed);
    } else {
      break;
    }
  }
}

/// A wakeup measured the lag of at least one running flow (call once per
/// wakeup, after wakeup_lag).
pub(crate) fn running_wakeup() {
  RUNNING_WAKEUPS.fetch_add(1, Ordering::Relaxed);
}

/// Totals since start; the `max_*` fields since the previous call.
pub fn take() -> TxStats {
  TxStats {
    late_wakeups: LATE_WAKEUPS.load(Ordering::Relaxed),
    dropped_samples: DROPPED_SAMPLES.load(Ordering::Relaxed),
    max_lag_samples: MAX_LAG_SAMPLES.swap(0, Ordering::Relaxed),
    clock_jumps: CLOCK_JUMPS.load(Ordering::Relaxed),
    send_errors: SEND_ERRORS.load(Ordering::Relaxed),
    max_wakeup_lag_samples: MAX_WAKEUP_LAG_SAMPLES.swap(0, Ordering::Relaxed),
    lag_over_0_5ms: LAG_OVER_0_5MS.load(Ordering::Relaxed),
    lag_over_1ms: LAG_OVER_1MS.load(Ordering::Relaxed),
    lag_over_2ms: LAG_OVER_2MS.load(Ordering::Relaxed),
    lag_over_4ms: LAG_OVER_4MS.load(Ordering::Relaxed),
    running_wakeups: RUNNING_WAKEUPS.load(Ordering::Relaxed),
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::Mutex;

  // The counters are process-wide; tests that check them must not overlap.
  static SERIAL: Mutex<()> = Mutex::new(());

  #[test]
  fn late_wakeups_count_once_per_wakeup() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let before = take();
    late_wakeup(500, 512, true);
    late_wakeup(700, 768, false); // another flow, same wakeup
    late_wakeup(300, 320, true);
    clock_jump();
    send_error();
    running_wakeup();
    let after = take();
    assert_eq!(after.late_wakeups - before.late_wakeups, 2);
    assert_eq!(after.dropped_samples - before.dropped_samples, 512 + 320);
    assert_eq!(after.max_lag_samples, 700);
    assert_eq!(after.clock_jumps - before.clock_jumps, 1);
    assert_eq!(after.send_errors - before.send_errors, 1);
    assert_eq!(after.running_wakeups - before.running_wakeups, 1);
    assert_eq!(take().max_lag_samples, 0); // reset by take()
  }

  #[test]
  fn wakeup_lag_buckets() {
    let _g = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let before = take();
    // 48 kHz: 0.5 ms = 24, 1 ms = 48, 2 ms = 96, 4 ms = 192 samples
    wakeup_lag(0, 48000);
    wakeup_lag(24, 48000); // exactly 0.5 ms: not over
    wakeup_lag(25, 48000); // > 0.5
    wakeup_lag(49, 48000); // > 0.5, > 1
    wakeup_lag(100, 48000); // > 0.5, > 1, > 2
    wakeup_lag(193, 48000); // all four
    let after = take();
    assert_eq!(after.lag_over_0_5ms - before.lag_over_0_5ms, 4);
    assert_eq!(after.lag_over_1ms - before.lag_over_1ms, 3);
    assert_eq!(after.lag_over_2ms - before.lag_over_2ms, 2);
    assert_eq!(after.lag_over_4ms - before.lag_over_4ms, 1);
    assert_eq!(after.max_wakeup_lag_samples, 193);
    assert_eq!(take().max_wakeup_lag_samples, 0); // reset by take()
    // 44.1 kHz: 1 ms = 44.1 samples, so 45 is over 1 ms and 44 is not
    let before = take();
    wakeup_lag(44, 44100);
    wakeup_lag(45, 44100);
    let after = take();
    assert_eq!(after.lag_over_1ms - before.lag_over_1ms, 1);
  }
}
