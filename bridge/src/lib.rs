//! Virgil <-> Inferno bridge (C ABI). See include/virgil/dante_bridge.h.
//!
//! Inferno runs on its own thread with a tokio runtime. It reads transmit
//! samples from, and writes received samples to, rings owned by the daemon,
//! indexed by the same absolute media-clock frame the daemon uses:
//! * transmit: a packet stamped media time T carries ring frames
//!   [T - tx_latency, ...)
//! * receive: a packet stamped T lands at ring frame T + rx_latency, and
//!   frames before "now" are final (Inferno fills gaps with silence).

use std::collections::BTreeMap;
use std::ffi::{c_char, CStr, CString};
use std::net::Ipv4Addr;
use std::sync::atomic::AtomicUsize;
use std::sync::{mpsc, Arc, RwLock};
use std::thread::JoinHandle;

use inferno_aoip::device_server::{AtomicSample, DeviceServer, ExternalBufferParameters, Sample, Settings};

#[repr(C)]
pub struct VgDanteConfig {
    name: *const c_char,
    bind_ip: *const c_char,
    sample_rate: u32,
    tx_channels: u32,
    rx_channels: u32,
    tx_latency_ns: u32,
    rx_latency_ns: u32,
    tx_send_delay_ns: u32,
    tx_ring: *mut i32,
    tx_ring_frames: u32,
    rx_ring: *mut i32,
    rx_ring_frames: u32,
    mono_ns: Option<extern "C" fn() -> i64>,
    log: Option<extern "C" fn(i32, *const c_char)>,
}

struct Handle {
    stop: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<JoinHandle<()>>,
    valid: Arc<RwLock<bool>>,
}

// ---- logging ----------------------------------------------------------------

static LOG_SINK: RwLock<Option<extern "C" fn(i32, *const c_char)>> = RwLock::new(None);

struct BridgeLogger;

impl log::Log for BridgeLogger {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Info || (m.level() <= log::Level::Debug && std::env::var_os("VIRGIL_DEBUG").is_some())
    }
    fn log(&self, record: &log::Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let level = match record.level() {
            log::Level::Error => 0,
            log::Level::Warn => 1,
            log::Level::Info => 2,
            _ => 3,
        };
        // Inferno logs from its real-time threads: never write the log file
        // there. Hand the line to the log thread; drop it if that is behind.
        if let Some(tx) = LOG_TX.get() {
            let _ = tx.try_send((level, format!("dante: {}", record.args())));
        }
    }
    fn flush(&self) {}
}

static LOGGER: BridgeLogger = BridgeLogger;
static TEST_CRASHED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static PANICKED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Inferno runs its audio on threads of its own: a panic there would end the
/// thread silently. Log it (synchronously, it is the last word) and flag it
/// so the daemon can restart the engine.
fn install_panic_hook() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        std::panic::set_hook(Box::new(|info| {
            PANICKED.store(true, std::sync::atomic::Ordering::SeqCst);
            let thread = std::thread::current().name().unwrap_or("?").to_owned();
            let msg = info
                .payload()
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| info.payload().downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_else(|| "panic".into());
            let loc = info.location().map(|l| format!(" at {}:{}", l.file(), l.line())).unwrap_or_default();
            log_line(0, &format!("dante: thread '{thread}' crashed: {msg}{loc}"));
        }));
    });
}
static LOG_TX: std::sync::OnceLock<mpsc::SyncSender<(i32, String)>> = std::sync::OnceLock::new();

/// Log thread: writes lines through the C sink, collapsing bursts of the
/// same message (e.g. a send error per packet) into one line per second.
fn start_log_thread() {
    LOG_TX.get_or_init(|| {
        let (tx, rx) = mpsc::sync_channel::<(i32, String)>(1024);
        std::thread::Builder::new()
            .name("virgil-dante-log".into())
            .spawn(move || {
                use std::time::{Duration, Instant};
                let mut last: Option<(i32, String)> = None;
                let mut repeats = 0u64;
                let mut window = Instant::now();
                let flush_repeats = |last: &Option<(i32, String)>, repeats: &mut u64| {
                    if let (Some((lvl, msg)), true) = (last, *repeats > 0) {
                        log_line(*lvl, &format!("{msg} (repeated {repeats} more times)"));
                        *repeats = 0;
                    }
                };
                loop {
                    match rx.recv_timeout(Duration::from_millis(1000)) {
                        Ok((lvl, msg)) => {
                            if last.as_ref().map_or(false, |(_, m)| *m == msg) && window.elapsed() < Duration::from_secs(1) {
                                repeats += 1;
                                continue;
                            }
                            flush_repeats(&last, &mut repeats);
                            log_line(lvl, &msg);
                            last = Some((lvl, msg));
                            window = Instant::now();
                        }
                        Err(mpsc::RecvTimeoutError::Timeout) => {
                            flush_repeats(&last, &mut repeats);
                            window = Instant::now();
                        }
                        Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    }
                }
            })
            .expect("log thread");
        tx
    });
}

