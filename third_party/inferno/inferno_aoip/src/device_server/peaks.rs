use std::sync::Arc;

use crate::{
  common::Sample,
  ring_buffer::{ProxyToSamplesBuffer, RingBufferShared},
};

pub fn peaks_of_buffers<P: ProxyToSamplesBuffer>(
  buffers: &Vec<Arc<RingBufferShared<Sample, P>>>,
) -> Vec<u8> {
  buffers
    .iter()
    .map(|rb| {
      let peak = rb.peak_sample();
      let peak_lin = (peak as f32) / (Sample::MAX as f32);
      let peak_log = peak_lin.log10() * 40.0;
      (-peak_log).round().clamp(0.0, 255.0) as u8
    })
    .collect()
}
