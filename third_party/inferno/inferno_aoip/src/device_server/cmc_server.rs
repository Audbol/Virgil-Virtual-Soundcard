use crate::common::*;
use crate::protocol::req_resp::CODE_OK;
use std::sync::Arc;

use crate::device_info::DeviceInfo;
use crate::net_utils::UdpSocketWrapper;
use crate::protocol::proto_cmc::*;
use crate::protocol::req_resp;
use bytebuffer::ByteBuffer;
use tokio::sync::broadcast::Receiver as BroadcastReceiver;

pub async fn run_server(self_info: Arc<DeviceInfo>, shutdown: BroadcastReceiver<()>) {
  let server = UdpSocketWrapper::new(Some(self_info.ip_address), self_info.cmc_port, shutdown).await;
  let mut conn = req_resp::Connection::new(server);
  let mut recv_buff = crate::net_utils::ReceiveBuffer::new();
  while conn.should_work() {
    let request = match conn.recv(&mut recv_buff).await {
      Some(v) => v,
      None => continue,
    };

    if request.opcode2().read() == 0 {
      match request.opcode1().read() {
        REQUEST_DEVICE_ADVERTISEMENT => {
          let adv = DeviceAdvertisement {
            process_id: self_info.process_id,
            factory_device_id: self_info.factory_device_id,
            unknown1_1: 1,
            unknown2_0: 0,
            ip_address: self_info.ip_address.octets(),
            info_request_port: self_info.info_request_port,
            unknown3_0: 0,
          };
          conn.respond_with_struct(CODE_OK, adv).await;
        }
        other => {
          error!("received unknown opcode1 {other:#04x}, content {}", hex::encode(request.content()));
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
}
