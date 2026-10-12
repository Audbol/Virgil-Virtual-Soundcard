use log::{error, info};

use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use tokio::{net::UdpSocket, select, sync::broadcast::Receiver};

pub const MTU: usize = 1500;
const PACKET_BUFFER_SIZE: usize = MTU;
pub const MAX_PAYLOAD_BYTES: usize = 1400; // ???

pub struct ReceiveBuffer {
  buff: [u8; PACKET_BUFFER_SIZE],
}

impl ReceiveBuffer {
  pub fn new() -> Self {
    Self { buff: [0u8; PACKET_BUFFER_SIZE] }
  }
}

pub struct UdpSocketWrapper {
  socket: Option<UdpSocket>,
  listen_addr: Ipv4Addr,
  listen_port: u16,
  shutdown: Receiver<()>,
  dowork: bool,
  recv_buff: [u8; PACKET_BUFFER_SIZE],
}

impl UdpSocketWrapper {
  pub async fn new(
    listen_addr: Option<Ipv4Addr>,
    listen_port: u16,
    shutdown: Receiver<()>,
  ) -> UdpSocketWrapper {
    let listen_addr = listen_addr.unwrap_or(Ipv4Addr::new(0, 0, 0, 0));
    let socket_opt =
      UdpSocket::bind(SocketAddr::new(std::net::IpAddr::V4(listen_addr), listen_port)).await;
    let socket = socket_opt.expect("error starting really needed listener");
    // TODO MAY PANIC: this error should be non-fatal because some apps may use Inferno as an optional audio I/O
    let listen_port = socket.local_addr().unwrap().port();
    UdpSocketWrapper {
      listen_addr,
      socket: Some(socket),
      listen_port,
      shutdown,
      dowork: true,
      recv_buff: [0; PACKET_BUFFER_SIZE],
    }
  }

  pub fn should_work(&self) -> bool {
    self.dowork
  }

  pub fn port(&self) -> u16 {
    self.listen_port
  }

  pub async fn recv<'a>(&mut self, recv_buff: &'a mut ReceiveBuffer) -> Option<(SocketAddr, &'a [u8])> {
    let socket = match &self.socket {
      Some(s) => s,
      None => {
        return None;
      }
    };
    select! {
      r = socket.recv_from(&mut recv_buff.buff) => {
        match r {
          Ok((len_recv, src)) => {
            return Some((src, &recv_buff.buff[..len_recv]));
          },
          Err(e) => {
            error!("error receiving from socket: {e:?}");
            return None;
          }
        }
      },
      _ = self.shutdown.recv() => {
        self.dowork = false;
        return None;
      }
    };
  }

  pub async fn send(&self, dst: &SocketAddr, packet: &[u8]) {
    let socket = match &self.socket {
      Some(s) => s,
      None => {
        info!("shutting down, discarding message to send");
        return;
      }
    };
    if let Err(e) = socket.send_to(packet, dst).await {
      error!("send error (ignoring): {e:?}");
    }
  }
}

pub async fn create_tokio_udp_socket(self_ip: Ipv4Addr) -> tokio::io::Result<(UdpSocket, u16)> {
  let socket = UdpSocket::bind(SocketAddr::new(IpAddr::V4(self_ip), 0)).await?;
  let port = socket.local_addr()?.port();
  return Ok((socket, port));
}

pub fn create_mio_udp_socket(self_ip: Ipv4Addr) -> std::io::Result<(mio::net::UdpSocket, u16)> {
  let socket = mio::net::UdpSocket::bind(SocketAddr::new(IpAddr::V4(self_ip), 0))?;
  tune_flow_socket(&socket, Some(FLOW_RX_SOCKET_BUFFER_BYTES));
  let port = socket.local_addr()?.port();
  return Ok((socket, port));
}

// ---- Virgil patch: flow socket options --------------------------------------------

/// Receive buffer requested for audio flow sockets, so a receive thread that
/// is held up for a few milliseconds does not lose packets (the OS may cap it,
/// e.g. Linux at net.core.rmem_max).
pub const FLOW_RX_SOCKET_BUFFER_BYTES: usize = 2 * 1024 * 1024;

