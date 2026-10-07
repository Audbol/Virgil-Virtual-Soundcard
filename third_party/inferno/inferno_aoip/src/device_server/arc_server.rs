use super::channels_subscriber::ChannelsSubscriber;
use super::saved_settings::SavedChannelsSettings;
use super::tx_multicasts::TransmitMulticasts;
use crate::mdns_client::MdnsClient;
use crate::{byte_utils::*, net_utils};

use super::flows_rx::MAX_FLOWS as MAX_RX_FLOWS;
use super::flows_tx::{FlowInfo as TXFlowInfo, FPP_MAX_ADVERTISED};
use super::flows_tx::{FlowsTransmitter, MAX_CHANNELS_IN_FLOW, MAX_FLOWS as MAX_TX_FLOWS};
use super::mdns_server::DeviceMDNSResponder;
use crate::device_info::DeviceInfo;
use crate::net_utils::UdpSocketWrapper;
use crate::protocol::mcast::make_channel_change_notification;
use crate::protocol::mcast::MulticastMessage;
use crate::protocol::proto_arc::*;
use crate::protocol::req_resp::HEADER_LENGTH;
use crate::protocol::req_resp::{self, CODE_OK};
use crate::state_storage::StateStorage;
use crate::utils::LogAndForget;
use binary_serde::recursive_array::RecursiveArray as _;
use binary_serde::BinarySerde;
use bytebuffer::{ByteBuffer, Endian};
use itertools::Itertools;
use log::{error, info, trace, warn};
use rand::{thread_rng, Rng as _};
use std::net::Ipv4Addr;
use std::sync::RwLock;
use std::{cmp::min, sync::Arc};
use tokio::sync::broadcast::Receiver as BroadcastReceiver;
use tokio::sync::mpsc::Sender;
use tokio::sync::{watch, Mutex};

