//! Virgil patch: transmit-side counters for dropout diagnostics, shared by
//! all flows. Read (and the lag maximum reset) with `take()`. Realtime-safe:
//! relaxed atomics only, no allocation.

use std::sync::atomic::{AtomicU64, Ordering};

static LATE_WAKEUPS: AtomicU64 = AtomicU64::new(0);
static DROPPED_SAMPLES: AtomicU64 = AtomicU64::new(0);
static MAX_LAG_SAMPLES: AtomicU64 = AtomicU64::new(0);
static CLOCK_JUMPS: AtomicU64 = AtomicU64::new(0);

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

/// Totals since start; `max_lag_samples` since the previous call.
pub fn take() -> TxStats {
  TxStats {
    late_wakeups: LATE_WAKEUPS.load(Ordering::Relaxed),
    dropped_samples: DROPPED_SAMPLES.load(Ordering::Relaxed),
    max_lag_samples: MAX_LAG_SAMPLES.swap(0, Ordering::Relaxed),
    clock_jumps: CLOCK_JUMPS.load(Ordering::Relaxed),
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn late_wakeups_count_once_per_wakeup() {
    let before = take();
    late_wakeup(500, 512, true);
    late_wakeup(700, 768, false); // another flow, same wakeup
    late_wakeup(300, 320, true);
    clock_jump();
    let after = take();
    assert_eq!(after.late_wakeups - before.late_wakeups, 2);
    assert_eq!(after.dropped_samples - before.dropped_samples, 512 + 320);
    assert_eq!(after.max_lag_samples, 700);
    assert_eq!(after.clock_jumps - before.clock_jumps, 1);
    assert_eq!(take().max_lag_samples, 0); // reset by take()
  }
}
