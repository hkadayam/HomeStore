/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

use sisl::bitword::{
    get_leading_zeros, get_trailing_zeros, log_base2, BitFilter, BitMatchType, Bitword, UnsafeBits,
};

fn validate(val: u64, offset: u8, filter: BitFilter, exp_start: u8, exp_match: BitMatchType, exp_count: u8) -> bool {
    let bword = Bitword::<UnsafeBits<u64>>::from_value(val);
    let result = bword.get_next_reset_bits_filtered(offset, &filter);

    if result.match_type != exp_match {
        println!(
            "Val={:x} offset={} filter[{}] Expected type={:?} but got {:?}, result[{}]: FAILED",
            val, offset, filter, exp_match, result.match_type, result
        );
        return false;
    }

    if result.match_type != BitMatchType::NoMatch && (result.start_bit != exp_start || result.count != exp_count) {
        println!(
            "Val={:x} offset={} filter[{}] Expected start bit={} & count={} but got {} & {}, result[{}]: FAILED",
            val, offset, filter, exp_start, exp_count, result.start_bit, result.count, result
        );
        return false;
    }

    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_log2_base() {
        // Test edge cases
        assert_eq!(log_base2(0), 255);
        assert_eq!(log_base2(1), 0);
        assert_eq!(log_base2(2), 1);
        assert_eq!(log_base2(8), 3);

        // Test powers of 2
        let mut v = 1u64;
        for bit in 0..64 {
            assert_eq!(log_base2(v), bit);
            if bit < 63 {
                // Avoid overflow
                v <<= 1;
            }
        }

        // Test some specific values
        for x in 1..=255u8 {
            let val = x as u64;
            let expected = (val as f64).log2().floor() as u8;
            assert_eq!(log_base2(val), expected);
        }
    }

    #[test]
    fn test_set_count() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x1);
        assert_eq!(word1.get_set_count(), 1);

        let word2 = Bitword::<UnsafeBits<u64>>::from_value(0x0);
        assert_eq!(word2.get_set_count(), 0);

        let word3 = Bitword::<UnsafeBits<u64>>::from_value(0x100000000);
        assert_eq!(word3.get_set_count(), 1);

        let word4 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFFF);
        assert_eq!(word4.get_set_count(), 64);
    }

    #[test]
    fn test_trailing_zeros() {
        assert_eq!(get_trailing_zeros(0x01), 0);
        assert_eq!(get_trailing_zeros(0x02), 1);
        assert_eq!(get_trailing_zeros(0x00), 64);
        assert_eq!(get_trailing_zeros(0xf000000000), 36);
        assert_eq!(get_trailing_zeros(0xf00f000000000), 36);
        assert_eq!(get_trailing_zeros(0x8000000000000000), 63);
    }

    #[test]
    fn test_leading_zeros() {
        assert_eq!(get_leading_zeros(0x01), 63);
        assert_eq!(get_leading_zeros(0x00), 64);
        assert_eq!(get_leading_zeros(0xFFFFFFFFFFFFFFFF), 0);
        assert_eq!(get_leading_zeros(0x7FFFFFFFFFFFFFFF), 1);
        assert_eq!(get_leading_zeros(0x0FFFFFFFFFFFFFFF), 4);
        assert_eq!(get_leading_zeros(0x00FFFFFFFFFFFFFF), 8);
        assert_eq!(get_leading_zeros(0x00F0FFFFFFFFFFFF), 8);
    }

    #[test]
    fn test_reset_count() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x1);
        assert_eq!(word1.get_reset_count(), 63);

        let word2 = Bitword::<UnsafeBits<u64>>::from_value(0x0);
        assert_eq!(word2.get_reset_count(), 64);

        let word3 = Bitword::<UnsafeBits<u64>>::from_value(0x100000000);
        assert_eq!(word3.get_reset_count(), 63);

        let word4 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFFF);
        assert_eq!(word4.get_reset_count(), 0);
    }

    #[test]
    fn test_set_reset_bit() {
        let mut word1 = Bitword::<UnsafeBits<u64>>::from_value(0x0);
        assert_eq!(word1.set_reset_bit(0, true), 0x01);
        assert_eq!(word1.set_reset_bit(63, true), 0x8000000000000001);

        assert_eq!(word1.set_reset_bit(0, false), 0x8000000000000000);
        assert_eq!(word1.set_reset_bit(63, false), 0x00);
    }

    #[test]
    fn test_set_bits() {
        let mut word1 = Bitword::<UnsafeBits<u64>>::from_value(0x0);
        assert_eq!(word1.set_bits(0, 2), 0x03);
        assert_eq!(word1.set_bits(62, 2), 0xC000000000000003);
    }

    #[test]
    fn test_reset_bits() {
        let mut word1 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFFF);
        assert_eq!(word1.reset_bits(0, 2), 0xFFFFFFFFFFFFFFFC);
        assert_eq!(word1.reset_bits(62, 2), 0x3FFFFFFFFFFFFFFC);
    }

    #[test]
    fn test_get_bit_val() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x8000000000000001);
        assert!(word1.get_bitval(63));
        assert!(word1.get_bitval(0));
        assert!(!word1.get_bitval(62));
        assert!(!word1.get_bitval(1));
    }

    #[test]
    fn test_is_bit_set_reset() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x8000000000000001);
        assert!(word1.is_bit_set_reset(63, true));
        assert!(word1.is_bit_set_reset(0, true));
        assert!(word1.is_bit_set_reset(62, false));
        assert!(word1.is_bit_set_reset(1, false));
    }

    #[test]
    fn test_is_bits_set_reset() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0xC000000000000003);
        assert!(word1.is_bits_set_reset(62, 2, true));
        assert!(word1.is_bits_set_reset(0, 2, true));
        assert!(word1.is_bits_set_reset(60, 2, false));
        assert!(word1.is_bits_set_reset(2, 2, false));
    }

    #[test]
    fn test_get_next_set_bit() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x05);
        assert_eq!(word1.get_next_set_bit(0), Some(0));
        assert_eq!(word1.get_next_set_bit(1), Some(2));

        let word2 = Bitword::<UnsafeBits<u64>>::from_value(0x8000000000000000);
        assert_eq!(word2.get_next_set_bit(0), Some(63));
        assert_eq!(word2.get_next_set_bit(8), Some(63));

        let word3 = Bitword::<UnsafeBits<u64>>::from_value(0x0);
        assert_eq!(word3.get_next_set_bit(0), None);
        assert_eq!(word3.get_next_set_bit(8), None);
    }

    #[test]
    fn test_get_next_reset_bit() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x02);
        assert_eq!(word1.get_next_reset_bit(0), Some(0));
        assert_eq!(word1.get_next_reset_bit(1), Some(2));

        let word2 = Bitword::<UnsafeBits<u64>>::from_value(0x7FFFFFFFFFFFFFFF);
        assert_eq!(word2.get_next_reset_bit(0), Some(63));
        assert_eq!(word2.get_next_reset_bit(8), Some(63));

        let word3 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFFF);
        assert_eq!(word3.get_next_reset_bit(0), None);
        assert_eq!(word3.get_next_reset_bit(8), None);
    }

    #[test]
    fn test_get_next_reset_bits() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x00);
        let (start, count) = word1.get_next_reset_bits(0);
        assert_eq!(start, 0);
        assert_eq!(count, 64);
        let (start, count) = word1.get_next_reset_bits(8);
        assert_eq!(start, 8);
        assert_eq!(count, 56);

        let word2 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFF00);
        let (start, count) = word2.get_next_reset_bits(0);
        assert_eq!(start, 0);
        assert_eq!(count, 8);
        let (start, count) = word2.get_next_reset_bits(4);
        assert_eq!(start, 4);
        assert_eq!(count, 4);
        let (start, count) = word2.get_next_reset_bits(8);
        assert_eq!(start, 64);
        assert_eq!(count, 0);

        let word3 = Bitword::<UnsafeBits<u64>>::from_value(0x3FFFFFFFFFFFFFFF);
        let (start, count) = word3.get_next_reset_bits(0);
        assert_eq!(start, 62);
        assert_eq!(count, 2);
        let (start, count) = word3.get_next_reset_bits(8);
        assert_eq!(start, 62);
        assert_eq!(count, 2);
        let (start, count) = word3.get_next_reset_bits(63);
        assert_eq!(start, 63);
        assert_eq!(count, 1);

        let word4 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFFF);
        let (start, count) = word4.get_next_reset_bits(0);
        assert_eq!(start, 64);
        assert_eq!(count, 0);
        let (start, count) = word4.get_next_reset_bits(8);
        assert_eq!(start, 64);
        assert_eq!(count, 0);

        let word5 = Bitword::<UnsafeBits<u64>>::from_value(0x3FFFFFFFFFFFFFF0);
        let (start, count) = word5.get_next_reset_bits(0);
        assert_eq!(start, 0);
        assert_eq!(count, 4);
        let (start, count) = word5.get_next_reset_bits(8);
        assert_eq!(start, 62);
        assert_eq!(count, 2);
    }

    #[test]
    fn test_set_next_reset_bit() {
        let mut word1 = Bitword::<UnsafeBits<u64>>::from_value(0x00);
        assert_eq!(word1.set_next_reset_bit(0, 64), Some(0));
        assert_eq!(word1.set_next_reset_bit(1, 64), Some(1));

        let mut word2 = Bitword::<UnsafeBits<u64>>::from_value(0x7FFFFFFFFFFFFFFF);
        assert_eq!(word2.set_next_reset_bit(0, 64), Some(63));
        assert_eq!(word2.set_next_reset_bit(1, 64), None);

        let mut word3 = Bitword::<UnsafeBits<u64>>::from_value(0x0FF);
        assert_eq!(word3.set_next_reset_bit(0, 8), None);
    }

    #[test]
    fn test_right_shift() {
        let mut word1 = Bitword::<UnsafeBits<u64>>::from_value(0xFF00);
        assert_eq!(word1.right_shift(8), 0xFF);
    }

    #[test]
    fn test_to_string() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0x0F);
        // Note: The display format shows bits from LSB to MSB
        let expected = format!("{}", word1);
        assert!(expected.len() == 64); // Should show all 64 bits
        assert!(expected.starts_with("1111")); // First 4 bits are set (0x0F)
    }

    #[test]
    fn test_get_next_reset_bits_filtered() {
        assert!(validate(0xfff0, 0, BitFilter::new(5, 5, 1), 16, BitMatchType::MsbMatch, 48));
        assert!(validate(0xfff0, 0, BitFilter::new(4, 5, 1), 0, BitMatchType::LsbMatch, 4));

        assert!(validate(0x0, 0, BitFilter::new(5, 5, 1), 0, BitMatchType::FullMatch, 64));
        assert!(validate(0x0, 0, BitFilter::new(64, 70, 1), 0, BitMatchType::FullMatch, 64));
        assert!(validate(0xffffffffffffffff, 0, BitFilter::new(5, 5, 1), 0, BitMatchType::NoMatch, 0));

        assert!(validate(0x7fffffffffffffff, 0, BitFilter::new(2, 2, 1), 63, BitMatchType::MsbMatch, 1));
        assert!(validate(0x7f0f0f0f0f0f0f0f, 0, BitFilter::new(2, 2, 1), 4, BitMatchType::MidMatch, 4));
        assert!(validate(0x7f0f0f0f0f0f0f0f, 29, BitFilter::new(2, 2, 1), 29, BitMatchType::MidMatch, 3));

        assert!(validate(0x8000000000000000, 0, BitFilter::new(5, 8, 1), 0, BitMatchType::LsbMatch, 63));
        assert!(validate(0x8000000000000001, 0, BitFilter::new(5, 8, 1), 1, BitMatchType::MidMatch, 62));
        assert!(validate(0x8000000000000001, 10, BitFilter::new(8, 8, 1), 10, BitMatchType::MidMatch, 53));

        assert!(validate(0x7fffffffffffffff, 0, BitFilter::new(1, 1, 1), 63, BitMatchType::MsbMatch, 1));
        assert!(validate(0x7fffffffffffffff, 56, BitFilter::new(1, 1, 1), 63, BitMatchType::MsbMatch, 1));
        assert!(validate(0x7fffffffffffffff, 56, BitFilter::new(2, 2, 1), 63, BitMatchType::MsbMatch, 1));

        assert!(validate(0x7ff000ffff00ff0f, 0, BitFilter::new(11, 11, 1), 40, BitMatchType::MidMatch, 12));
        assert!(validate(0x7ff000ffff00ff0f, 5, BitFilter::new(2, 2, 1), 5, BitMatchType::MidMatch, 3));
        assert!(validate(0x7ff000ffff00ff0f, 5, BitFilter::new(8, 8, 1), 16, BitMatchType::MidMatch, 8));

        assert!(validate(0x0ff000ffff00ff0f, 5, BitFilter::new(8, 64, 4), 60, BitMatchType::MsbMatch, 4));

        assert!(validate(0x8fffff0f0f0f00f4, 0, BitFilter::new(3, 9, 1), 0, BitMatchType::NoMatch, 0));
        assert!(validate(0x8ff00f0f0f0f00f4, 1, BitFilter::new(3, 9, 1), 0, BitMatchType::NoMatch, 0));
        assert!(validate(0x7ff00f0f0f0f00f4, 0, BitFilter::new(3, 9, 2), 0, BitMatchType::NoMatch, 0));
        assert!(validate(0x00ff0f0f0f0ff0f4, 0, BitFilter::new(3, 9, 9), 0, BitMatchType::NoMatch, 0));
    }

    #[test]
    fn test_get_max_contiguous_reset_bits() {
        let word1 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFFF);
        let (start, max_count) = word1.get_max_contiguous_reset_bits(0);
        assert_eq!(start, u8::MAX);
        assert_eq!(max_count, 0);

        let word2 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFFF0);
        let (start, max_count) = word2.get_max_contiguous_reset_bits(0);
        assert_eq!(start, 0);
        assert_eq!(max_count, 4);
        let (start, max_count) = word2.get_max_contiguous_reset_bits(1);
        assert_eq!(start, 1);
        assert_eq!(max_count, 3);

        let word3 = Bitword::<UnsafeBits<u64>>::from_value(0x0FFFFFFFFFFFFFFF);
        let (start, max_count) = word3.get_max_contiguous_reset_bits(0);
        assert_eq!(start, 60);
        assert_eq!(max_count, 4);
        let (start, max_count) = word3.get_max_contiguous_reset_bits(1);
        assert_eq!(start, 60);
        assert_eq!(max_count, 4);

        let word4 = Bitword::<UnsafeBits<u64>>::from_value(0xFFFFFFFFFFFFFF0F);
        let (start, max_count) = word4.get_max_contiguous_reset_bits(0);
        assert_eq!(start, 4);
        assert_eq!(max_count, 4);
        let (start, max_count) = word4.get_max_contiguous_reset_bits(1);
        assert_eq!(start, 4);
        assert_eq!(max_count, 4);
        let (start, max_count) = word4.get_max_contiguous_reset_bits(8);
        assert_eq!(start, u8::MAX);
        assert_eq!(max_count, 0);

        let word5 = Bitword::<UnsafeBits<u64>>::from_value(0xFF00FFFFFFFFFF0F);
        let (start, max_count) = word5.get_max_contiguous_reset_bits(0);
        assert_eq!(start, 48);
        assert_eq!(max_count, 8);

        let word6 = Bitword::<UnsafeBits<u64>>::from_value(0xFF00FFFFFFFF000F);
        let (start, max_count) = word6.get_max_contiguous_reset_bits(0);
        assert_eq!(start, 4);
        assert_eq!(max_count, 12);
    }
}
