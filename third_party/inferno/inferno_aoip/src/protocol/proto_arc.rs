use std::marker::PhantomData;

use binary_serde::BinarySerde;
use bytebuffer::ByteBuffer;
use log::error;

use crate::{byte_utils::make_u16, device_info::DeviceInfo};

use super::req_resp::{Connection, HEADER_LENGTH};

pub const PACKET_SIZE_SOFT_LIMIT: usize = 800;
pub const PORT: u16 = 4440;

pub mod channels_and_flows_count {
  use binary_serde::{binary_serde_bitfield, BinarySerde, BitfieldBitOrder};

  pub const OPCODE: u16 = 0x1000;

  #[derive(Debug, Default, PartialEq, Eq)]
  #[binary_serde_bitfield(order = BitfieldBitOrder::LsbFirst)]
  pub struct Flags2 {
    #[bits(4)]
    pub unknown1_0: u8,
    #[bits(1)]
    pub supports_tx_channel_rename: bool,
    #[bits(1)]
    pub supports_tx_multicast: bool,
    #[bits(2)]
    pub unknown2_0: u8,
  }

  #[derive(Debug, BinarySerde, Default, PartialEq, Eq)]
  pub struct Response {
    pub unknown1_0: u8, // or 5
    pub flags2: Flags2,
    pub tx_channels_count: u16,
    pub rx_channels_count: u16,
    pub unknown2_4: u16, // or 1
    pub max_channels_in_flow: u16,
    pub unknown4_8: u16,
    pub max_tx_flows: u16,
    pub max_rx_flows: u16,
    pub unknown5_total_channels: u16,
    pub unknown6_1: u16,
    pub unknown7_1: u16,
    pub unknown8_0: [u16; 6],
  }
}

pub const GET_DEVICE_NAME_OPCODE: u16 = 0x1002;

pub mod get_device_names {
  use binary_serde::BinarySerde;

  pub const OPCODE: u16 = 0x1003;

  #[derive(Debug, BinarySerde, Default)]
  pub struct ResponseHeader {
    pub unknown1_0: u16,
    pub unknown2_0: u16, // was 0x14
    pub unknown3_0: u16, // was 0x20
    pub board_name_offset: u16,
    pub revision_string_offset: u16,
    pub unknown4_0: u16, // was 0x500
    pub friendly_hostname_offset1: u16,
    pub factory_hostname_offset: u16,
    pub friendly_hostname_offset2: u16,
    pub unknown5_0: [u16; 6], // was [0, 0, 4, 0, 4, 0]
    pub start_code: u16,      // 0x2729
    pub unknown6_0: u16,
    pub unknown_opcode_1102: u16,
    pub unknown7_0: u16,
  }
}

#[derive(Debug, BinarySerde, Default)]
pub struct CommonChannelsDescriptor {
  pub sample_rate: u32,
  pub unknown1_1: u8,
  pub unknown2_1: u8,
  pub bits_per_sample_1: u16,
  pub unknown3_400: u16,
  pub bits_per_sample_2: u16,
  pub bits_per_sample_3: u16,
  pub pcm_type: u16,
}

impl CommonChannelsDescriptor {
  pub fn new(self_info: &DeviceInfo) -> Self {
    Self {
      sample_rate: self_info.sample_rate,
      unknown1_1: 1,
      unknown2_1: 1,
      bits_per_sample_1: self_info.bits_per_sample.into(),
      unknown3_400: 0x400,
      bits_per_sample_2: self_info.bits_per_sample.into(),
      bits_per_sample_3: self_info.bits_per_sample.into(),
      pcm_type: self_info.pcm_type.into(),
    }
  }
}

pub mod get_receive_channels {
  use binary_serde::BinarySerde;

  pub const OPCODE: u16 = 0x3000;

  #[derive(Debug, BinarySerde, Default)]
  pub struct ChannelDescriptor {
    pub channel_id: u16,
    pub unknown1_6: u16,
    pub common_descriptor_offset: u16,
    pub tx_channel_name_offset: u16,
    pub tx_hostname_offset: u16,
    pub friendly_name_offset: u16,
    pub subscription_status: u32, // TODO. 0x01010009 if subscribed currently, 0x00000001 if not found but remembers subscription or in progress
    pub unknown2_0: u32,
  }
}

pub mod get_transmit_channels {
  use binary_serde::BinarySerde;

