//! In-process stand-in for `usrvclock` (same API surface as Inferno uses).
//!
//! The host process (Virgil's daemon) registers its monotonic clock with
//! [`set_underlying_clock`] and publishes PTP corrections with [`publish`].
//! Inferno subscribes through [`AsyncClient`] exactly as it would to the
//! out-of-process clock server.

use std::path::PathBuf;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::OnceLock;

/// Maps the host's monotonic clock onto PTP time:
/// `ptp_ns = t + shift + (t - last_sync) * freq_scale`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ClockOverlay {
    pub clock_id: i64,
    pub last_sync: i64,
    pub shift: i64,
    pub freq_scale: f64,
}

#[derive(Debug)]
pub enum OverlayReceiveError {
    PacketTooShort,
    UnexpectedData,
    UnsupportedMajorVersion,
    InvalidFlags,
}

impl std::fmt::Display for OverlayReceiveError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}

type MonoFn = extern "C" fn() -> i64;

static UNDERLYING: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());

extern "C" fn fallback_now() -> i64 {
    static START: OnceLock<std::time::Instant> = OnceLock::new();
    START.get_or_init(std::time::Instant::now).elapsed().as_nanos() as i64
}

/// Registers the clock that `last_sync` and overlays refer to. Must be the
/// same clock the host uses when computing overlays.
pub fn set_underlying_clock(f: MonoFn) {
    UNDERLYING.store(f as *mut (), Ordering::Release);
}

#[inline(always)]
fn underlying_now() -> i64 {
    let p = UNDERLYING.load(Ordering::Acquire);
    if p.is_null() {
        fallback_now()
    } else {
        // SAFETY: only ever stored from a `MonoFn` in `set_underlying_clock`.
        let f: MonoFn = unsafe { std::mem::transmute(p) };
        f()
    }
}

impl ClockOverlay {
    /// Calculates timestamp in overlay clock's timescale given underlying clock's timestamp.
    pub fn underlying_to_overlay_ns(&self, timestamp: i64) -> i64 {
        let elapsed = timestamp.wrapping_sub(self.last_sync);
        let correction = ((elapsed as f64) * self.freq_scale).round() as i64;
        timestamp.wrapping_add(self.shift).wrapping_add(correction)
    }
    pub fn now_underlying_ns(&self) -> i64 {
        underlying_now()
    }
    pub fn now_ns(&self) -> i64 {
        self.underlying_to_overlay_ns(self.now_underlying_ns())
    }
    pub fn freq_scale_including_hw(&self) -> f64 {
        self.freq_scale
    }
}

/// Kept for API compatibility; Inferno only uses it with `use_safe_clock`
/// disabled.
pub struct SafeClock {
    _tolerance: f64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct SafeTimestamp {
    pub nanos: i64,
    pub estimated: bool,
}

impl SafeTimestamp {
    pub fn precise_ns(&self) -> Option<i64> {
        (!self.estimated).then_some(self.nanos)
    }
}

impl SafeClock {
    pub fn new(tolerance: f64, _timeout_ns: i64) -> Self {
        Self { _tolerance: tolerance }
    }
    pub fn now(&mut self, overlay: &ClockOverlay) -> SafeTimestamp {
        SafeTimestamp { nanos: overlay.now_ns(), estimated: false }
    }
}

pub const DEFAULT_SERVER_SOCKET_PATH: &str = "in-process";

fn channel() -> &'static tokio::sync::watch::Sender<Option<ClockOverlay>> {
    static CH: OnceLock<tokio::sync::watch::Sender<Option<ClockOverlay>>> = OnceLock::new();
    CH.get_or_init(|| tokio::sync::watch::channel(None).0)
}

/// Publishes a new overlay to every subscribed Inferno component.
pub fn publish(overlay: Option<ClockOverlay>) {
    channel().send_replace(overlay);
}

/// Subscription handle (the path is ignored: the source is in-process).
pub struct AsyncClient {
    _private: (),
}

impl AsyncClient {
    pub fn start(_path: PathBuf, _error_handler: Box<dyn FnMut(OverlayReceiveError) + Send>) -> Self {
        Self { _private: () }
    }
    pub fn subscribe(&self) -> tokio::sync::watch::Receiver<Option<ClockOverlay>> {
        channel().subscribe()
    }
    pub async fn stop(self) -> Result<(), OverlayReceiveError> {
        Ok(())
    }
}
