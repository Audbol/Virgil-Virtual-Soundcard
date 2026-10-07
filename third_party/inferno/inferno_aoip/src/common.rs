pub use log::{debug, error, info, trace, warn};
pub type Sample = i32;
pub type USample = u32;

/// Audio clock (number of samples since arbitrary epoch). May wrap.
pub type Clock = usize;

/// Signed version of the clock. For clock deltas.
pub type ClockDiff = isize;

/// Non-wrapping clock
pub type LongClock = u64;

/// Signed version of non-wrapping clock. For clock deltas.
/// In a correctly working network, can be casted to ClockDiff without checks.
pub type LongClockDiff = i64;

/// Subtract clocks and return the result as a signed number.
/// Hint: wrapped `a > b` is equivalent to `wrapped_diff(a, b) > 0`
/// This function is intentionally not defined for LongClock, because diffs should never exceed i32 anyway.
pub fn wrapped_diff(a: Clock, b: Clock) -> ClockDiff {
  (a as ClockDiff).wrapping_sub(b as ClockDiff)
}

pub trait LogAndForget {
  fn log_and_forget(&self);
}

impl<T, E: std::fmt::Debug> LogAndForget for Result<T, E> {
  fn log_and_forget(&self) {
    if let Err(e) = self {
      warn!("Encountered error {e:?} at {:?}", std::backtrace::Backtrace::capture());
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn wrapped_diff_positive() {
    assert_eq!(wrapped_diff(10, 5), 5);
  }

  #[test]
  fn wrapped_diff_negative() {
    assert_eq!(wrapped_diff(5, 10), -5);
  }

  #[test]
  fn wrapped_diff_max_to_zero() {
    assert_eq!(wrapped_diff(usize::MAX, 0), -1);
  }

  #[test]
  fn wrapped_diff_zero_to_max() {
    assert_eq!(wrapped_diff(0, usize::MAX), 1);
  }

  #[test]
  fn wrapped_diff_same() {
    assert_eq!(wrapped_diff(0, 0), 0);
  }

  #[test]
  fn log_and_forget_ok() {
    let result: Result<(), &str> = Ok(());
    result.log_and_forget();
  }

  #[test]
  fn log_and_forget_err() {
    let result: Result<(), &str> = Err("test error");
    result.log_and_forget();
  }
}
