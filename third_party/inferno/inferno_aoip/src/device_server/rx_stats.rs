//! Virgil patch: receive-side counters for dropout diagnostics, shared by all
//! flows. Read (and the latency maximum reset) with `take()`.

use std::sync::atomic::{AtomicU64, Ordering};

static PACKETS: AtomicU64 = AtomicU64::new(0);
static LATE_PACKETS: AtomicU64 = AtomicU64::new(0);
static GAPS: AtomicU64 = AtomicU64::new(0);
static MISSING_SAMPLES: AtomicU64 = AtomicU64::new(0);
static MAX_AGE_SAMPLES: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Debug, Default)]
pub struct RxStats {
  /// Audio packets received.
  pub packets: u64,
  /// Packets that arrived after their receive latency had passed (their
  /// audio came too late to be played).
  pub late_packets: u64,
  /// Holes in a flow's timeline (lost or dropped packets).
  pub gaps: u64,
  /// Samples (per channel) that never arrived.
  pub missing_samples: u64,
  /// Largest packet age on arrival since the previous take(), in samples.
  pub max_age_samples: u64,
}

pub(crate) fn packet(age_samples: Option<u64>, latency_samples: usize, sample_rate: u32) {
  PACKETS.fetch_add(1, Ordering::Relaxed);
  // An "age" over a second does not mean a slow network: the sender and this
  // device are not on the same clock yet (e.g. before either follows the
  // leader). Such packets are neither late nor counted in the maximum.
  let age_samples = age_samples.filter(|&age| age <= sample_rate as u64);
  if let Some(age) = age_samples {
    MAX_AGE_SAMPLES.fetch_max(age, Ordering::Relaxed);
    if age > latency_samples as u64 {
      LATE_PACKETS.fetch_add(1, Ordering::Relaxed);
    }
  }
}

pub(crate) fn gap(samples: u64) {
  GAPS.fetch_add(1, Ordering::Relaxed);
  MISSING_SAMPLES.fetch_add(samples, Ordering::Relaxed);
}

/// Totals since start; `max_age_samples` since the previous call.
pub fn take() -> RxStats {
  RxStats {
    packets: PACKETS.load(Ordering::Relaxed),
    late_packets: LATE_PACKETS.load(Ordering::Relaxed),
    gaps: GAPS.load(Ordering::Relaxed),
    missing_samples: MISSING_SAMPLES.load(Ordering::Relaxed),
    max_age_samples: MAX_AGE_SAMPLES.swap(0, Ordering::Relaxed),
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn ages_beyond_a_second_are_not_late() {
    let before = take();
    packet(Some(300), 192, 48000); // late
    packet(Some(100), 192, 48000); // in time
    packet(Some(i32::MAX as u64), 192, 48000); // other clock: ignored
    let after = take();
    assert_eq!(after.packets - before.packets, 3);
    assert_eq!(after.late_packets - before.late_packets, 1);
    assert_eq!(after.max_age_samples, 300);
  }
}