#[cfg(unix)]
mod sockopt {
  use std::os::raw::{c_int, c_void};
  #[cfg(any(target_os = "linux", target_os = "android"))]
  const SOL_SOCKET: c_int = 1;
  #[cfg(any(target_os = "linux", target_os = "android"))]
  const SO_RCVBUF: c_int = 8;
  #[cfg(not(any(target_os = "linux", target_os = "android")))]
  const SOL_SOCKET: c_int = 0xffff;
  #[cfg(not(any(target_os = "linux", target_os = "android")))]
  const SO_RCVBUF: c_int = 0x1002;
  extern "C" {
    fn setsockopt(fd: c_int, level: c_int, name: c_int, value: *const c_void, len: u32) -> c_int;
  }
  pub fn set_rcvbuf(fd: c_int, bytes: usize) -> bool {
    let v: c_int = bytes.min(c_int::MAX as usize) as c_int;
    unsafe {
      setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v as *const c_int as *const c_void, std::mem::size_of::<c_int>() as u32)
        == 0
    }
  }
}

#[cfg(windows)]
mod sockopt {
  use std::os::raw::{c_char, c_int, c_void};
  const SOL_SOCKET: c_int = 0xffff;
  const SO_RCVBUF: c_int = 0x1002;
  // _WSAIOW(IOC_VENDOR, 12) and _WSAIOW(IOC_VENDOR, 15)
  const SIO_UDP_CONNRESET: u32 = 0x9800_000C;
  const SIO_UDP_NETRESET: u32 = 0x9800_000F;
  #[link(name = "ws2_32")]
  extern "system" {
    fn setsockopt(s: usize, level: c_int, name: c_int, value: *const c_char, len: c_int) -> c_int;
    fn WSAIoctl(
      s: usize,
      code: u32,
      in_buf: *const c_void,
      in_len: u32,
      out_buf: *mut c_void,
      out_len: u32,
      bytes_returned: *mut u32,
      overlapped: *mut c_void,
      completion: *const c_void,
    ) -> c_int;
  }
  pub fn set_rcvbuf(s: usize, bytes: usize) -> bool {
    let v: c_int = bytes.min(c_int::MAX as usize) as c_int;
    unsafe {
      setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v as *const c_int as *const c_char, std::mem::size_of::<c_int>() as c_int)
        == 0
    }
  }
  /// Without this, an ICMP "port unreachable" (e.g. answering a keepalive
  /// sent to a transmitter that has just restarted) makes the next receive
  /// on the socket fail with WSAECONNRESET instead of delivering packets.
  pub fn disable_udp_resets(s: usize) -> bool {
    let off: i32 = 0; // FALSE
    let mut ok = true;
    for code in [SIO_UDP_CONNRESET, SIO_UDP_NETRESET] {
      let mut returned = 0u32;
      let r = unsafe {
        WSAIoctl(
          s,
          code,
          &off as *const i32 as *const c_void,
          std::mem::size_of::<i32>() as u32,
          std::ptr::null_mut(),
          0,
          &mut returned,
          std::ptr::null_mut(),
          std::ptr::null(),
        )
      };
      ok &= r == 0;
    }
    ok
  }
}

/// Options for audio flow sockets (best effort, failures are only logged at
/// debug level): the receive buffer size if given, and on Windows no
/// connection-reset / network-reset errors on UDP receives.
#[cfg(unix)]
pub fn tune_flow_socket<S: std::os::fd::AsRawFd>(socket: &S, rcvbuf_bytes: Option<usize>) {
  if let Some(bytes) = rcvbuf_bytes {
    if !sockopt::set_rcvbuf(socket.as_raw_fd(), bytes) {
      log::debug!("could not set SO_RCVBUF to {bytes} on a flow socket");
    }
  }
}

/// Options for audio flow sockets (best effort, failures are only logged at
/// debug level): the receive buffer size if given, and on Windows no
/// connection-reset / network-reset errors on UDP receives.
#[cfg(windows)]
pub fn tune_flow_socket<S: std::os::windows::io::AsRawSocket>(socket: &S, rcvbuf_bytes: Option<usize>) {
  let raw = socket.as_raw_socket() as usize;
  if let Some(bytes) = rcvbuf_bytes {
    if !sockopt::set_rcvbuf(raw, bytes) {
      log::debug!("could not set SO_RCVBUF to {bytes} on a flow socket");
    }
  }
  if !sockopt::disable_udp_resets(raw) {
    log::debug!("could not disable SIO_UDP_CONNRESET/NETRESET on a flow socket");
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn flow_socket_gets_bigger_receive_buffer() {
    let (socket, _) = create_mio_udp_socket(Ipv4Addr::LOCALHOST).unwrap();
    // just must not fail or panic; the size actually granted depends on the OS
    tune_flow_socket(&socket, Some(FLOW_RX_SOCKET_BUFFER_BYTES));
  }
}