  pub const OPCODE: u16 = 0x2000;

  #[derive(Debug, BinarySerde, Default)]
  pub struct ChannelDescriptor {
    pub channel_id: u16,
    pub unknown1_7: u16,
    pub common_descriptor_offset: u16,
    pub name_offset: u16,
  }
}

pub mod get_transmit_channels_friendly_names {
  use binary_serde::BinarySerde;

  pub const OPCODE: u16 = 0x2010;

  #[derive(Debug, BinarySerde, Default)]
  pub struct ChannelDescriptor {
    pub channel_id_1: u16,
    pub channel_id_2: u16,
    pub friendly_name_offset: u16,
  }
}

pub mod rename_tx_channels {
  pub const OPCODE: u16 = 0x2013;

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct SingleChannelRenameRequest {
    pub unknown1_0: u16,
    pub channel_id: u16,
    pub new_name_offset: u16,
  }
}

pub mod rename_rx_channels {
  pub const OPCODE: u16 = 0x3001;

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct SingleChannelRenameRequest {
    pub channel_id: u16,
    pub new_name_offset: u16,
  }
}

#[derive(Debug, binary_serde::BinarySerde, Default)]
pub struct DestinationSocketDescriptor {
  pub unknown1_8002: u16,
  pub port: u16,
  pub addr: [u8; 4],
}

pub mod query_tx_flows {
  pub const OPCODE: u16 = 0x2200;

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct NamesDescriptor {
    pub unknown1_a00: u16,
    pub unknown2_1: u16,
    pub remote_hostname_offset: u16,
    pub remote_rx_flow_name_offset: u16,
    pub unknown3_10: u16, // or 0x3c ???
    pub local_tx_flow_name_offset: u16,
    pub unknown4_0: [u8; 8], // in multicast flows first 4B are latency_ns
  }

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct FlowDescriptorHeader {
    pub flow_id: u16,
    pub flow_type: u16, // 0x11 for unicast, 2 for multicast
    pub sample_rate: u32,
    pub unknown1_0: u16,
    pub bits_per_sample: u16,
    pub unknown2_1: u16,
    pub channels_count: u16,
    pub receiver_socket_descriptor_offset: u16,
  }

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct FlowDescriptorFooter {
    pub names_descriptor_offset: u16,
  }
}

pub mod create_multicast_tx_flow {
  pub const OPCODE: u16 = 0x2201;

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct FlowDescriptorHeader {
    pub flow_id: u16,
    pub flow_type: u16,
    pub unknown1_0: [u8; 10],
    pub channels_count: u16,
  }

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct FlowDescriptorFooter {
    pub mostly_zeros_offset: u16,
  }

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct MostlyZeros {
    pub unknown1_a00: u16,
    pub unknown2_0: [u8; 14],
    pub unknown3_1: u16,
    pub unknown4_0: u16,
  }
}

pub mod delete_multicast_tx_flow {
  pub const OPCODE: u16 = 0x2202;
}

pub mod query_rx_flows {
  pub const OPCODE: u16 = 0x3200;

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct Descriptor2 {
    pub unknown1_9: u16,
    pub unknown2_1: u16,
    pub unknown3_800: u16,
    pub unknown4_0: u16,
    pub latency_ns: u32,
    pub unknown5_0: u32,
  }

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct FlowDescriptorHeader {
    pub flow_id: u16,
    pub unknown1_1: u16,
    pub sample_rate: u32,
    pub unknown2_0: u16,
    pub bits_per_sample: u16,
    pub unknown3_1: u16,
    pub channels_count: u16,
    pub words_per_bitmask: u16,
    pub receiver_socket_descriptor_offset: u16,
  }

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct FlowDescriptorFooter {
    pub descriptor2_offset: u16,
  }
}

pub mod set_channels_subscriptions {
  pub const OPCODE: u16 = 0x3010;

  #[derive(Debug, binary_serde::BinarySerde, Default)]
  pub struct SingleChannelSubscriptionRequest {
    pub local_channel_id: u16,
    pub tx_channel_name_offset: u16,
    pub tx_hostname_offset: u16,
  }
}