pub async fn run_server(
  self_info: Arc<DeviceInfo>,
  state_storage: Arc<StateStorage>,
  mdns_server: Arc<DeviceMDNSResponder>,
  mcast: Sender<MulticastMessage>,
  mut channels_sub_rx: watch::Receiver<Option<Arc<ChannelsSubscriber>>>,
  flows_tx: Arc<Mutex<Option<FlowsTransmitter>>>,
  tx_multicasts: Arc<Mutex<Option<TransmitMulticasts>>>,
  shutdown: BroadcastReceiver<()>,
) {
  let mut subscriber = None;
  let mut saved_channels = SavedChannelsSettings::load(state_storage, self_info.clone());
  let server = UdpSocketWrapper::new(Some(self_info.ip_address), self_info.arc_port, shutdown).await;
  let mut conn = req_resp::Connection::new(server);
  let mut recv_buff = net_utils::ReceiveBuffer::new();
  while conn.should_work() {
    let request = match conn.recv(&mut recv_buff).await {
      Some(v) => v,
      None => continue,
    };

    if channels_sub_rx.has_changed().unwrap_or(false) {
      subscriber = channels_sub_rx.borrow_and_update().clone();
    }

    if request.opcode2().read() == 0 {
      match request.opcode1().read() {
        channels_and_flows_count::OPCODE => {
          let total_channels_wtf = self_info.tx_channels.len() + self_info.rx_channels.len(); // ??? not actually total number of channels but in some devices it is
          let response = channels_and_flows_count::Response {
            unknown1_0: 0,
            flags2: channels_and_flows_count::Flags2 {
              supports_tx_channel_rename: true,
              supports_tx_multicast: true,
              ..Default::default()
            },
            tx_channels_count: self_info.tx_channels.len().try_into().unwrap(),
            rx_channels_count: self_info.rx_channels.len().try_into().unwrap(),
            unknown2_4: 4, // or 1
            max_channels_in_flow: MAX_CHANNELS_IN_FLOW
              .min(self_info.tx_channels.len().try_into().unwrap()), // or 8
            unknown4_8: 8,
            max_tx_flows: MAX_TX_FLOWS.try_into().unwrap(),
            max_rx_flows: MAX_RX_FLOWS.try_into().unwrap(),
            unknown5_total_channels: total_channels_wtf.try_into().unwrap(),
            unknown6_1: 1,
            unknown7_1: 1,
            unknown8_0: [0; 6],
          };
          conn.respond_with_struct(CODE_OK, response).await;
        }

        GET_DEVICE_NAME_OPCODE => {
          // device name (used by network-audio-controller)
          let mut buff = ByteBuffer::new();
          buff.write_bytes(self_info.friendly_hostname.as_bytes());
          buff.write_u8(0);
          conn.respond(buff.as_bytes()).await;
        }

        get_device_names::OPCODE => {
          let mut bytes = ByteBuffer::new();
          let strings_offset = HEADER_LENGTH;
          bytes.write_bytes(&[0u8; get_device_names::ResponseHeader::SERIALIZED_SIZE]);
          let friendly_hostname_offset = (bytes.get_wpos() + strings_offset).try_into().unwrap();
          bytes.write_bytes(self_info.friendly_hostname.as_bytes());
          bytes.write_u8(0);
          let factory_hostname_offset = (bytes.get_wpos() + strings_offset).try_into().unwrap();
          bytes.write_bytes(self_info.factory_hostname.as_bytes());
          bytes.write_u8(0);
          let board_name_offset = (bytes.get_wpos() + strings_offset).try_into().unwrap();
          bytes.write_bytes(self_info.board_name.as_bytes());
          bytes.write_u8(0);
          let revision_string_offset = (bytes.get_wpos() + strings_offset).try_into().unwrap();
          bytes.write_bytes(b":705\0");

          let response = get_device_names::ResponseHeader {
            board_name_offset,
            revision_string_offset,
            friendly_hostname_offset1: friendly_hostname_offset,
            factory_hostname_offset,
            friendly_hostname_offset2: friendly_hostname_offset,
            start_code: 0x2729,
            unknown_opcode_1102: 0x1102,
            ..Default::default()
          };
          bytes.set_wpos(0);
          bytes.write_bytes(response.binary_serialize_to_array(binary_serde::Endianness::Big).as_slice());
          conn.respond(&bytes.as_bytes()).await;
        }

        get_receive_channels::OPCODE => {
          // Dante Receivers names and subscriptions:

          let mut common_descriptor_offset: u16 = 0;
          paginate_respond(
            &mut conn,
            request.content(),
            if subscriber.is_some() {
              self_info.rx_channels.len().min(32).try_into().unwrap()
            } else {
              0
            },
            self_info.rx_channels.iter().enumerate(),
            |(channel_index, ch), bytes| {
              if common_descriptor_offset == 0 {
                let descr = CommonChannelsDescriptor::new(&self_info);
                common_descriptor_offset = bytes.get_wpos().try_into().unwrap();
                bytes
                  .write_bytes(descr.binary_serialize_to_array(binary_serde::Endianness::Big).as_slice());
              }
              let status = subscriber.as_ref().unwrap().channel_status(channel_index);
              let (tx_channel_name_offset, tx_hostname_offset) = match &status {
                None => (0, 0),
                Some(status) => (
                  write_0term_str_to_bytebuffer(bytes, &status.tx_channel_name),
                  write_0term_str_to_bytebuffer(bytes, &status.tx_hostname),
                ),
              };
              let status_value: u32 = match &status {
                None => 0,
                Some(ss) => ss.status as u32,
              };
              Some(get_receive_channels::ChannelDescriptor {
                channel_id: (channel_index + 1).try_into().unwrap(),
                unknown1_6: 6,
                common_descriptor_offset,
                tx_channel_name_offset,
                tx_hostname_offset,
                friendly_name_offset: write_0term_str_to_bytebuffer(
                  bytes,
                  &ch.friendly_name.read().unwrap(),
                ),
                subscription_status: status_value,
                unknown2_0: 0,
              })
            },
          )
          .await;
        }

        get_transmit_channels::OPCODE => {
          // Dante Transmitters default names:
          let mut common_descriptor_offset: u16 = 0;
          paginate_respond(
            &mut conn,
            request.content(),
            self_info.tx_channels.len().min(32).try_into().unwrap(),
            self_info.tx_channels.iter().enumerate(),
            |(channel_index, ch), bytes| {
              if common_descriptor_offset == 0 {
                let descr = CommonChannelsDescriptor::new(&self_info);
                common_descriptor_offset = bytes.get_wpos().try_into().unwrap();
                bytes
                  .write_bytes(descr.binary_serialize_to_array(binary_serde::Endianness::Big).as_slice());
              }
              Some(get_transmit_channels::ChannelDescriptor {
                channel_id: (channel_index + 1).try_into().unwrap(),
                unknown1_7: 7,
                common_descriptor_offset,
                name_offset: write_0term_str_to_bytebuffer(bytes, &ch.factory_name),
              })
            },
          )
          .await;
        }

        get_transmit_channels_friendly_names::OPCODE => {
          // Dante Transmitters user-specified names:
          let mut wrote = false;
          paginate_respond(
            &mut conn,
            request.content(),
            self_info.tx_channels.len().min(32).try_into().unwrap(),
            self_info.tx_channels.iter().enumerate(),
            |(channel_index, ch), bytes| {
              if !wrote {
                bytes.write_u32(0);
                wrote = true;
              }
              let channel_id = (channel_index + 1).try_into().unwrap();
              Some(get_transmit_channels_friendly_names::ChannelDescriptor {
                channel_id_1: channel_id,
                channel_id_2: channel_id,
                friendly_name_offset: write_0term_str_to_bytebuffer(
                  bytes,
                  &ch.friendly_name.read().unwrap(),
                ),
              })
            },
          )
          .await;
        }

        rename_tx_channels::OPCODE => {
          let content = request.content();
          let mut renamed_ids = deserialize_items::<rename_tx_channels::SingleChannelRenameRequest>(
            content,
          )
          .filter_map(|rename| {
            let channel_id = rename.channel_id;
            let name_offset = rename.new_name_offset.saturating_sub(HEADER_LENGTH as _) as usize;
            if channel_id == 0 || name_offset == 0 {
              return None;
            }
            match read_0term_str_from_buffer(content, name_offset) {
              Ok(new_name) => {
                let index = (channel_id - 1) as usize;
                if index < self_info.tx_channels.len() {
                  info!("renaming TX channel id {channel_id} to {new_name}");
                  mdns_server.remove_tx_channel(index);
                  saved_channels.rename_tx_channel(index, new_name.to_owned());
                  mdns_server.add_tx_channel(index);
                  Some(channel_id)
                } else {
                  error!("got rename TX channel request with invalid channel number {channel_id}");
                  None
                }
              }
              Err(e) => {
                error!("could not read new channel name from packet: {e:?}");
                None
              }
            }
          });
          let renamed_anything = renamed_ids.next().is_some();
          renamed_ids.for_each(drop); // consume the whole iterator

          if renamed_anything {
            conn.respond_with_code(1, &[0, 0]).await;
            // sometimes it is [0, 1, 0, 0, H(channel_id), L(channel_id)], but it doesn't look necessary
          } else {
            conn.respond_with_code(0xFFFF /* TODO: really? */, &[]).await;
          }
        }
        rename_rx_channels::OPCODE => {
          let content = request.content();
          let mut renamed_any = false;
          let renamed_indices = deserialize_items::<rename_rx_channels::SingleChannelRenameRequest>(
            content,
          )
          .filter_map(|rename| {
            let channel_id = rename.channel_id;
            let name_offset = rename.new_name_offset.saturating_sub(HEADER_LENGTH as _) as usize;
            if channel_id == 0 || name_offset == 0 {
              return None;
            }
            match read_0term_str_from_buffer(content, name_offset) {
              Ok(new_name) => {
                let index = (channel_id - 1) as usize;
                if index < self_info.rx_channels.len() {
                  info!("renaming RX channel id {channel_id} to {new_name}");
                  saved_channels.rename_rx_channel(index, new_name.to_owned());
                  renamed_any = true;
                  Some(index)
                } else {
                  error!("got rename RX channel request with invalid channel number {channel_id}");
                  None
                }
              }
              Err(e) => {
                error!("could not read new channel name from packet: {e:?}");
                None
              }
            }
          });
          mcast.send(make_channel_change_notification(renamed_indices)).await.log_and_forget();
          conn
            .respond_with_code(
              if renamed_any {
                1
              } else {
                0xFFFF /* TODO */
              },
              &[],
            )
            .await;
        }
        query_tx_flows::OPCODE => {
          // query TX flows
          let content = request.content();
          let (code, response) = paginate_make_response(
            &mut conn,
            content,
            MAX_TX_FLOWS.min(16).try_into().unwrap(),
            flows_tx
              .lock()
              .await
              .as_ref()
              .map(|tx| tx.get_flows_info())
              .unwrap_or(&vec![])
              .iter()
              .enumerate(),
            |(flow_index, flow_opt), bytes| -> Option<u16> {
              flow_opt.as_ref().map(|flow_info| -> u16 {
                let flow_id = flow_index + 1;
                let flow_name = format!("{}_{}", flow_id, self_info.process_id);
                let local_tx_flow_name_offset = write_0term_str_to_bytebuffer(bytes, &flow_name);
                let remote_hostname_offset =
                  write_0term_str_or_0_to_bytebuffer(bytes, flow_info.rx_hostname.as_deref());
                let remote_rx_flow_name_offset =
                  write_0term_str_or_0_to_bytebuffer(bytes, flow_info.rx_flow_name.as_deref());
                let is_multicast = remote_hostname_offset == 0 && remote_rx_flow_name_offset == 0;

                align_wpos(bytes, 4);
                let receiver_socket_descriptor_offset = bytes.get_wpos().try_into().unwrap();
                bytes.write_bytes(
                  DestinationSocketDescriptor {
                    unknown1_8002: 0x8002,
                    port: flow_info.dst_port,
                    addr: flow_info.dst_addr.octets(),
                  }
                  .binary_serialize_to_array(binary_serde::Endianness::Big)
                  .as_slice(),
                );

                let names_descriptor_offset = bytes.get_wpos().try_into().unwrap();
                bytes.write_bytes(
                  query_tx_flows::NamesDescriptor {
                    unknown1_a00: 0xa00,
                    unknown2_1: 1,
                    remote_hostname_offset,
                    remote_rx_flow_name_offset,
                    unknown3_10: 0x10,
                    local_tx_flow_name_offset,
                    ..Default::default()
                  }
                  .binary_serialize_to_array(binary_serde::Endianness::Big)
                  .as_slice(),
                );

                let main_descriptor_offset = bytes.get_wpos();
                bytes.write_bytes(
                  query_tx_flows::FlowDescriptorHeader {
                    flow_id: flow_id.try_into().unwrap(),
                    flow_type: if is_multicast { 2 } else { 0x11 },
                    sample_rate: self_info.sample_rate,
                    unknown1_0: 0,
                    bits_per_sample: self_info.bits_per_sample.into(),
                    unknown2_1: 1,
                    channels_count: flow_info.local_channel_indices.len().try_into().unwrap(),
                    receiver_socket_descriptor_offset,
                  }
                  .binary_serialize_to_array(binary_serde::Endianness::Big)
                  .as_slice(),
                );

                for ch in &flow_info.local_channel_indices {
                  bytes.write_u16(ch.map(|i| i + 1).unwrap_or(0).try_into().unwrap());
                }

                bytes.write_bytes(
                  query_tx_flows::FlowDescriptorFooter { names_descriptor_offset }
                    .binary_serialize_to_array(binary_serde::Endianness::Big)
                    .as_slice(),
                );

                main_descriptor_offset.try_into().unwrap()
              })
            },
          );
          conn.respond_with_code(code, &response).await;
        }
        create_multicast_tx_flow::OPCODE => {
          // Create multicast TX flow
          let content = request.content();
          let mut flow_ids = vec![];
          for descr_offset in deserialize_items::<u16>(content) {
            let descr_offset: usize = descr_offset.into();
            if descr_offset - HEADER_LENGTH
              + create_multicast_tx_flow::FlowDescriptorHeader::SERIALIZED_SIZE
              > content.len()
            {
              continue;
            }
            if let Ok(descr) = create_multicast_tx_flow::FlowDescriptorHeader::binary_deserialize(
              &content[descr_offset - HEADER_LENGTH..]
                [..create_multicast_tx_flow::FlowDescriptorHeader::SERIALIZED_SIZE],
              binary_serde::Endianness::Big,
            ) {
              if descr.flow_type != 2 {
                error!("wanted to create unknown flow type {}", descr.flow_type);
                continue;
              }
              if descr.flow_id == 0 || descr.flow_id as usize > MAX_TX_FLOWS as _ {
                // MAYBE TODO move this check to tx_multicasts
                error!("wanted to create multicast tx flow with invalid flow id: {}", descr.flow_id);
                continue;
              }
              let flow_index = (descr.flow_id as usize) - 1;
              {
                let mut flows_tx_opt = flows_tx.lock().await;
                let flows_tx = if let Some(flows_tx) = flows_tx_opt.as_mut() {
                  flows_tx
                } else {
                  error!("trying to create multicast tx flow but we have no flows transmitter active");
                  continue;
                };
                if flows_tx.get_flows_info()[flow_index].is_some() {
                  // TODO move this check to tx_multicasts or flows_tx
                  error!("tx flow id busy: {}", descr.flow_id);
                  continue;
                }
              }
              let after_header = &content[descr_offset - HEADER_LENGTH
                + create_multicast_tx_flow::FlowDescriptorHeader::SERIALIZED_SIZE..];
              if after_header.len()
                < (descr.channels_count as usize)
                  + create_multicast_tx_flow::FlowDescriptorFooter::SERIALIZED_SIZE
              {
                error!("multicast tx flow descriptor parse failed, too short channels list or footer");
                continue;
              }
              let channels_bytes = &after_header[..(descr.channels_count as usize) * 2];
              let channel_indices = channels_bytes
                .chunks_exact(2)
                .map(|chunk| u16::from_be_bytes(chunk.try_into().unwrap()))
                .map(|id| if id > 0 { Some((id - 1).try_into().unwrap()) } else { None })
                .collect_vec();

              if let Some(txm) = tx_multicasts.lock().await.as_ref() {
                txm.add_flow(flow_index, channel_indices).await;
              } else {
                error!("tx_multicasts None but got add multicast request");
                continue;
              }
              flow_ids.push(flow_index + 1);
            } else {
              error!("failed to parse multicast tx flow descriptor");
              continue;
            }
          }
          if flow_ids.len() > 0 {
            let mut response = ByteBuffer::new();
            response.write_u16(flow_ids.len().try_into().unwrap());
            response.write_u16(0);
            for id in flow_ids {
              response.write_u16(id.try_into().unwrap());
            }
            conn.respond(response.as_bytes()).await;
          } else {
            conn.respond_with_code(0xFFFF /* TODO */, &[]).await;
          }
        }
        delete_multicast_tx_flow::OPCODE => {
          let content = request.content();
          let count = make_u16(content[0], content[1]).try_into().unwrap();
          let flow_indices = content[4..]
            .chunks_exact(2)
            .map(|chunk| u16::from_be_bytes(chunk.try_into().unwrap()))
            .take(count)
            .filter_map(|id| if id > 0 { Some((id as usize) - 1) } else { None });

          let mut deleted_any = false;

          for flow_index in flow_indices {
            if let Some(txm) = tx_multicasts.lock().await.as_ref() {
              txm
                .remove_flow(flow_index)
                .await
                .map(|()| {
                  deleted_any = true;
                  info!("deleted multicast tx flow id {}", flow_index + 1);
                })
                .log_and_forget();
            } else {
              error!("tx_multicasts None but got delete multicast request");
              continue;
            }
          }
          if deleted_any {
            conn.respond(&[]).await;
          } else {
            conn.respond_with_code(0xFFFF /* TODO */, &[]).await;
          }
        }

        0x2320 => {
          // ???
          conn.respond_with_code(0x30, &[]).await;
        }

        query_rx_flows::OPCODE => {
          // query RX flows
          let content = request.content();
          let (code, response) = if let Some(chsub) = subscriber.as_ref() {
            paginate_make_response(
              &mut conn,
              content,
              MAX_RX_FLOWS.min(16).try_into().unwrap(),
              chsub.flows_info().read().unwrap().iter().enumerate(),
              |(flow_index, flow_opt), bytes| -> Option<u16> {
                flow_opt.as_ref().map(|flow_info| -> u16 {
                  align_wpos(bytes, 4);
                  let receiver_socket_descriptor_offset = bytes.get_wpos().try_into().unwrap();
                  bytes.write_bytes(
                    DestinationSocketDescriptor {
                      unknown1_8002: 0x8002,
                      port: flow_info.rx_port,
                      addr: self_info.ip_address.octets(),
                    }
                    .binary_serialize_to_array(binary_serde::Endianness::Big)
                    .as_slice(),
                  );

                  let descriptor2_offset = bytes.get_wpos().try_into().unwrap();
                  bytes.write_bytes(
                    query_rx_flows::Descriptor2 {
                      unknown1_9: 9,
                      unknown2_1: 1,
                      unknown3_800: 0x800,
                      unknown4_0: 0,
                      latency_ns: (flow_info.latency_samples as u64 * 1_000_000_000u64
                        / self_info.sample_rate as u64)
                        .try_into()
                        .unwrap(),
                      unknown5_0: 0,
                    }
                    .binary_serialize_to_array(binary_serde::Endianness::Big)
                    .as_slice(),
                  );

                  let req_bits_in_mask =
                    flow_info.channels_map.iter().map(|bv| bv.len()).max().unwrap_or(0);
                  let words_per_bitmask = ((req_bits_in_mask + 15) / 16).max(1);

                  let bitmask_offsets = flow_info
                    .channels_map
                    .iter()
                    .map(|mask| {
                      let mut chi = 0;
                      let pos = bytes.get_wpos();
                      for _ in 0..words_per_bitmask {
                        let mut word: u16 = 0;
                        let mut single_bit = 1;
                        while single_bit != 0 {
                          word |= if mask.get(chi).unwrap_or(false) { single_bit } else { 0 };
                          chi += 1;
                          single_bit <<= 1;
                        }
                        bytes.write_u16(word);
                      }
                      pos
                    })
                    .collect_vec();

                  align_wpos(bytes, 4);
                  let main_descriptor_offset = bytes.get_wpos();
                  bytes.write_bytes(
                    query_rx_flows::FlowDescriptorHeader {
                      flow_id: (flow_index + 1).try_into().unwrap(),
                      unknown1_1: 1,
                      sample_rate: self_info.sample_rate,
                      unknown2_0: 0,
                      bits_per_sample: self_info.bits_per_sample.into(),
                      unknown3_1: 1,
                      channels_count: flow_info.channels_map.len().try_into().unwrap(),
                      words_per_bitmask: words_per_bitmask.try_into().unwrap(),
                      receiver_socket_descriptor_offset,
                    }
                    .binary_serialize_to_array(binary_serde::Endianness::Big)
                    .as_slice(),
                  );
                  for pos in bitmask_offsets {
                    bytes.write_u16(pos.try_into().unwrap());
                  }
                  bytes.write_bytes(
                    query_rx_flows::FlowDescriptorFooter { descriptor2_offset }
                      .binary_serialize_to_array(binary_serde::Endianness::Big)
                      .as_slice(),
                  );

                  main_descriptor_offset.try_into().unwrap()
                })
              },
            )
          } else {
            (1, vec![])
          };
          conn.respond_with_code(code, &response).await;
        }

        0x1100 => {
          // used by DC
          // received unknown opcode1 0x1100, content 00130201820482050210021182188219830183028306031003110303802100f08060002200630064
          // whole packet: "272900320e621100000000130201820482050210021182188219830183028306031003110303802100f08060002200630064"

          // ???
          // looks like something dependent on active connections
          let content = [0u8; 110];
          // XXX: not necessary
          /* let content = [
            0x12, 0x12, 0x02, 0x01, 0x00, 0x01, 0x82, 0x04, 0x00, 0x54, 0x82, 0x05, 0x00, 0x58,
            0x02, 0x10, 0x00, 0x10, 0x02, 0x11, 0x00, 0x10, 0x00, 0x00, 0x82, 0x18, 0x00, 0x00,
            0x82, 0x19, 0x83, 0x01, 0x00, 0x5c, 0x83, 0x02, 0x00, 0x60, 0x83, 0x06, 0x00, 0x64,
            0x03, 0x10, 0x00, 0x10, 0x03, 0x11, 0x00, 0x10, 0x03, 0x03, 0x00, 0x02, 0x80, 0x21,
            0x00, 0x68, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x80, 0x60, 0x00, 0x22, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x63, /* 1000000: */ 0x00, 0x0f, 0x42, 0x40, 0x00, 0x0f, 0x42,
            0x40, 0x00, 0x0f, 0x42, 0x40, 0x01, 0x35, 0xf1, 0xb4, 0x00, 0x0f, 0x42, 0x40, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00,
          ]; */
          conn.respond(&content).await;
        }
        0x1102 => {
          // identical for all low channels count devices
          let content = [0u8; 94];
          // XXX not necessary
          /* let content = [
            /* number of items, 2B: */ 0x00, 0x17, 0x80, 0x20, 0x00, 0x01,
            0x80, 0x21, 0x00, 0x03, 0x00, 0x22, 0x00, 0x03, 0x00, 0x23, 0x00, 0x03, 0x00, 0x24, 0x00, 0x01,
            0x02, 0x01, 0x00, 0x03, 0x82, 0x04, 0x00, 0x03, 0x82, 0x05, 0x00, 0x03, 0x02, 0x0a, 0x00, 0x01,
            0x02, 0x0b, 0x00, 0x01, 0x02, 0x10, 0x00, 0x03, 0x02, 0x11, 0x00, 0x03, 0x02, 0x12, 0x00, 0x03,
            0x02, 0x13, 0x00, 0x01, 0x02, 0x14, 0x00, 0x01, 0x83, 0x01, 0x00, 0x03, 0x83, 0x06, 0x00, 0x01,
            0x83, 0x02, 0x00, 0x01, 0x03, 0x10, 0x00, 0x01, 0x03, 0x11, 0x00, 0x01, 0x03, 0x03, 0x00, 0x03,
            0x83, 0xf0, 0x00, 0x01, 0x06, 0x01, 0x00, 0x01
          ]; */
          conn.respond(&content).await;
        }
        0x3300 => {
          // WTF: this is necessary to avoid 'clock domain mismatch' error in DC
          conn.respond(&[0x38, 0x00, 0x38, 0xfd, 0x38, 0xfe, 0x38, 0xff]).await;
          //conn.respond(&[0u8; 8]).await;
        }

        set_channels_subscriptions::OPCODE => {
          // subscribe (connect our receiver to remote transmitter)
          // or unsubscribe if tx_*_offset is 0
          if let Some(channels_recv) = &subscriber {
            let c_whole = request.content();
            for req in
              deserialize_items::<set_channels_subscriptions::SingleChannelSubscriptionRequest>(c_whole)
            {
              if req.local_channel_id == 0 {
                continue;
              }
              let local_channel_index = (req.local_channel_id - 1).try_into().unwrap();
              if local_channel_index >= self_info.rx_channels.len() {
                error!("got connect/disconnect request for nonexisting channel {}", req.local_channel_id);
                continue;
              }
              if req.tx_channel_name_offset > 0 && req.tx_hostname_offset > 0 {
                let str_or_none = |offset: u16| match offset {
                  _ if offset < (HEADER_LENGTH as _) => None,
                  v => match read_0term_str_from_buffer(&c_whole, v as usize - HEADER_LENGTH) {
                    Ok(s) => Some(s),
                    Err(e) => {
                      error!("failed to decode string: {e:?}");
                      None
                    }
                  },
                };
                let tx_channel_name = str_or_none(req.tx_channel_name_offset);
                let tx_hostname = str_or_none(req.tx_hostname_offset);
                info!(
                  "connection requested: {} <- {:?} @ {:?}",
                  req.local_channel_id, tx_channel_name, tx_hostname
                );
                if tx_channel_name.is_some() && tx_hostname.is_some() {
                  channels_recv
                    .subscribe(local_channel_index, tx_channel_name.unwrap(), tx_hostname.unwrap())
                    .await;
                } else {
                  error!("couldn't read tx names from subscription request: {}", hex::encode(&c_whole));
                }
              } else {
                info!("disconnect requested: local channel {}", req.local_channel_id);
                channels_recv.unsubscribe(local_channel_index).await;
              }
            }
            conn.respond(&[]).await;
          }
        }

        0x3014 => {
          // netaudio subscription remove (used by network-audio-controller)
          // received unknown opcode1 0x3014, content 000100000002
          // whole packet: "27ff00104a1c30140000000100000002"
          if let Some(channels_recv) = &subscriber {
            let content = request.content();
            let local_channel = make_u16(content[4], content[5]);
            let local_channel_index = (local_channel - 1) as usize;
            info!("disconnect requested: local channel {}", local_channel);
            channels_recv.unsubscribe(local_channel_index).await;
            conn.respond(&[]).await;
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
}
