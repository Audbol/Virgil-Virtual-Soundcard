use crate::common::*;
use rand::Rng;

pub struct SamplesReader<'a> {
  pub bytes: &'a [u8],
  pub read_pos: usize,
  pub stride: usize,
  pub remaining_samples: usize,
}

impl<'a> SamplesReader<'a> {
  #[inline(always)]
  fn get_next_bytes(&mut self, count: usize) -> Option<&'a [u8]> {
    if self.read_pos + count > self.bytes.len() {
      return None;
    }
    let r = &self.bytes[self.read_pos..self.read_pos + count];
    self.read_pos += self.stride;
    self.remaining_samples -= 1;
    return Some(r);
  }
  #[inline(always)]
  fn size_hint(&self) -> (usize, Option<usize>) {
    let size = self.remaining_samples;
    (size, Some(size))
  }
}

macro_rules! samples_rw {
  ($bytes: literal, $reader_iterator: ident, $to_sample: expr, $writer_function: ident, $dither_type: ident) => {
    pub struct $reader_iterator<'a>(pub SamplesReader<'a>);
    impl<'a> Iterator for $reader_iterator<'a> {
      type Item = Sample;
      #[inline(always)]
      fn next(&mut self) -> Option<Sample> {
        self.0.get_next_bytes($bytes).map($to_sample).map(|s| s as Sample)
      }
      #[inline(always)]
      fn size_hint(&self) -> (usize, Option<usize>) {
        self.0.size_hint()
      }
    }
    impl<'a> ExactSizeIterator for $reader_iterator<'a> {}

    #[inline(always)]
    pub fn $writer_function<'a, I, R>(
      src: I,
      dst: &mut [u8],
      start_pos: usize,
      stride: usize,
      mut dither_rng: Option<&mut R>,
    ) where
      I: IntoIterator<Item = &'a Sample>,
      R: Rng,
    {
      let mut pos = start_pos;
      let mut srci = src.into_iter();
      let half_step: Sample = 1 << ($dither_type::BITS - 1);
      while pos + $bytes <= dst.len() {
        if let Some(&sample) = srci.next() {
          let out_sample = match &mut dither_rng {
            Some(rng) if $bytes < 4 => sample.saturating_add(
              (rng.gen::<$dither_type>() as Sample) - (rng.gen::<$dither_type>() as Sample) + half_step,
            ),
            _ => sample,
          };
          dst[pos..pos + $bytes].copy_from_slice(&out_sample.to_be_bytes()[0..$bytes]);
          pos += stride;
        } else {
          break;
        }
      }
    }
  };
}

samples_rw!(
  2,
  S16ReaderIterator,
  |b| ((b[0] as USample) << 24) | ((b[1] as USample) << 16),
  write_s16_samples,
  u16
);
samples_rw!(
  3,
  S24ReaderIterator,
  |b| ((b[0] as USample) << 24) | ((b[1] as USample) << 16) | ((b[2] as USample) << 8),
  write_s24_samples,
  u8
);
samples_rw!(
  4,
  S32ReaderIterator,
  |b| ((b[0] as USample) << 24)
    | ((b[1] as USample) << 16)
    | ((b[2] as USample) << 8)
    | (b[3] as USample),
  write_s32_samples,
  u8 // won't be used anyway
);

#[cfg(test)]
mod tests {
  use super::*;
  use rand::rngs::mock::StepRng;

  #[test]
  fn s16_reader_iterator_correct_values() {
    let bytes: Vec<u8> = vec![0x12, 0x34, 0x56, 0x78];
    let reader = SamplesReader { bytes: &bytes, read_pos: 0, stride: 2, remaining_samples: 2 };
    let mut iter = S16ReaderIterator(reader);
    assert_eq!(iter.next(), Some(0x12340000i32));
    assert_eq!(iter.next(), Some(0x56780000i32));
    assert_eq!(iter.next(), None);
  }

  #[test]
  fn s24_reader_iterator_correct_values() {
    let bytes: Vec<u8> = vec![0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF];
    let reader = SamplesReader { bytes: &bytes, read_pos: 0, stride: 3, remaining_samples: 2 };
    let mut iter = S24ReaderIterator(reader);
    assert_eq!(iter.next(), Some(0x12345600i32));
    assert_eq!(iter.next(), Some(0xABCDEF00u32 as i32)); // 0xABCDEF00 as signed
    assert_eq!(iter.next(), None);
  }

  #[test]
  fn s32_reader_iterator_correct_values() {
    let bytes: Vec<u8> = vec![0x12, 0x34, 0x56, 0x78];
    let reader = SamplesReader { bytes: &bytes, read_pos: 0, stride: 4, remaining_samples: 1 };
    let mut iter = S32ReaderIterator(reader);
    assert_eq!(iter.next(), Some(0x12345678i32));
    assert_eq!(iter.next(), None);
  }

  #[test]
  fn write_s16_samples_basic() {
    let samples: Vec<Sample> = vec![0x12340000, 0x56780000];
    let mut dst = vec![0u8; 4];
    write_s16_samples(&samples, &mut dst, 0, 2, None::<&mut StepRng>);
    assert_eq!(dst, vec![0x12, 0x34, 0x56, 0x78]);
  }

  #[test]
  fn write_s24_samples_basic() {
    let samples: Vec<Sample> = vec![0x12345600, 0xABCDEF00u32 as i32];
    let mut dst = vec![0u8; 6];
    write_s24_samples(&samples, &mut dst, 0, 3, None::<&mut StepRng>);
    assert_eq!(dst, vec![0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF]);
  }

  #[test]
  fn write_s32_samples_basic() {
    let samples: Vec<Sample> = vec![0x12345678];
    let mut dst = vec![0u8; 4];
    write_s32_samples(&samples, &mut dst, 0, 4, None::<&mut StepRng>);
    assert_eq!(dst, vec![0x12, 0x34, 0x56, 0x78]);
  }

  #[test]
  fn exact_size_iterator_length() {
    let bytes: Vec<u8> = vec![0; 8];
    let reader = SamplesReader { bytes: &bytes, read_pos: 0, stride: 2, remaining_samples: 4 };
    let iter = S16ReaderIterator(reader);
    assert_eq!(iter.len(), 4);
  }

  #[test]
  fn writer_stops_at_dst_end() {
    let samples: Vec<Sample> = vec![0x11227777, 0x33446666, 0x66666666];
    let mut dst = vec![0u8; 4]; // only room for 2 bytes
    write_s16_samples(&samples, &mut dst, 0, 2, None::<&mut StepRng>);
    assert_eq!(dst, vec![0x11, 0x22, 0x33, 0x44]);
  }

  #[test]
  fn writer_stops_when_source_exhausted() {
    let samples: Vec<Sample> = vec![0x12345678];
    let mut dst = vec![0xFFu8; 8];
    write_s16_samples(&samples, &mut dst, 0, 2, None::<&mut StepRng>);
    assert_eq!(&dst[0..2], &[0x12, 0x34]);
    assert_eq!(&dst[2..], &[0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
  }

  #[test]
  fn writer_stride_gaps() {
    let samples: Vec<Sample> = vec![0x12340000, 0x56780000];
    let mut dst = vec![0xFFu8; 6];
    write_s16_samples(&samples, &mut dst, 0, 3, None::<&mut StepRng>);
    assert_eq!(dst, vec![0x12, 0x34, 0xFF, 0x56, 0x78, 0xFF]);
  }
}
