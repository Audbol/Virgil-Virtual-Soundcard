//! Integration test: loopback TX→RX audio transmission without Docker.
//!
//! Two Inferno instances run on the same host using different ALT_PORT/PROCESS_ID:
//! - TX device transmits a deterministic ramp pattern on 2 channels
//! - RX device receives and verifies the exact payload
//!
//! A fake usrvclock server provides the media clock.

use rand::rngs::SmallRng;
use rand::{Rng, SeedableRng};
use std::collections::BTreeMap;
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, RwLock};
use std::time::Duration;

use inferno_aoip::device_server::{
  AtomicSample, DeviceServer, ExternalBufferParameters, MediaClock, Sample, Settings, TransferNotifier,
};

const BUF_SIZE: usize = 65536;
const SAMPLE_RATE: u32 = 48000;

fn make_settings(
  name: &str,
  process_id: u16,
  alt_port: u16,
  rx_channels: usize,
  tx_channels: usize,
  clock_path: &str,
) -> Settings {
  let mut config = BTreeMap::new();
  config.insert("NAME".to_string(), name.to_string());
  config.insert("PROCESS_ID".to_string(), process_id.to_string());
  config.insert("ALT_PORT".to_string(), alt_port.to_string());
  config.insert("RX_CHANNELS".to_string(), rx_channels.to_string());
  config.insert("TX_CHANNELS".to_string(), tx_channels.to_string());
  config.insert("SAMPLE_RATE".to_string(), SAMPLE_RATE.to_string());
  config.insert("CLOCK_PATH".to_string(), clock_path.to_string());
  config.insert("BIND_IP".to_string(), "127.0.0.1".to_string());

  let mut settings = Settings::new(name, name, Some(Ipv4Addr::new(127, 0, 0, 1)), &config);
  settings.make_rx_channels(rx_channels);
  settings.make_tx_channels(tx_channels);
  settings
}

fn start_clock_server(path: &str, running: Arc<AtomicBool>) -> std::thread::JoinHandle<()> {
  let path = path.to_string();
  std::thread::spawn(move || {
    let mut server = usrvclock::Server::new(path.into()).unwrap();
    let overlay = usrvclock::ClockOverlay {
      clock_id: 1i64, // CLOCK_MONOTONIC
      last_sync: 0,
      shift: 0,
      freq_scale: 0.0,
    };
    while running.load(Ordering::Relaxed) {
      server.send(overlay);
      std::thread::sleep(Duration::from_millis(100));
    }
  })
}

