//! Virgil patch: receive-side counters for dropout diagnostics, shared by all
//! flows. Read (and the latency maximum reset) with `take()`.

use std::sync::atomic::{AtomicU64, Ordering};

use crate::common::{wrapped_diff, Clock, ClockDiff};

static PACKETS: AtomicU64 = AtomicU64::new(0);
static LATE_PACKETS: AtomicU64 = AtomicU64::new(0);
static GAPS: AtomicU64 = AtomicU64::new(0);
static MISSING_SAMPLES: AtomicU64 = AtomicU64::new(0);
static MAX_AGE_SAMPLES: AtomicU64 = AtomicU64::new(0);
static RECV_ERRORS: AtomicU64 = AtomicU64::new(0);

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
  /// Receive errors on flow sockets and packets too short to be audio.
  pub recv_errors: u64,
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

pub(crate) fn recv_error() {
  RECV_ERRORS.fetch_add(1, Ordering::Relaxed);
}

/// Finds the holes in one flow's timeline. A hole is only reported (by
/// `packet()` returning its size) once it can no longer be filled by a
/// reordered packet: when the stream has moved `window` samples past it or a
/// newer hole opens. Reordered packets that arrive at either edge of the
/// pending hole shrink it, so plain reordering is not counted as loss and the
/// counters only ever grow.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct GapTracker {
  next: Option<Clock>,
  /// Unfilled part [front, back) of the newest hole.
  pending: Option<(Clock, Clock)>,
}

impl GapTracker {
  fn take_pending(&mut self) -> Option<u64> {
    self.pending.take().map(|(front, back)| wrapped_diff(back, front)).filter(|&n| n > 0).map(|n| n as u64)
  }

  /// A packet of `samples` frames stamped `ts`. Returns a confirmed hole's
  /// size in samples. Jumps of a second or more are restarts, not holes.
  pub(crate) fn packet(&mut self, ts: Clock, samples: usize, sample_rate: u32, window: usize) -> Option<u64> {
    let end = ts.wrapping_add(samples);
    let expected = match self.next {
      None => {
        self.next = Some(end);
        return None;
      }
      Some(e) => e,
    };
    let d = wrapped_diff(ts, expected);
    let sr = sample_rate as ClockDiff;
    if d >= sr || d <= -sr {
      // sender restarted: what was pending is lost for good
      self.next = Some(end);
      return self.take_pending();
    }
    if d > 0 {
      let confirmed = self.take_pending();
      self.pending = Some((expected, ts));
      self.next = Some(end);
      return confirmed;
    }
    if d < 0 {
      // older than expected: reordered (or duplicate); keep the timeline
      if let Some((front, back)) = self.pending.as_mut() {
        if ts == *front {
          *front = if wrapped_diff(end, *back) >= 0 { *back } else { end };
        } else if end == *back && wrapped_diff(ts, *front) > 0 {
          *back = ts;
        }
        if wrapped_diff(*back, *front) <= 0 {
          self.pending = None;
        }
      }
      return None;
    }
    self.next = Some(end);
    if let Some((_, back)) = self.pending {
      if wrapped_diff(ts, back) >= window as ClockDiff {
        return self.take_pending();
      }
    }
    None
  }
}

/// Totals since start; `max_age_samples` since the previous call.
pub fn take() -> RxStats {
  RxStats {
    packets: PACKETS.load(Ordering::Relaxed),
    late_packets: LATE_PACKETS.load(Ordering::Relaxed),
    gaps: GAPS.load(Ordering::Relaxed),
    missing_samples: MISSING_SAMPLES.load(Ordering::Relaxed),
    max_age_samples: MAX_AGE_SAMPLES.swap(0, Ordering::Relaxed),
    recv_errors: RECV_ERRORS.load(Ordering::Relaxed),
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

  #[test]
  fn in_order_stream_has_no_gaps() {
    let mut g = GapTracker::default();
    for i in 0..100 {
      assert_eq!(g.packet(i * 16, 16, 48000, 48), None);
    }
  }

  #[test]
  fn lost_packet_is_confirmed_after_the_window() {
    let mut g = GapTracker::default();
    assert_eq!(g.packet(0, 16, 48000, 48), None);
    assert_eq!(g.packet(32, 16, 48000, 48), None); // 16..32 missing, pending
    assert_eq!(g.packet(48, 16, 48000, 48), None);
    assert_eq!(g.packet(64, 16, 48000, 48), None);
    assert_eq!(g.packet(80, 16, 48000, 48), Some(16)); // 80 - 32 >= 48
    assert_eq!(g.packet(96, 16, 48000, 48), None); // reported once
  }

  #[test]
  fn reordered_packet_is_not_a_gap() {
    let mut g = GapTracker::default();
    g.packet(0, 16, 48000, 48);
    g.packet(32, 16, 48000, 48); // 16 late
    assert_eq!(g.packet(16, 16, 48000, 48), None); // arrives: fills the hole
    for ts in [48, 64, 80, 96, 112] {
      assert_eq!(g.packet(ts, 16, 48000, 48), None);
    }
  }

  #[test]
  fn partly_refilled_hole_counts_the_rest() {
    let mut g = GapTracker::default();
    g.packet(0, 16, 48000, 48);
    g.packet(64, 16, 48000, 48); // 16..64 missing (3 packets)
    g.packet(48, 16, 48000, 48); // back edge refilled
    g.packet(16, 16, 48000, 48); // front edge refilled
    g.packet(16, 16, 48000, 48); // duplicate: no effect
    assert_eq!(g.packet(80, 16, 48000, 48), None);
    assert_eq!(g.packet(96, 16, 48000, 48), Some(16)); // 32..48 never came
  }

  #[test]
  fn newer_hole_confirms_the_previous_one() {
    let mut g = GapTracker::default();
    g.packet(0, 16, 48000, 4800);
    g.packet(32, 16, 48000, 4800);
    assert_eq!(g.packet(80, 16, 48000, 4800), Some(16)); // new hole 48..80
    assert_eq!(g.packet(200_000, 16, 48000, 4800), Some(32)); // restart
    assert_eq!(g.packet(200_016, 16, 48000, 4800), None);
  }
}
