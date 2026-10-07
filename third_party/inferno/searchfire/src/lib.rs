//! <h1 align="center">🔥 Searchfire - <a href="https://github.com/WilliamVenner/searchlight">Searchlight</a> modified for the needs of <a href="https://gitlab.com/lumifaza/inferno">Inferno</a></h1>

//!
//! <h1 align="center">📡 Searchlight</h1>
//!
//! Searchlight is an mDNS server & client library designed to be simple, lightweight and easy to use,
//! even if you just have basic knowledge about mDNS.
//!
//! In layman's terms, Searchlight is a library for broadcasting and discovering "services" on a local network.
//! This technology is part of the same technology used by Chromecast, AirDrop, Phillips Hue, and et cetera.
//!
//! **Searchlight is designed with user interfaces in mind.**
//! The defining feature of this library is that it keeps track of the presence of services on the network,
//! and notifies you when they come and go, allowing you to update your user interface accordingly,
//! providing a user experience that is responsive, intuitive and familiar to a scanning list for
//! WiFi, Bluetooth, Chromecast, etc.
//!
//! - **🌐 IPv4 and IPv6** - Support for both IPv4 and IPv6.
//! - **✨ OS support** - Support for Windows, macOS and most UNIX systems.
//! - **📡 Broadcasting** - Send out service announcements to the network and respond to discovery requests. (mDNS server)
//! - **👽 Discovery** - Discover services on the network and keep track of their presence. (mDNS client)
//! - **🧵 Single threaded** - Searchlight operates on just a single thread, thanks to the [Tokio](https://tokio.rs/) async runtime & task scheduler.
//! - **🤸 Flexible API** - No async, no streams, no channels, no bullsh*t. Just provide an event handler function and bridge the gap between your application and Searchlight however you like.
//! - **👻 Background runtime** - Discovery and broadcasting can both run in the background on separate threads, providing a handle to gracefully shut down if necessary.
//! - **📨 UDP** - All networking, including discovery and broadcasting, is connectionless and done over UDP.
//! - **🔁 Loopback** - Support for receiving packets sent by the same socket, intended to be used in tests.
//! - **🎯 Interface targeting** - Support for targeting specific network interface(s) for discovery and broadcasting.
//!
//! # Feature flags
//!
//! - **`broadcast` ᵈᵉᶠᵃᵘˡᵗ**<br>Provides the [`Broadcaster`](broadcast::Broadcaster) type that will broadcast [`Service`](broadcast::Service)s on the network and respond to discovery requests.
//!
//! - **`discovery` ᵈᵉᶠᵃᵘˡᵗ**<br>Provides the [`Discovery`](discovery::Discovery) type that will discover [`Responder`](discovery::Responder)s on the network and keep track of their presence, notifying you via [`DiscoveryEvent`](discovery::DiscoveryEvent)s.
//!
//! # Examples
//!
//! Examples for [broadcasting](broadcast) and [discovery] can be found in the documentation for their respective modules.

#![cfg_attr(docsrs, feature(doc_cfg))]
#![warn(missing_docs)]
#![warn(rustdoc::broken_intra_doc_links)]

use std::net::{Ipv4Addr, Ipv6Addr};

#[macro_use]
extern crate thiserror;

#[cfg(test)]
mod tests;

mod socket;
mod util;

pub mod errors;
pub mod net;

#[cfg(feature = "broadcast")]
#[cfg_attr(docsrs, doc(cfg(feature = "broadcast")))]
pub mod broadcast;

#[cfg(feature = "discovery")]
#[cfg_attr(docsrs, doc(cfg(feature = "discovery")))]
pub mod discovery;

/// The port used for mDNS.
pub const MDNS_PORT: u16 = 5353;

/// The IPv4 multicast address used for mDNS.
pub const MDNS_V4_IP: Ipv4Addr = Ipv4Addr::new(224, 0, 0, 251);

/// The IPv6 multicast address used for mDNS.
pub const MDNS_V6_IP: Ipv6Addr = Ipv6Addr::new(0xff02, 0, 0, 0, 0, 0, 0, 0xfb);

/// Searchlight uses [`trust-dns`](https://github.com/bluejekyll/trust-dns) internally for DNS parsing and packet building, so here's a re-export for your convenience.
pub use trust_dns_client as dns;