async fn wait_for_clock(
  mut clock_rx: inferno_aoip::device_server::RealTimeClockReceiver,
  sample_rate: u32,
) -> usize {
  let mut media_clock = MediaClock::new(false);
  loop {
    clock_rx.update();
    if let Some(overlay) = clock_rx.get() {
      media_clock.update_overlay(*overlay);
      if let Some(now) = media_clock.wrapping_now_in_timebase(sample_rate as u64) {
        return now as usize;
      }
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
}

#[inline(always)]
fn compare_samples(a: &[AtomicSample], b: &[Sample]) -> bool {
  if a.len() != b.len() {
    return false;
  }
  let mut mindiff = Sample::MAX;
  let mut maxdiff = Sample::MIN;
  for (a, b) in a.iter().zip(b) {
    let diff = a.load(atomic::Ordering::Relaxed).saturating_sub(*b);
    if diff.saturating_abs() > 384 {
      return false;
    }
    if diff > maxdiff {
      maxdiff = diff;
    }
    if diff < mindiff {
      mindiff = diff;
    }
  }
  log::debug!("diff: {mindiff}..{maxdiff}");
  true
}

fn find_samples_offset(a: &[AtomicSample], b: &[Sample]) -> Option<usize> {
  let mut offset = 0;
  while offset < a.len() - b.len() {
    if compare_samples(&a[offset..][..b.len()], b) {
      return Some(offset);
    }
    offset += 1;
  }
  None
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn test_loopback_trx() {
  let _ = env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).try_init();

  let clock_path = "/tmp/inferno-test-usrvclock";
  let _ = std::fs::remove_file(clock_path);

  let clock_running = Arc::new(AtomicBool::new(true));
  let clock_thread = start_clock_server(clock_path, clock_running.clone());
  tokio::time::sleep(Duration::from_millis(300)).await;

  let tx_settings = make_settings("test_tx", 1, 9000, 0, 2, clock_path);
  let rx_settings = make_settings("test_rx", 2, 9100, 2, 0, clock_path);

  let mut tx_server = DeviceServer::start(tx_settings).await;
  let mut rx_server = DeviceServer::start(rx_settings).await;

  let tx_hostname = tx_server.self_info.friendly_hostname.clone();

  let tx_clock_rx = tx_server.get_realtime_clock_receiver();
  let rx_clock_rx = rx_server.get_realtime_clock_receiver();
  let tx_start_time = wait_for_clock(tx_clock_rx, SAMPLE_RATE).await;
  let _rx_start_time = wait_for_clock(rx_clock_rx, SAMPLE_RATE).await;

  let mut rng = SmallRng::from_rng(rand::thread_rng()).unwrap();

  let tx_buffers: Vec<Arc<Vec<AtomicSample>>> = (0..2)
    .map(move |_| Arc::new((0..BUF_SIZE).map(|_| AtomicSample::new(rng.gen::<Sample>())).collect()))
    .collect();

  for ch in 0..2 {
    let buf = &tx_buffers[ch];
    std::mem::forget(buf.clone()); // TODO: need to pin, too???
                                   // include some edge cases:
    buf[BUF_SIZE / 2].store(Sample::MIN, Ordering::Relaxed);
    buf[BUF_SIZE / 2 + 1].store(Sample::MAX, Ordering::Relaxed);
  }
  atomic::fence(Ordering::SeqCst);

  let tx_external_params: Vec<ExternalBufferParameters<Sample>> = (0..2)
    .map(|ch| {
      let buf = tx_buffers[ch].clone();
      unsafe {
        ExternalBufferParameters::new(
          buf.as_ptr(),
          buf.len(),
          1,
          Arc::new(RwLock::new(true)),
          None, // unconditional read
        )
      }
    })
    .collect();

  let (tx_start_tx, tx_start_rx) = tokio::sync::oneshot::channel::<usize>();
  let tx_current_timestamp: Arc<AtomicUsize> = Arc::new(AtomicUsize::new(usize::MAX));

  tx_server
    .transmit_from_external_buffer(
      tx_external_params,
      tx_start_rx,
      tx_current_timestamp.clone(),
      Some(TransferNotifier { callback: Box::new(|| {}), max_interval_samples: 480 }),
    )
    .await;

  let offset = Arc::new(AtomicUsize::new(0));
  let has_offset = Arc::new(AtomicBool::new(false));
  let total_samples_per_channel = Arc::new([AtomicUsize::new(0), AtomicUsize::new(0)]);

  let rx_callback = {
    let offset = offset.clone();
    let has_offset = has_offset.clone();
    let total_samples_per_channel = total_samples_per_channel.clone();
    let mut has_ch = [false; 2];

    move |samples_count: usize, channels: &Vec<Vec<Sample>>| {
      let mut start_pos_per_channel = [0; 2];
      for (chi, samples) in channels.iter().enumerate() {
        if !has_ch[chi] {
          for (i, sample) in samples[..samples_count].iter().enumerate() {
            if *sample != 0 {
              has_ch[chi] = true;
              start_pos_per_channel[chi] = i;
              break;
            }
          }
        }
      }
      if (!has_offset.load(atomic::Ordering::Relaxed)) && has_ch[0] {
        let found_offset =
          find_samples_offset(&tx_buffers[0], &channels[0][..samples_count][start_pos_per_channel[0]..]);
        if found_offset.is_none() {
          panic!("compared only non-zero samples but failed to find offset");
        }
        offset.store(found_offset.unwrap() - start_pos_per_channel[0], atomic::Ordering::Relaxed);
        has_offset.store(true, atomic::Ordering::Relaxed);
      }
      if has_offset.load(atomic::Ordering::Relaxed) {
        let offset_v = offset.load(atomic::Ordering::Relaxed);
        let cnt = samples_count.min(tx_buffers[0].len() - offset_v);
        if cnt == 0 {
          return;
        }

        for (chi, samples) in channels.iter().enumerate() {
          if !has_ch[chi] {
            continue;
          }
          let transmitted = &tx_buffers[chi];
          let start = start_pos_per_channel[chi];
          let ch_cnt = cnt - start;
          assert!(compare_samples(
            &transmitted[offset_v + start..][..ch_cnt],
            &samples[start..][..ch_cnt]
          ));
          total_samples_per_channel[chi].fetch_add(ch_cnt, atomic::Ordering::Relaxed);
        }
        offset.fetch_add(cnt, atomic::Ordering::Relaxed);
      }
    }
  };

  rx_server.receive_with_callback(Box::new(rx_callback)).await;

  let _ = tx_start_tx.send(tx_start_time);
  rx_server.subscribe(0, "TX 1", &tx_hostname).await;
  rx_server.subscribe(1, "TX 2", &tx_hostname).await;

  tokio::time::sleep(Duration::from_secs(2)).await;

  rx_server.shutdown().await;
  tx_server.shutdown().await;
  clock_running.store(false, Ordering::Relaxed);
  let _ = clock_thread.join();
  let _ = std::fs::remove_file(clock_path);

  atomic::fence(Ordering::SeqCst);

  log::info!(
    "offset: {:?}, total_samples_per_channel: {:?}",
    offset.load(Ordering::Relaxed),
    total_samples_per_channel
  );
  assert!(has_offset.load(atomic::Ordering::Relaxed));
  assert!(total_samples_per_channel[0].load(Ordering::Relaxed) > BUF_SIZE * 3 / 4);
  assert!(total_samples_per_channel[1].load(Ordering::Relaxed) > BUF_SIZE * 3 / 4);
}
