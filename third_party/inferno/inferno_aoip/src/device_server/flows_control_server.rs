use crate::common::*;
use crate::device_server::flows_tx::FlowInfo;
use itertools::Itertools;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::sync::Arc;
use tokio::sync::Mutex;

use super::flows_tx::{FlowsTransmitter, FPP_MAX, MAX_FLOWS};
use crate::protocol::flows_control::{FlowControlError, FlowHandle};
use crate::{
  byte_utils::{make_u16, read_0term_str_from_buffer},
  device_info::DeviceInfo,
  net_utils::UdpSocketWrapper,
  protocol::req_resp,
};
use tokio::sync::broadcast::Receiver as BroadcastReceiver;

pub async fn run_server(
  self_info: Arc<DeviceInfo>,
  flows_tx: Arc<Mutex<Option<FlowsTransmitter>>>,
  shutdown: BroadcastReceiver<()>,
) {
  let server =
    UdpSocketWrapper::new(Some(self_info.ip_address), self_info.flows_control_port, shutdown).await;
  let mut conn = req_resp::Connection::new(server);
  let mut recv_buff = crate::net_utils::ReceiveBuffer::new();
  while conn.should_work() {
    let request = match conn.recv(&mut recv_buff).await {
      Some(v) => v,
      None => continue,
    };

    if request.opcode2().read() == 0 {
      match request.opcode1().read() {
        0x0100 => {
          // request flow
          // TODO add index sanity checks
          let c = request.content();
          if c.len() <= 16 {
            error!("invalid flow request, too short packet: {}", hex::encode(c));
            continue;
          }
          let hostname_offset = make_u16(c[0], c[1]) as usize;
          let sample_rate = u32::from_be_bytes(c[2..6].try_into().unwrap());
          let bits_per_sample = u32::from_be_bytes(c[6..10].try_into().unwrap());
          let one = make_u16(c[10], c[11]);
          if one != 1 {
            warn!("expecting 1, received {one:#x}");
          }
          let num_channels = make_u16(c[12], c[13]);
          let remote_descr_offset = make_u16(c[14], c[15]) as usize;
          let channel_indices = (0..num_channels as usize)
            .map(|i| {
              let id = make_u16(c[16 + i * 2], c[17 + i * 2]);
              match id {
                0 => None,
                x => Some(x as usize - 1),
              }
            })
            .collect_vec();
          let offset = (16 + num_channels * 2 + 6) as usize;
          let fpp = make_u16(c[offset], c[offset + 1]);
          let rx_flow_name_offset = make_u16(c[offset + 2], c[offset + 3]) as usize;

          let req_bytes = request.into_storage();
          let hostname = read_0term_str_from_buffer(req_bytes, hostname_offset).unwrap().to_owned();
          let rx_flow_name =
            read_0term_str_from_buffer(req_bytes, rx_flow_name_offset).unwrap().to_owned();

          if req_bytes.len() < remote_descr_offset + 8 {
            error!("packet too short: {}", hex::encode(req_bytes));
            continue;
          }

          if req_bytes[remote_descr_offset] != 0x08 || req_bytes[remote_descr_offset + 1] != 0x02 {
            warn!(
              "expected 0x0802, got 0x{:02x}{:02x}",
              req_bytes[remote_descr_offset],
              req_bytes[remote_descr_offset + 1]
            );
          }
          let rx_port = make_u16(req_bytes[remote_descr_offset + 2], req_bytes[remote_descr_offset + 3]);
          let ip_bytes: [u8; 4] =
            req_bytes[remote_descr_offset + 4..remote_descr_offset + 8].try_into().unwrap();
          let rx_ip = Ipv4Addr::from(ip_bytes);

          info!("{hostname} requesting flow {rx_flow_name} of channel indices {channel_indices:?} at {sample_rate}Hz {bits_per_sample}bit {fpp} fpp to {rx_ip}:{rx_port}");
          if channel_indices.iter().flatten().find(|&&chi| chi >= self_info.tx_channels.len()).is_some() {
            error!("too large channel number, returning error");
            conn.respond_with_code(0x0302u16 /* ??? TODO */, &[]).await;
          }
          if sample_rate != self_info.sample_rate {
            error!("sample rate mismatch, returning error");
            conn.respond_with_code(FlowControlError::SampleRateMismatch as u16, &[]).await;
            continue;
          }
          if fpp > FPP_MAX {
            error!("too large fpp, returning error");
            conn.respond_with_code(0x0302u16 /* TODO */, &[]).await;
            continue;
          }
          let flow_info = FlowInfo {
            rx_hostname: hostname.into(),
            rx_flow_name: rx_flow_name.into(),
            dst_addr: rx_ip,
            dst_port: rx_port,
            local_channel_indices: channel_indices,
          };
          let result = flows_tx
            .lock()
            .await
            .as_mut()
            .unwrap()
            .add_flow(flow_info, fpp as usize, (bits_per_sample / 8) as usize, None, false)
            .await;
          match result {
            Ok((flow_index, handle)) => {
              conn.respond(&handle).await;
            }
            Err(e) => {
              error!("adding flow failed: {e:?}");
              conn.respond_with_code(FlowControlError::TooManyTXFlows as u16, &[]).await;
            }
          }
        }
        0x0101 => {
          // stop flow
          let handle = if let Ok(handle) = request.content().try_into() {
            handle
          } else {
            error!("packet too short: {}", hex::encode(request.content()));
            continue;
          };
          if let Ok(flow_index) = flows_tx.lock().await.as_mut().unwrap().remove_flow(handle).await {
            info!("stopped flow {handle:?}");
            conn.respond(&[]).await;
          } else {
            warn!("received stop flow request for unknown handle {handle:?}");
            conn.respond_with_code(FlowControlError::FlowNotFound as u16, &[]).await;
          }
        }
        0x0102 => {
          // update flow
          let c = request.content();
          let handle: FlowHandle = c[0..6].try_into().unwrap();
          let num_channels = make_u16(c[6], c[7]);

          let channel_indices = (0..num_channels as usize)
            .map(|i| {
              let id = make_u16(c[8 + i * 2], c[9 + i * 2]);
              match id {
                0 => None,
                x => Some(x as usize - 1),
              }
            })
            .collect_vec();

          if let Ok(_flow_index) =
            flows_tx.lock().await.as_mut().unwrap().set_channels(handle, channel_indices.clone()).await
          {
            info!("set channels {channel_indices:?} in flow {handle:?}");
            conn.respond(&[]).await;
          } else {
            warn!("received update flow request for unknown handle {handle:?}");
            conn.respond_with_code(FlowControlError::FlowNotFound as u16, &[]).await;
          }
        }

        x => {
          error!("received unknown opcode1 {x:#04x}, content {}", hex::encode(request.content()));
          error!("whole packet: {:?}", hex::encode(request.into_storage()));
        }
      }
    } else {
      error!(
        "received unknown opcode2 {:#04x}, content {}",
        request.opcode2().read(),
        hex::encode(request.content())
      );
      error!("whole packet: {:?}", hex::encode(request.into_storage()));
    }
  }
  flows_tx.lock().await.as_mut().unwrap().shutdown().await;
}
