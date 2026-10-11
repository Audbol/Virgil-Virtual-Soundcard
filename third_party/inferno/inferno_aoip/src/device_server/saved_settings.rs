use std::sync::{Arc, Mutex};

use log::error;
use serde::{Deserialize, Serialize};

use crate::{
  device_info::{self, DeviceInfo},
  state_storage::StateStorage,
  utils::LogAndForget,
};

#[derive(Deserialize, Serialize, Default)]
pub struct ChannelSettings {
  id: usize,
  friendly_name: String,
}

#[derive(Deserialize, Serialize)]
struct SavedChannels {
  channels: Vec<ChannelSettings>,
}

// Virgil patch: channel names owned by the embedding application. Names set
// here win over the saved ones (and are saved); renames from the network are
// reported to the hook; the current names can be read back at any time.
static OVERRIDES: Mutex<(Vec<Option<String>>, Vec<Option<String>>)> = Mutex::new((Vec::new(), Vec::new()));
static CURRENT: Mutex<(Vec<String>, Vec<String>)> = Mutex::new((Vec::new(), Vec::new()));
pub type RenameHook = fn(is_tx: bool, index: usize, name: &str);
static HOOK: Mutex<Option<RenameHook>> = Mutex::new(None);

/// Virgil patch: whether a channel name from the network can be used. Longer
/// names made the mDNS registration panic ("name@device" is one DNS label of
/// at most 63 bytes) on every start once saved; control characters and the
/// characters Dante uses as separators are refused too (same rules as
/// Virgil's configuration).
pub fn valid_channel_name(name: &str) -> bool {
  !name.is_empty()
    && name.len() <= 31
    && !name.starts_with(' ')
    && !name.ends_with(' ')
    && name.bytes().all(|b| (0x20..=0x7e).contains(&b) && !b"=.@\"\\".contains(&b))
}

/// Names for the next device start: `None` keeps the saved (or default) name.
pub fn set_name_overrides(tx: Vec<Option<String>>, rx: Vec<Option<String>>) {
  *OVERRIDES.lock().unwrap() = (tx, rx);
}
pub fn set_rename_hook(hook: Option<RenameHook>) {
  *HOOK.lock().unwrap() = hook;
}
/// Current name of a channel of the running device, if known.
pub fn current_name(is_tx: bool, index: usize) -> Option<String> {
  let c = CURRENT.lock().unwrap();
  (if is_tx { &c.0 } else { &c.1 }).get(index).cloned()
}
pub(crate) fn name_override(is_tx: bool, index: usize) -> Option<String> {
  let o = OVERRIDES.lock().unwrap();
  (if is_tx { &o.0 } else { &o.1 }).get(index).cloned().flatten()
}
fn publish(is_tx: bool, channels: &Vec<device_info::Channel>) {
  let names: Vec<String> = channels.iter().map(|c| c.friendly_name.read().unwrap().clone()).collect();
  let mut c = CURRENT.lock().unwrap();
  if is_tx {
    c.0 = names;
  } else {
    c.1 = names;
  }
}

pub struct SavedChannelsSettings {
  state_storage: Arc<StateStorage>,
  self_info: Arc<DeviceInfo>,
  rx_channels: SavedChannels,
  tx_channels: SavedChannels,
}

impl SavedChannelsSettings {
  pub fn load(state_storage: Arc<StateStorage>, self_info: Arc<DeviceInfo>) -> Self {
    let mut r = Self {
      rx_channels: state_storage.load("rx_channels").unwrap_or(SavedChannels { channels: vec![] }),
      tx_channels: state_storage.load("tx_channels").unwrap_or(SavedChannels { channels: vec![] }),
      state_storage,
      self_info,
    };
    Self::load_and_init(&mut r.rx_channels.channels, &r.self_info.rx_channels, false);
    Self::load_and_init(&mut r.tx_channels.channels, &r.self_info.tx_channels, true);
    // Names set by the application are saved as if renamed from the network.
    let (mut tx_changed, mut rx_changed) = (false, false);
    for i in 0..r.self_info.tx_channels.len() {
      if let Some(n) = name_override(true, i) {
        tx_changed |= r.tx_channels.channels[i].friendly_name != n;
        Self::rename(&mut r.tx_channels.channels, &r.self_info.tx_channels, i, n);
      }
    }
    for i in 0..r.self_info.rx_channels.len() {
      if let Some(n) = name_override(false, i) {
        rx_changed |= r.rx_channels.channels[i].friendly_name != n;
        Self::rename(&mut r.rx_channels.channels, &r.self_info.rx_channels, i, n);
      }
    }
    if tx_changed {
      r.state_storage.save("tx_channels", &r.tx_channels).log_and_forget();
    }
    if rx_changed {
      r.state_storage.save("rx_channels", &r.rx_channels).log_and_forget();
    }
    publish(true, &r.self_info.tx_channels);
    publish(false, &r.self_info.rx_channels);
    r
  }
  fn load_and_init(src: &mut Vec<ChannelSettings>, dst: &Vec<device_info::Channel>, is_tx: bool) {
    for (index, cs) in src.iter().enumerate().take(dst.len()) {
      if cs.id == 0
        || cs.friendly_name.is_empty()
        || !valid_channel_name(&cs.friendly_name)
        || name_override(is_tx, index).is_some()
      {
        continue;
      }
      if index != (cs.id - 1) {
        error!("corrupted saved channels: id {}", cs.id);
        continue;
      }
      *dst[index].friendly_name.write().unwrap() = cs.friendly_name.clone();
    }
    if dst.len() > src.len() {
      src.resize_with(dst.len(), || Default::default());
    };
  }
  fn rename(
    local: &mut Vec<ChannelSettings>,
    device_channels: &Vec<device_info::Channel>,
    index: usize,
    name: String,
  ) {
    local[index].friendly_name = name.clone();
    local[index].id = index + 1;
    *device_channels[index].friendly_name.write().unwrap() = name;
  }
  pub fn rename_rx_channel(&mut self, index: usize, name: String) {
    Self::rename(&mut self.rx_channels.channels, &self.self_info.rx_channels, index, name.clone());
    self.state_storage.save("rx_channels", &self.rx_channels).log_and_forget();
    publish(false, &self.self_info.rx_channels);
    let hook = *HOOK.lock().unwrap();  // not called under the lock
    if let Some(h) = hook {
      h(false, index, &name);
    }
  }
  pub fn rename_tx_channel(&mut self, index: usize, name: String) {
    Self::rename(&mut self.tx_channels.channels, &self.self_info.tx_channels, index, name.clone());
    self.state_storage.save("tx_channels", &self.tx_channels).log_and_forget();
    publish(true, &self.self_info.tx_channels);
    let hook = *HOOK.lock().unwrap();  // not called under the lock
    if let Some(h) = hook {
      h(true, index, &name);
    }
  }
}
