use std::{
  net::Ipv4Addr,
  sync::{Arc, RwLock},
};

use netdev::mac::MacAddr;

#[derive(Clone)]
pub struct Channel {
  pub factory_name: String,
  pub friendly_name: Arc<RwLock<String>>, // Arc is needed only because of Clone requirement, TODO: fix the ALSA plugin
}

pub type DeviceId = [u8; 8];

#[derive(Clone)] // TODO: this shouldn't need to be clonable, fix the ALSA plugin
pub struct DeviceInfo {
  pub ip_address: Ipv4Addr,
  pub netmask: Ipv4Addr,
  pub gateway: Ipv4Addr,
  pub mac_address: MacAddr,
  pub link_speed: u16,

  pub board_name: String,
  pub manufacturer: String,
  pub model_name: String,
  pub model_number: String, // _000000000000000b
  pub factory_device_id: DeviceId,
  pub process_id: u16,
  pub vendor_string: String,
  pub friendly_hostname: String, // TODO limit length to 31, otherwise DC ignores the device
  pub factory_hostname: String,  // TODO as above

  pub rx_channels: Vec<Channel>,
  pub tx_channels: Vec<Channel>,
  pub bits_per_sample: u8,
  pub pcm_type: u8, // usually 0xe, in older devices 4
  pub latency_ns: usize,
  pub sample_rate: u32,

  pub arc_port: u16,
  pub cmc_port: u16,
  pub flows_control_port: u16,
  pub info_request_port: u16,
}

impl DeviceInfo {
  pub fn latency_samples(&self) -> usize {
    self.latency_ns * (self.sample_rate as usize) / 1_000_000_000
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn dummy_device_info(latency_ns: usize, sample_rate: u32) -> DeviceInfo {
    DeviceInfo {
      ip_address: Ipv4Addr::UNSPECIFIED,
      netmask: Ipv4Addr::UNSPECIFIED,
      gateway: Ipv4Addr::UNSPECIFIED,
      mac_address: MacAddr::zero(),
      link_speed: 0,
      board_name: String::new(),
      manufacturer: String::new(),
      model_name: String::new(),
      model_number: String::new(),
      factory_device_id: [0; 8],
      process_id: 0,
      vendor_string: String::new(),
      friendly_hostname: String::new(),
      factory_hostname: String::new(),
      rx_channels: Vec::new(),
      tx_channels: Vec::new(),
      bits_per_sample: 0,
      pcm_type: 0,
      latency_ns,
      sample_rate,
      arc_port: 0,
      cmc_port: 0,
      flows_control_port: 0,
      info_request_port: 0,
    }
  }

  #[test]
  fn zero_latency_ns_returns_zero() {
    let device = dummy_device_info(0, 48_000);
    assert_eq!(device.latency_samples(), 0);
  }

  #[test]
  fn zero_sample_rate_returns_zero() {
    let device = dummy_device_info(1_000_000_000, 0);
    assert_eq!(device.latency_samples(), 0);
  }

  #[test]
  fn one_second_at_48000_hz() {
    let device = dummy_device_info(1_000_000_000, 48_000);
    assert_eq!(device.latency_samples(), 48_000);
  }

  #[test]
  fn one_second_at_44100_hz() {
    let device = dummy_device_info(1_000_000_000, 44_100);
    assert_eq!(device.latency_samples(), 44_100);
  }

  #[test]
  fn five_ms_at_48000_hz() {
    let device = dummy_device_info(5_000_000, 48_000);
    assert_eq!(device.latency_samples(), 240);
  }

  #[test]
  fn large_values_no_overflow() {
    let device = dummy_device_info(1_000_000_000, 44100 * 1024);
    let result = device.latency_samples();
    assert!(result <= usize::MAX);
  }
}
