// Inferno-AoIP
// Copyright (C) 2023-2025 Teodor Woźniak
//
// You may choose which license to use from the two following.
// Remove the other license if desirable
// (e.g. forking into a project that will benefit from AGPL protection)
//
// 1.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// 2.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

//! Inferno - unofficial implementation of Dante protocol (Audio over IP)
//!
//! This library provides both RX (receive) and TX (transmit) capabilities for
//! audio streaming over IP networks using the Dante protocol.
//!
//! ## Features
//! * receiving audio from and sending audio to Dante devices and virtual devices
//! * works with most features of Dante Controller and [network-audio-controller](https://github.com/chris-ritsen/network-audio-controller) (`netaudio` command line tool)
//!
//! ## External dependencies (not handled by `Cargo.toml`)
//! * PTP clock synchronization daemon - see [README](https://gitlab.com/lumifaza/inferno/-/blob/dev/README.md?ref_type=heads#clocking-options) for details.
//!
//! ## Example: Multi-channel peak level meter
//!
//! ```no_run
//! use inferno_aoip::device_server::{DeviceServer, Settings, Sample};
//!
//! fn audio_callback(samples_count: usize, channels: &Vec<Vec<Sample>>) {
//!   let line = channels.iter().map(|ch| {
//!     let peak = (ch.iter().take(samples_count).map(|samp|
//!       samp.saturating_abs()).max().unwrap_or(0) as f32
//!     ) / (Sample::MAX as f32);
//!     if peak > 0.0 {
//!       let db = 20.0 * peak.log10();
//!       format!("{db:>+6.1} ")
//!     } else {
//!       format!("------ ")
//!     }
//!   }).collect::<String>();
//!   println!("{line}");
//! }
//!
//! #[tokio::main(flavor = "current_thread")]
//! async fn main() {
//!   let logenv = env_logger::Env::default().default_filter_or("debug");
//!   env_logger::init_from_env(logenv);
//!   
//!   let mut settings = Settings::new(
//!     "My Peak Meter",
//!     "PkMeter",
//!     None,
//!     &Default::default()
//!   );
//!   settings.make_rx_channels(8);
//!
//!   let mut server = DeviceServer::start(settings).await;
//!   server.receive_with_callback(Box::new(audio_callback)).await;
//!
//!   tokio::signal::ctrl_c().await.ok();
//!   server.shutdown().await;
//! }
//! ```
//!
//! ## Legal
//! * This project makes no claim to be either authorized or approved by Audinate.
//! * Dual licensed under GPLv3-or-later and AGPLv3-or-later
//!

mod common;
pub mod device_info;
pub mod device_server;
mod mdns_client;
mod media_clock;
mod protocol;
mod ring_buffer;
mod state_storage;
mod util;

pub use util::bytes as byte_utils;
pub use util::net as net_utils;

pub mod utils {
  pub use crate::common::LogAndForget;
  pub use crate::util::os::set_current_thread_realtime;
  pub use crate::util::thread::run_future_in_new_thread;
}
