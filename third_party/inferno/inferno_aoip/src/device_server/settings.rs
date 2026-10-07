use std::{
  collections::BTreeMap,
  env,
  net::{IpAddr, Ipv4Addr},
  path::PathBuf,
  sync::{Arc, RwLock},
};

use netdev::mac::MacAddr;

use crate::device_info::{Channel, DeviceInfo};
use crate::protocol::flows_control::PORT as FLOWS_CONTROL_PORT;
use crate::protocol::mcast::INFO_REQUEST_PORT;
use crate::protocol::proto_arc::PORT as ARC_PORT;
use crate::protocol::proto_cmc::PORT as CMC_PORT;

fn create_self_info(
  app_name: &str,
  short_app_name: &str,
  my_ip: Option<Ipv4Addr>,
  settings: &BTreeMap<String, String>,
) -> DeviceInfo {
  // TODO: change expect to non-fatal errors, with current approach an app using ALSA plugin may be crashed for a bening reason

  let interfaces = netdev::get_interfaces();
  let my_ipv4 = my_ip
    .or_else(|| {
      settings.get("BIND_IP").map(|ipstr| {
        ipstr.parse().unwrap_or_else(|_| {
          interfaces
            .iter()
            .find(|iface| &iface.name == ipstr)
            .expect("invalid setting BIND_IP, must contain IP address or network interface name")
            .ipv4
            .get(0)
            .expect("interface specified in BIND_IP has no IPv4 addresses")
            .addr()
        })
      })
    })
    .unwrap_or_else(|| match local_ip_address::local_ip().expect("unknown local IP, cannot continue") {
      IpAddr::V4(a) => a,
      other => panic!("got local IP which is not IPv4: {other:?}"),
    });

  let process_id: u16 =
    settings.get("PROCESS_ID").map(|s| s.parse().expect("PROCESS_ID must be u16")).unwrap_or(0);

  let mut devid = [0u8; 8];
  settings
    .get("DEVICE_ID")
    .map(|idstr| {
      hex::decode_to_slice(idstr, &mut devid).expect("invalid DEVICE_ID, should contain hex data");
    })
    .unwrap_or_else(|| {
      devid[2..6].copy_from_slice(&my_ipv4.octets());
      devid[6..8].copy_from_slice(&process_id.to_be_bytes());
    });

  // TODO make hostname and sample rate configurable from DC
  let friendly_hostname = settings
    .get("NAME")
    .map(|s| if s.len() > 31 { s[0..31].to_owned() } else { s.clone() })
    .unwrap_or_else(|| {
      format!(
        "{} {}",
        if app_name.len() > 22 { &app_name[0..22] } else { &app_name },
        hex::encode(&my_ipv4.octets())
      )
    });
  let short_app_name = if short_app_name.len() > 14 { &short_app_name[0..14] } else { short_app_name };

  let sample_rate = settings
    .get("SAMPLE_RATE")
    .map(|s| s.parse().expect("invalid SAMPLE_RATE, must be integer"))
    .unwrap_or(48000);

  let mut netmask = Ipv4Addr::new(0, 0, 0, 0);
  let mut gateway = Ipv4Addr::new(0, 0, 0, 0);
  let mut mac_address = MacAddr::zero();
  let mut speed = 0;
  for iface in interfaces {
    let mut our_iface = false;
    for network in iface.ipv4 {
      if network.addr() == my_ipv4 {
        netmask = network.netmask();
        our_iface = true;
        break;
      }
    }
    if our_iface {
      speed =
        [iface.transmit_speed.unwrap_or(0), iface.receive_speed.unwrap_or(0)].iter().max().unwrap_or(&0)
          / 1_000_000;
      if let Some(gws) = iface.gateway {
        for gw in gws.ipv4 {
          if (gw.to_bits() & netmask.to_bits()) == (my_ipv4.to_bits() & netmask.to_bits()) {
            gateway = gw;
            break;
          }
        }
      }
      if let Some(mac) = iface.mac_addr {
        mac_address = mac;
      }
      break;
    }
  }

  let latency_ns = settings
    .get("RX_LATENCY_NS")
    .map(|s| s.parse().expect("invalid RX_LATENCY_NS, must be integer"))
    .unwrap_or(10_000_000);

  let mut result = DeviceInfo {
    ip_address: my_ipv4,
    netmask,
    gateway,
    mac_address,
    link_speed: speed.clamp(0, 10000).try_into().unwrap(),

    board_name: "Inferno-AoIP".to_owned(),
    manufacturer: "Inferno-AoIP".to_owned(),
    model_name: app_name.to_owned(),
    factory_device_id: devid,
    process_id,
    vendor_string: "Audinate Dante-compatible".to_owned(),
    factory_hostname: format!("{short_app_name}-{}", hex::encode(devid)),
    friendly_hostname,
    model_number: "_000000000000000b".to_owned(),
    rx_channels: vec![],
    tx_channels: vec![],
    bits_per_sample: 24, // TODO make it configurable
    pcm_type: 0xe,
    latency_ns,
    sample_rate,

    arc_port: ARC_PORT,
    cmc_port: CMC_PORT,
    flows_control_port: FLOWS_CONTROL_PORT,
    info_request_port: INFO_REQUEST_PORT,
  };

  if let Some(altport) = settings.get("ALT_PORT").map(|s| s.parse().expect("ALT_PORT must be u16")) {
    result.arc_port = altport;
    result.cmc_port = altport + 1;
    result.flows_control_port = altport + 2;
    result.info_request_port = altport + 3;
  }

  result
}