pub fn serialize_items<InItem, OutItem>(
  space_items: u8,
  source: impl IntoIterator<Item = InItem>,
  mut transform: impl FnMut(InItem, &mut ByteBuffer) -> Option<OutItem>,
) -> (bool, Vec<u8>)
where
  OutItem: BinarySerde,
{
  let mut bytes = ByteBuffer::new();
  bytes.write_bytes(&[0u8; HEADER_LENGTH]);
  bytes.write_u8(space_items);
  bytes.write_u8(0);
  if space_items == 0 {
    return (false, bytes.as_bytes()[HEADER_LENGTH..].into());
  }
  let space_items: usize = space_items.into();
  bytes.write_bytes(&vec![0u8; space_items * OutItem::SERIALIZED_SIZE]);

  let source = source.into_iter();
  let mut item_pos = 2 + HEADER_LENGTH;
  let mut actual_items = 0;
  let mut have_more = false;

  let mut tmp_buffer = vec![0u8; OutItem::SERIALIZED_SIZE];
  for in_item in source {
    if actual_items >= space_items {
      have_more = true;
      break;
    }
    let out_item = if let Some(item) = transform(in_item, &mut bytes) {
      item
    } else {
      continue;
    };
    out_item.binary_serialize(&mut tmp_buffer, binary_serde::Endianness::Big);
    let prev_pos = bytes.get_wpos();
    bytes.set_wpos(item_pos);
    bytes.write_bytes(&tmp_buffer);
    bytes.set_wpos(prev_pos);
    item_pos += OutItem::SERIALIZED_SIZE;
    if prev_pos >= PACKET_SIZE_SOFT_LIMIT {
      have_more = true;
      break;
    }
    actual_items += 1;
  }
  bytes.set_wpos(1 + HEADER_LENGTH);
  bytes.write_u8(actual_items.try_into().unwrap());
  (have_more, bytes.as_bytes()[HEADER_LENGTH..].into())
}

pub fn extract_start_index(request_payload: &[u8]) -> Option<usize> {
  if request_payload.len() < 4 || (request_payload[2] | request_payload[3]) == 0 {
    error!("got invalid paginate request, payload: {request_payload:?}");
    return None;
  }
  Some((make_u16(request_payload[2], request_payload[3]) - 1).into())
}

pub fn paginate_make_response<InItem, OutItem>(
  connection: &mut Connection,
  request_payload: &[u8],
  space_items: u8,
  source: impl IntoIterator<Item = InItem>,
  transform: impl FnMut(InItem, &mut ByteBuffer) -> Option<OutItem>,
) -> (u16, Vec<u8>)
where
  OutItem: BinarySerde,
{
  let start_index = match extract_start_index(request_payload) {
    Some(v) => v,
    None => {
      error!("unable to extract start index from request payload {}", hex::encode(request_payload));
      return (0xFFFF /* TODO */, vec![]);
    }
  };
  let (have_more, bytes) = serialize_items(space_items, source.into_iter().skip(start_index), transform);
  let code = if have_more { 0x8112 } else { 1 };
  (code, bytes)
}

pub async fn paginate_respond<InItem, OutItem>(
  connection: &mut Connection,
  request_payload: &[u8],
  space_items: u8,
  source: impl IntoIterator<Item = InItem>,
  transform: impl FnMut(InItem, &mut ByteBuffer) -> Option<OutItem>,
) where
  OutItem: BinarySerde,
{
  let (code, bytes) = paginate_make_response(connection, request_payload, space_items, source, transform);
  connection.respond_with_code(code, &bytes).await;
}

pub struct ItemsInPacketIterator<'a, T> {
  items_bytes: &'a [u8],
  item_start: usize,
  _t: PhantomData<T>,
}

impl<'a, T: BinarySerde> Iterator for ItemsInPacketIterator<'a, T> {
  type Item = T;
  fn next(&mut self) -> Option<Self::Item> {
    loop {
      let item_start = self.item_start;
      let item_end = item_start + T::SERIALIZED_SIZE;
      self.item_start = item_end;
      if item_end > self.items_bytes.len() {
        return None;
      }
      match T::binary_deserialize(&self.items_bytes[item_start..item_end], binary_serde::Endianness::Big)
      {
        Ok(item) => {
          return Some(item);
        }
        Err(e) => {
          error!(
            "unable to deserialize item in incoming packet: {e:?}, item: {}, all items: {}",
            hex::encode(&self.items_bytes[item_start..item_end]),
            hex::encode(&self.items_bytes)
          );
        }
      }
    }
  }
}