fn log_line(level: i32, msg: &str) {
    if let Some(f) = *LOG_SINK.read().unwrap() {
        if let Ok(c) = CString::new(msg) {
            f(level, c.as_ptr());
        }
    } else {
        eprintln!("{msg}");
    }
}

unsafe fn cstr(p: *const c_char) -> Option<String> {
    if p.is_null() {
        None
    } else {
        Some(CStr::from_ptr(p).to_string_lossy().into_owned())
    }
}

/// One ExternalBufferParameters per channel over an interleaved ring.
unsafe fn channel_views(
    ring: *mut i32,
    frames: usize,
    channels: usize,
    valid: &Arc<RwLock<bool>>,
) -> Vec<ExternalBufferParameters<Sample>> {
    (0..channels)
        .map(|ch| {
            ExternalBufferParameters::new(
                (ring as *const AtomicSample).add(ch),
                frames * channels - ch,
                channels,
                valid.clone(),
                None,
            )
        })
        .collect()
}

// ---- C ABI ---------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn vg_dante_set_clock(last_sync: i64, shift: i64, freq_scale: f64) {
    usrvclock::publish(Some(usrvclock::ClockOverlay { clock_id: 1, last_sync, shift, freq_scale }));
}

/// # Safety
/// `config` must point to a valid VgDanteConfig whose rings outlive the device.
#[no_mangle]
pub unsafe extern "C" fn vg_dante_start(config: *const VgDanteConfig) -> *mut std::ffi::c_void {
    let c = match config.as_ref() {
        Some(c) => c,
        None => return std::ptr::null_mut(),
    };
    *LOG_SINK.write().unwrap() = c.log;
    start_log_thread();
    install_panic_hook();
    PANICKED.store(false, std::sync::atomic::Ordering::SeqCst);
    // Tests: simulate a crash in one of the Dante threads after N seconds.
    if let Some(secs) = std::env::var("VIRGIL_TEST_DANTE_CRASH_S").ok().and_then(|s| s.parse::<u64>().ok()) {
        if !TEST_CRASHED.swap(true, std::sync::atomic::Ordering::SeqCst) {
            let _ = std::thread::Builder::new().name("test-crash".into()).spawn(move || {
                std::thread::sleep(std::time::Duration::from_secs(secs));
                panic!("simulated crash (VIRGIL_TEST_DANTE_CRASH_S)");
            });
        }
    }
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Debug);
    if let Some(f) = c.mono_ns {
        usrvclock::set_underlying_clock(f);
    }

    let frames_ok = |f: u32| f.is_power_of_two() && f >= 1024;
    if !frames_ok(c.tx_ring_frames) || !frames_ok(c.rx_ring_frames) {
        log_line(0, "dante: ring sizes must be powers of two");
        return std::ptr::null_mut();
    }
    let ip: Option<Ipv4Addr> = cstr(c.bind_ip).and_then(|s| s.parse().ok());
    if ip.is_none() {
        log_line(0, "dante: bind_ip is not an IPv4 address");
        return std::ptr::null_mut();
    }
    let name = cstr(c.name).unwrap_or_else(|| "Virgil".into());

    let mut cfg = BTreeMap::new();
    cfg.insert("NAME".to_owned(), name.chars().take(31).collect::<String>());
    cfg.insert("SAMPLE_RATE".to_owned(), c.sample_rate.to_string());
    cfg.insert("TX_CHANNELS".to_owned(), c.tx_channels.to_string());
    cfg.insert("RX_CHANNELS".to_owned(), c.rx_channels.to_string());
    cfg.insert("TX_LATENCY_NS".to_owned(), c.tx_latency_ns.to_string());
    cfg.insert("RX_LATENCY_NS".to_owned(), c.rx_latency_ns.to_string());
    cfg.insert("TX_SEND_DELAY_NS".to_owned(), c.tx_send_delay_ns.to_string());
    // Stamp packets tx_latency after their samples' media time: they are sent
    // tx_send_delay after it, so every receiver gets them before even its
    // own latency starts counting, whatever its latency setting (as Dante
    // Virtual Soundcard does; the latency shows up as our output latency).
    cfg.insert("TX_TIMESTAMP_OFFSET_NS".to_owned(), c.tx_latency_ns.to_string());

    let valid = Arc::new(RwLock::new(true));
    let tx_views = channel_views(c.tx_ring, c.tx_ring_frames as usize, c.tx_channels as usize, &valid);
    let rx_views = channel_views(c.rx_ring, c.rx_ring_frames as usize, c.rx_channels as usize, &valid);
    let (tx_ch, rx_ch) = (c.tx_channels, c.rx_channels);

    let (stop_tx, stop_rx) = tokio::sync::oneshot::channel::<()>();
    let (ready_tx, ready_rx) = mpsc::channel::<Result<(), String>>();

    let thread = std::thread::Builder::new().name("virgil-dante".into()).spawn(move || {
        let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
            Ok(rt) => rt,
            Err(e) => {
                let _ = ready_tx.send(Err(format!("tokio runtime: {e}")));
                return;
            }
        };
        let ready_tx2 = ready_tx.clone();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            rt.block_on(async move {
                let settings = Settings::new("Virgil", "Virgil", ip, &cfg);
                let mut server = DeviceServer::start(settings).await;
                if rx_ch > 0 {
                    let (t, r) = tokio::sync::oneshot::channel();
                    let _ = t.send(0);
                    server.receive_to_external_buffer(rx_views, r, Arc::new(AtomicUsize::new(usize::MAX)), None).await;
                }
                if tx_ch > 0 {
                    let (t, r) = tokio::sync::oneshot::channel();
                    let _ = t.send(0);
                    server.transmit_from_external_buffer(tx_views, r, Arc::new(AtomicUsize::new(usize::MAX)), None).await;
                }
                let _ = ready_tx2.send(Ok(()));
                let _ = stop_rx.await;
                if tx_ch > 0 {
                    server.stop_transmitter().await;
                }
                if rx_ch > 0 {
                    server.stop_receiver().await;
                }
                server.shutdown().await;
            })
        }));
        if let Err(p) = result {
            let msg = p
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| p.downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_else(|| "panic".into());
            log_line(0, &format!("dante: engine stopped: {msg}"));
            let _ = ready_tx.send(Err(msg));
        }
    });
    let thread = match thread {
        Ok(t) => t,
        Err(_) => return std::ptr::null_mut(),
    };
    match ready_rx.recv_timeout(std::time::Duration::from_secs(15)) {
        Ok(Ok(())) => Box::into_raw(Box::new(Handle { stop: Some(stop_tx), thread: Some(thread), valid })) as *mut _,
        Ok(Err(e)) => {
            log_line(0, &format!("dante: start failed: {e}"));
            let _ = thread.join();
            std::ptr::null_mut()
        }
        Err(_) => {
            log_line(0, "dante: start timed out");
            // Leave the thread; it owns nothing the caller frees until stop.
            Box::into_raw(Box::new(Handle { stop: Some(stop_tx), thread: Some(thread), valid })) as *mut _
        }
    }
}

/// # Safety
/// `handle` must come from vg_dante_start and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn vg_dante_stop(handle: *mut std::ffi::c_void) {
    if handle.is_null() {
        return;
    }
    let mut h = Box::from_raw(handle as *mut Handle);
    if let Some(s) = h.stop.take() {
        let _ = s.send(());
    }
    if let Some(t) = h.thread.take() {
        let _ = t.join();
    }
    // Inferno may still hold views until its threads are gone; they are.
    *h.valid.write().unwrap() = false;
}

/// 1 while the Dante stack runs normally; 0 once any of its threads crashed
/// or the main thread ended.
///
/// # Safety
/// `handle` must come from vg_dante_start and not have been stopped.
#[no_mangle]
pub unsafe extern "C" fn vg_dante_healthy(handle: *mut std::ffi::c_void) -> i32 {
    if handle.is_null() {
        return 0;
    }
    let h = &*(handle as *const Handle);
    let finished = h.thread.as_ref().map_or(true, |t| t.is_finished());
    (!finished && !PANICKED.load(std::sync::atomic::Ordering::SeqCst)) as i32
}