#[derive(Clone)] // TODO: this shouldn't need to be clonable, fix the ALSA plugin
pub struct Settings {
  pub self_info: DeviceInfo,
  pub tx_latency_ns: u32,
  pub clock_path: Option<PathBuf>,
  pub use_safe_clock: bool,
  pub tx_source_bit_depth: u8,
}

impl Settings {
  pub fn new(
    app_name: &str,
    short_app_name: &str,
    my_ip: Option<Ipv4Addr>,
    config: &BTreeMap<String, String>,
  ) -> Self {
    // convert all settings keys to upper case:
    let mut config: BTreeMap<String, String> =
      config.clone().into_iter().map(|(k, v)| (k.to_ascii_uppercase(), v)).collect();

    // add settings from env vars if not already set:
    env::vars().for_each(|(env_key, env_value)| {
      if let Some(key) = env_key.strip_prefix("INFERNO_") {
        let key = key.to_ascii_uppercase();
        config.entry(key).or_insert(env_value);
      }
    });
    let self_info = create_self_info(app_name, short_app_name, my_ip, &config);

    let use_safe_clock = config
      .get("USE_SAFE_CLOCK")
      .map(|s| s.parse().expect("invalid USE_SAFE_CLOCK, must be boolean"))
      .unwrap_or(false);
    let tx_source_bit_depth = config
      .get("TX_SOURCE_BIT_DEPTH")
      .map(|s| s.parse::<u8>().expect("invalid TX_SOURCE_BIT_DEPTH, must be one of: 16, 24, 32"))
      .unwrap_or(32);
    assert!(
      matches!(tx_source_bit_depth, 16 | 24 | 32),
      "invalid TX_SOURCE_BIT_DEPTH, must be one of: 16, 24, 32"
    );

    let mut result = Self {
      self_info,
      tx_latency_ns: config
        .get("TX_LATENCY_NS")
        .map(|p| p.parse().expect("invalid TX_LATENCY_NS, must be integer"))
        .unwrap_or(10_000_000),
      clock_path: config.get("CLOCK_PATH").map(|p| p.try_into().unwrap()),
      use_safe_clock,
      tx_source_bit_depth,
    };

    // the following should be harmless, as the application still has the chance to overwrite it
    let rx_count =
      config.get("RX_CHANNELS").map(|s| s.parse().expect("number of channels must be u16")).unwrap_or(2);
    result.make_rx_channels(rx_count);
    let tx_count =
      config.get("TX_CHANNELS").map(|s| s.parse().expect("number of channels must be u16")).unwrap_or(2);
    result.make_tx_channels(tx_count);

    result
  }
  pub fn make_rx_channels(&mut self, count: usize) {
    self.self_info.rx_channels = (1..=count)
      .map(|id| Channel {
        factory_name: format!("{id:02}"),
        friendly_name: Arc::new(RwLock::new(format!("RX {id}"))),
      })
      .collect();
  }
  pub fn make_tx_channels(&mut self, count: usize) {
    self.self_info.tx_channels = (1..=count)
      .map(|id| Channel {
        factory_name: format!("{id:02}"),
        friendly_name: Arc::new(RwLock::new(format!("TX {id}"))),
      })
      .collect();
  }
}