pub fn deserialize_items<'a, T: BinarySerde>(payload: &'a [u8]) -> ItemsInPacketIterator<'a, T> {
  let num_items: usize = (*payload.get(1).unwrap_or(&0)).into();
  let num_items = num_items.min(payload.len() / T::SERIALIZED_SIZE);
  ItemsInPacketIterator::<'a, T> {
    items_bytes: &payload[2..][..num_items * T::SERIALIZED_SIZE],
    item_start: 0,
    _t: Default::default(),
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use binary_serde::recursive_array::RecursiveArray;
  use binary_serde::BinarySerde;

  #[test]
  fn flags2_bitfield_roundtrip() {
    let original = channels_and_flows_count::Flags2 {
      unknown1_0: 0x0A,
      supports_tx_channel_rename: true,
      supports_tx_multicast: false,
      unknown2_0: 0x03,
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized = channels_and_flows_count::Flags2::binary_deserialize(
      bytes.as_slice(),
      binary_serde::Endianness::Big,
    )
    .unwrap();
    assert_eq!(original.unknown1_0, deserialized.unknown1_0);
    assert_eq!(original.supports_tx_channel_rename, deserialized.supports_tx_channel_rename);
    assert_eq!(original.supports_tx_multicast, deserialized.supports_tx_multicast);
    assert_eq!(original.unknown2_0, deserialized.unknown2_0);
  }

  #[test]
  fn channels_and_flows_count_response_roundtrip() {
    let original = channels_and_flows_count::Response {
      unknown1_0: 5,
      flags2: channels_and_flows_count::Flags2 {
        unknown1_0: 0,
        supports_tx_channel_rename: true,
        supports_tx_multicast: true,
        unknown2_0: 0,
      },
      tx_channels_count: 8,
      rx_channels_count: 8,
      unknown2_4: 1,
      max_channels_in_flow: 8,
      unknown4_8: 0,
      max_tx_flows: 4,
      max_rx_flows: 4,
      unknown5_total_channels: 16,
      unknown6_1: 1,
      unknown7_1: 1,
      unknown8_0: [0; 6],
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized = channels_and_flows_count::Response::binary_deserialize(
      bytes.as_slice(),
      binary_serde::Endianness::Big,
    )
    .unwrap();
    assert_eq!(original, deserialized);
  }

  #[test]
  fn common_channels_descriptor_new_matches_device_info() {
    let device = DeviceInfo {
      ip_address: std::net::Ipv4Addr::new(192, 168, 1, 1),
      netmask: std::net::Ipv4Addr::new(255, 255, 255, 0),
      gateway: std::net::Ipv4Addr::new(192, 168, 1, 1),
      mac_address: netdev::mac::MacAddr::from_hex_format("00:11:22:33:44:55"),
      link_speed: 1000,
      board_name: "TestBoard".to_string(),
      manufacturer: "TestMfg".to_string(),
      model_name: "TestModel".to_string(),
      model_number: "123".to_string(),
      factory_device_id: [1, 2, 3, 4, 5, 6, 7, 8],
      process_id: 1,
      vendor_string: "TestVendor".to_string(),
      friendly_hostname: "test".to_string(),
      factory_hostname: "factory".to_string(),
      rx_channels: vec![],
      tx_channels: vec![],
      bits_per_sample: 24,
      pcm_type: 0x0e,
      latency_ns: 5000000,
      sample_rate: 48000,
      arc_port: 4440,
      cmc_port: 8800,
      flows_control_port: 4455,
      info_request_port: 8700,
    };
    let desc = CommonChannelsDescriptor::new(&device);
    assert_eq!(desc.sample_rate, 48000);
    assert_eq!(desc.bits_per_sample_1, 24);
    assert_eq!(desc.bits_per_sample_2, 24);
    assert_eq!(desc.bits_per_sample_3, 24);
    assert_eq!(desc.pcm_type, 0x0e);
    assert_eq!(desc.unknown1_1, 1);
    assert_eq!(desc.unknown2_1, 1);
    assert_eq!(desc.unknown3_400, 0x400);
  }

  #[test]
  fn destination_socket_descriptor_roundtrip() {
    let original =
      DestinationSocketDescriptor { unknown1_8002: 0x8002, port: 5004, addr: [192, 168, 1, 100] };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized =
      DestinationSocketDescriptor::binary_deserialize(bytes.as_slice(), binary_serde::Endianness::Big)
        .unwrap();
    assert_eq!(original.unknown1_8002, deserialized.unknown1_8002);
    assert_eq!(original.port, deserialized.port);
    assert_eq!(original.addr, deserialized.addr);
  }

  #[test]
  fn get_receive_channels_channel_descriptor_roundtrip() {
    let original = get_receive_channels::ChannelDescriptor {
      channel_id: 5,
      unknown1_6: 6,
      common_descriptor_offset: 10,
      tx_channel_name_offset: 20,
      tx_hostname_offset: 30,
      friendly_name_offset: 40,
      subscription_status: 0x01010009,
      unknown2_0: 0,
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized = get_receive_channels::ChannelDescriptor::binary_deserialize(
      bytes.as_slice(),
      binary_serde::Endianness::Big,
    )
    .unwrap();
    assert_eq!(original.channel_id, deserialized.channel_id);
    assert_eq!(original.subscription_status, deserialized.subscription_status);
  }

  #[test]
  fn get_transmit_channels_channel_descriptor_roundtrip() {
    let original = get_transmit_channels::ChannelDescriptor {
      channel_id: 3,
      unknown1_7: 7,
      common_descriptor_offset: 12,
      name_offset: 24,
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized = get_transmit_channels::ChannelDescriptor::binary_deserialize(
      bytes.as_slice(),
      binary_serde::Endianness::Big,
    )
    .unwrap();
    assert_eq!(original.channel_id, deserialized.channel_id);
    assert_eq!(original.name_offset, deserialized.name_offset);
  }

  #[test]
  fn rename_tx_channels_request_roundtrip() {
    let original = rename_tx_channels::SingleChannelRenameRequest {
      unknown1_0: 0,
      channel_id: 7,
      new_name_offset: 42,
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized = rename_tx_channels::SingleChannelRenameRequest::binary_deserialize(
      bytes.as_slice(),
      binary_serde::Endianness::Big,
    )
    .unwrap();
    assert_eq!(original.channel_id, deserialized.channel_id);
    assert_eq!(original.new_name_offset, deserialized.new_name_offset);
  }

  #[test]
  fn query_tx_flows_flow_descriptor_header_roundtrip() {
    let original = query_tx_flows::FlowDescriptorHeader {
      flow_id: 1,
      flow_type: 0x11,
      sample_rate: 48000,
      unknown1_0: 0,
      bits_per_sample: 24,
      unknown2_1: 1,
      channels_count: 2,
      receiver_socket_descriptor_offset: 100,
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized = query_tx_flows::FlowDescriptorHeader::binary_deserialize(
      bytes.as_slice(),
      binary_serde::Endianness::Big,
    )
    .unwrap();
    assert_eq!(original.flow_id, deserialized.flow_id);
    assert_eq!(original.flow_type, deserialized.flow_type);
    assert_eq!(original.sample_rate, deserialized.sample_rate);
    assert_eq!(original.channels_count, deserialized.channels_count);
  }

  #[test]
  fn query_rx_flows_descriptor2_roundtrip() {
    let original = query_rx_flows::Descriptor2 {
      unknown1_9: 9,
      unknown2_1: 1,
      unknown3_800: 0x800,
      unknown4_0: 0,
      latency_ns: 5000000,
      unknown5_0: 0,
    };
    let bytes = original.binary_serialize_to_array(binary_serde::Endianness::Big);
    let deserialized =
      query_rx_flows::Descriptor2::binary_deserialize(bytes.as_slice(), binary_serde::Endianness::Big)
        .unwrap();
    assert_eq!(original.latency_ns, deserialized.latency_ns);
    assert_eq!(original.unknown3_800, deserialized.unknown3_800);
  }

  #[test]
  fn extract_start_index_valid() {
    assert_eq!(extract_start_index(&[0, 0, 0, 5]), Some(4));
  }

  #[test]
  fn extract_start_index_too_short() {
    assert_eq!(extract_start_index(&[0, 0, 0]), None);
  }

  #[test]
  fn extract_start_index_zero_value() {
    assert_eq!(extract_start_index(&[0, 0, 0, 0]), None);
  }

  #[test]
  fn deserialize_items_empty() {
    let items: Vec<get_receive_channels::ChannelDescriptor> = deserialize_items(&[0, 0]).collect();
    assert!(items.is_empty());
  }
}
