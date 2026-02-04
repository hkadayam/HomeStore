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

use std::{
    fmt,
    sync::atomic::{AtomicU64, Ordering},
};

// Bit mask constants for 64-bit operations
pub const BIT_MASK: [u64; 64] = [
    1 << 0,
    1 << 1,
    1 << 2,
    1 << 3,
    1 << 4,
    1 << 5,
    1 << 6,
    1 << 7,
    1 << 8,
    1 << 9,
    1 << 10,
    1 << 11,
    1 << 12,
    1 << 13,
    1 << 14,
    1 << 15,
    1 << 16,
    1 << 17,
    1 << 18,
    1 << 19,
    1 << 20,
    1 << 21,
    1 << 22,
    1 << 23,
    1 << 24,
    1 << 25,
    1 << 26,
    1 << 27,
    1 << 28,
    1 << 29,
    1 << 30,
    1 << 31,
    1 << 32,
    1 << 33,
    1 << 34,
    1 << 35,
    1 << 36,
    1 << 37,
    1 << 38,
    1 << 39,
    1 << 40,
    1 << 41,
    1 << 42,
    1 << 43,
    1 << 44,
    1 << 45,
    1 << 46,
    1 << 47,
    1 << 48,
    1 << 49,
    1 << 50,
    1 << 51,
    1 << 52,
    1 << 53,
    1 << 54,
    1 << 55,
    1 << 56,
    1 << 57,
    1 << 58,
    1 << 59,
    1 << 60,
    1 << 61,
    1 << 62,
    1 << 63,
];

// Consecutive bit mask constants
pub const CONSECUTIVE_BITMASK: [u64; 64] = [
    (1u64 << 1) - 1,
    (1u64 << 2) - 1,
    (1u64 << 3) - 1,
    (1u64 << 4) - 1,
    (1u64 << 5) - 1,
    (1u64 << 6) - 1,
    (1u64 << 7) - 1,
    (1u64 << 8) - 1,
    (1u64 << 9) - 1,
    (1u64 << 10) - 1,
    (1u64 << 11) - 1,
    (1u64 << 12) - 1,
    (1u64 << 13) - 1,
    (1u64 << 14) - 1,
    (1u64 << 15) - 1,
    (1u64 << 16) - 1,
    (1u64 << 17) - 1,
    (1u64 << 18) - 1,
    (1u64 << 19) - 1,
    (1u64 << 20) - 1,
    (1u64 << 21) - 1,
    (1u64 << 22) - 1,
    (1u64 << 23) - 1,
    (1u64 << 24) - 1,
    (1u64 << 25) - 1,
    (1u64 << 26) - 1,
    (1u64 << 27) - 1,
    (1u64 << 28) - 1,
    (1u64 << 29) - 1,
    (1u64 << 30) - 1,
    (1u64 << 31) - 1,
    (1u64 << 32) - 1,
    (1u64 << 33) - 1,
    (1u64 << 34) - 1,
    (1u64 << 35) - 1,
    (1u64 << 36) - 1,
    (1u64 << 37) - 1,
    (1u64 << 38) - 1,
    (1u64 << 39) - 1,
    (1u64 << 40) - 1,
    (1u64 << 41) - 1,
    (1u64 << 42) - 1,
    (1u64 << 43) - 1,
    (1u64 << 44) - 1,
    (1u64 << 45) - 1,
    (1u64 << 46) - 1,
    (1u64 << 47) - 1,
    (1u64 << 48) - 1,
    (1u64 << 49) - 1,
    (1u64 << 50) - 1,
    (1u64 << 51) - 1,
    (1u64 << 52) - 1,
    (1u64 << 53) - 1,
    (1u64 << 54) - 1,
    (1u64 << 55) - 1,
    (1u64 << 56) - 1,
    (1u64 << 57) - 1,
    (1u64 << 58) - 1,
    (1u64 << 59) - 1,
    (1u64 << 60) - 1,
    (1u64 << 61) - 1,
    (1u64 << 62) - 1,
    (1u64 << 63) - 1,
    u64::MAX,
];

// Utility functions
pub fn log_base2(v: u64) -> u8 {
    if v == 0 {
        255
    } else {
        63 - v.leading_zeros() as u8
    }
}

pub fn get_trailing_zeros(v: u64) -> u8 {
    if v == 0 {
        64
    } else {
        v.trailing_zeros() as u8
    }
}

pub fn get_set_bit_count(v: u64) -> u8 { v.count_ones() as u8 }

pub fn get_leading_zeros(v: u64) -> u8 {
    if v == 0 {
        64
    } else {
        v.leading_zeros() as u8
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BitMatchType {
    NoMatch = 0,
    FullMatch = 1,
    LsbMatch = 2,
    MidMatch = 3,
    MsbMatch = 4,
}

#[derive(Clone, Copy, Debug)]
pub struct BitFilter {
    pub n_lsb_reqd: u32,
    pub n_mid_reqd: u32,
    pub n_msb_reqd: u32,
}

impl BitFilter {
    pub fn new(lsb_reqd: u32, mid_reqd: u32, msb_reqd: u32) -> Self {
        BitFilter { n_lsb_reqd: lsb_reqd, n_mid_reqd: mid_reqd, n_msb_reqd: msb_reqd }
    }
}

impl fmt::Display for BitFilter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "n_lsb_reqd={} n_mid_reqd={} n_msb_reqd={}", self.n_lsb_reqd, self.n_mid_reqd, self.n_msb_reqd)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct BitMatchResult {
    pub match_type: BitMatchType,
    pub start_bit: u8,
    pub count: u8,
}

impl BitMatchResult {
    pub fn new(match_type: BitMatchType, start_bit: u8, count: u8) -> Self {
        BitMatchResult { match_type, start_bit, count }
    }
}

impl fmt::Display for BitMatchResult {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.match_type == BitMatchType::NoMatch {
            write!(f, "NoMatch")
        } else {
            write!(f, "{:?} start={} count={}", self.match_type, self.start_bit, self.count)
        }
    }
}

// Trait for bit storage
pub trait BitStorage {
    type WordType: Copy + Clone + PartialEq;

    fn set(&mut self, value: Self::WordType);
    fn get(&self) -> Self::WordType;
    fn or_with(&mut self, value: Self::WordType) -> Self::WordType;
    fn and_with(&mut self, value: Self::WordType) -> Self::WordType;
    fn right_shift(&mut self, nbits: u8) -> Self::WordType;
    fn set_if(&mut self, old_value: Self::WordType, new_value: Self::WordType) -> bool;
}

// Unsafe (non-atomic) bit storage
#[derive(Clone, Copy, Debug)]
pub struct UnsafeBits<T> {
    value: T,
}

impl<T> UnsafeBits<T>
where
    T: Copy + Clone + PartialEq,
{
    pub fn new(value: T) -> Self { UnsafeBits { value } }
}

impl BitStorage for UnsafeBits<u64> {
    type WordType = u64;

    fn set(&mut self, value: u64) { self.value = value; }

    fn get(&self) -> u64 { self.value }

    fn or_with(&mut self, value: u64) -> u64 {
        self.value |= value;
        self.value
    }

    fn and_with(&mut self, value: u64) -> u64 {
        self.value &= value;
        self.value
    }

    fn right_shift(&mut self, nbits: u8) -> u64 {
        self.value >>= nbits;
        self.value
    }

    fn set_if(&mut self, old_value: u64, new_value: u64) -> bool {
        if self.value == old_value {
            self.value = new_value;
            true
        } else {
            false
        }
    }
}

impl PartialEq for UnsafeBits<u64> {
    fn eq(&self, other: &Self) -> bool { self.value == other.value }
}

// Safe (atomic) bit storage
#[derive(Debug)]
pub struct SafeBits {
    value: AtomicU64,
}

impl SafeBits {
    pub fn new(value: u64) -> Self { SafeBits { value: AtomicU64::new(value) } }
}

impl BitStorage for SafeBits {
    type WordType = u64;

    fn set(&mut self, value: u64) { self.value.store(value, Ordering::Relaxed); }

    fn get(&self) -> u64 { self.value.load(Ordering::Relaxed) }

    fn or_with(&mut self, value: u64) -> u64 {
        let old_value = self.value.fetch_or(value, Ordering::Relaxed);
        old_value | value
    }

    fn and_with(&mut self, value: u64) -> u64 {
        let old_value = self.value.fetch_and(value, Ordering::Relaxed);
        old_value & value
    }

    fn right_shift(&mut self, nbits: u8) -> u64 {
        let mut old_value = self.value.load(Ordering::Acquire);
        let mut new_value = old_value >> nbits;
        while let Err(actual) =
            self.value.compare_exchange_weak(old_value, new_value, Ordering::AcqRel, Ordering::Relaxed)
        {
            old_value = actual;
            new_value = old_value >> nbits;
        }
        new_value
    }

    fn set_if(&mut self, old_value: u64, new_value: u64) -> bool {
        self.value.compare_exchange(old_value, new_value, Ordering::Relaxed, Ordering::Relaxed).is_ok()
    }
}

impl Clone for SafeBits {
    fn clone(&self) -> Self { SafeBits::new(self.get()) }
}

impl PartialEq for SafeBits {
    fn eq(&self, other: &Self) -> bool { self.get() == other.get() }
}

// Main Bitword structure
#[derive(Debug)]
pub struct Bitword<T: BitStorage> {
    pub bits: T,
}

impl<T: BitStorage<WordType = u64>> Bitword<T> {
    pub fn new(bits: T) -> Self { Bitword { bits } }

    pub fn from_value(value: u64) -> Self
    where
        T: From<u64>,
    {
        Bitword { bits: T::from(value) }
    }

    pub const fn word_size() -> u8 { 64 }

    pub fn get_set_count(&self) -> u8 { get_set_bit_count(self.bits.get()) }

    pub fn get_reset_count(&self) -> u8 { Self::word_size() - get_set_bit_count(self.bits.get()) }

    pub fn value(&self) -> u64 { self.bits.get() }

    pub fn set_bits(&mut self, start: u8, nbits: u8) -> u64 {
        assert!(start < Self::word_size());
        self.set_reset_bits(start, nbits, true)
    }

    pub fn reset_bits(&mut self, start: u8, nbits: u8) -> u64 {
        assert!(start < Self::word_size());
        self.set_reset_bits(start, nbits, false)
    }

    pub fn set_reset_bit(&mut self, start: u8, set: bool) -> u64 {
        assert!(start < Self::word_size());
        if set {
            self.bits.or_with(BIT_MASK[start as usize])
        } else {
            self.bits.and_with(!BIT_MASK[start as usize])
        }
    }

    pub fn set_reset_bits(&mut self, start: u8, nbits: u8, set: bool) -> u64 {
        assert!(start < Self::word_size());
        if nbits == 1 {
            return self.set_reset_bit(start, set);
        }

        let wanted_bits = std::cmp::min(Self::word_size() - start, nbits);
        let bit_mask = (CONSECUTIVE_BITMASK[(wanted_bits - 1) as usize]) << start;

        if set {
            self.bits.or_with(bit_mask)
        } else {
            self.bits.and_with(!bit_mask)
        }
    }

    pub fn get_bitval(&self, bit: u8) -> bool { (self.bits.get() & BIT_MASK[bit as usize]) != 0 }

    pub fn is_bit_set_reset(&self, start: u8, check_for_set: bool) -> bool {
        assert!(start < Self::word_size());
        let v = self.bits.get() & BIT_MASK[start as usize];
        if check_for_set {
            v != 0
        } else {
            v == 0
        }
    }

    pub fn is_bits_set_reset(&self, start: u8, nbits: u8, check_for_set: bool) -> bool {
        assert!(start < Self::word_size());
        if nbits == 1 {
            return self.is_bit_set_reset(start, check_for_set);
        }

        let actual = self.extract(start, nbits);
        let expected = if check_for_set { CONSECUTIVE_BITMASK[(nbits - 1) as usize] } else { 0 };
        actual == expected
    }

    pub fn get_next_set_bit(&self, start: u8) -> Option<u8> {
        assert!(start < Self::word_size());
        let e = self.extract(start, Self::word_size());
        if e != 0 {
            Some(get_trailing_zeros(e) + start)
        } else {
            None
        }
    }

    pub fn get_next_reset_bit(&self, start: u8) -> Option<u8> {
        assert!(start < Self::word_size());
        let e = !self.extract(start, Self::word_size());
        if e == 0 {
            None
        } else {
            let result = get_trailing_zeros(e) + start;
            if result < Self::word_size() {
                Some(result)
            } else {
                None
            }
        }
    }

    pub fn get_prev_set_bit(&self, start: u8) -> Option<u8> {
        let e = self.extract(0, start + 1);
        if e != 0 {
            Some(log_base2(e))
        } else {
            None
        }
    }

    pub fn get_next_reset_bits(&self, start: u8) -> (u8, u8) {
        assert!(start < Self::word_size());
        let e = self.extract(start, Self::word_size());

        if e == 0 {
            // Shortcut for all zeros
            (start, Self::word_size() - start)
        } else {
            // Find the first 0th bit in the word
            let first_0bit = get_trailing_zeros(!e) + start;
            if first_0bit >= Self::word_size() {
                // No more zero's here in our range
                (Self::word_size(), 0)
            } else {
                let count =
                    std::cmp::min(get_trailing_zeros(e >> (first_0bit - start)), Self::word_size() - first_0bit);
                (first_0bit, count)
            }
        }
    }

    pub fn get_next_reset_bits_filtered(&self, offset: u8, filter: &BitFilter) -> BitMatchResult {
        assert!(offset < Self::word_size());
        let mut result = BitMatchResult::new(BitMatchType::NoMatch, offset, 0);
        let mut lsb_search = offset == 0;

        let mut e = self.extract(offset, Self::word_size());
        let mut nbits = Self::word_size() - offset;

        while nbits > 0 {
            let first_0bit = get_trailing_zeros(!e);
            result.start_bit += first_0bit;

            if first_0bit >= nbits {
                // No more zero's here in our range
                result.count = 0;
                break;
            }

            if first_0bit > 0 {
                // remove all the 1's group
                e >>= first_0bit;
                nbits -= first_0bit;
            }

            result.count = if e > 0 { get_trailing_zeros(e) } else { nbits };

            if lsb_search {
                if first_0bit == 0 && result.count >= filter.n_lsb_reqd as u8 {
                    // We matched lsb with required count
                    result.match_type = if e == 0 { BitMatchType::FullMatch } else { BitMatchType::LsbMatch };
                    break;
                }
            }

            if e == 0 {
                if result.count >= filter.n_mid_reqd as u8 || result.count >= filter.n_msb_reqd as u8 {
                    result.match_type = BitMatchType::MsbMatch;
                }
                break;
            } else if result.count >= filter.n_mid_reqd as u8 {
                result.match_type = BitMatchType::MidMatch;
                break;
            }

            e >>= result.count;
            lsb_search = false;
            nbits -= result.count;
            result.start_bit += result.count;
        }

        result
    }

    pub fn set_next_reset_bit(&mut self, start: u8, maxbits: u8) -> Option<u8> {
        assert!(start < Self::word_size());
        if let Some(bit) = self.get_next_reset_bit(start) {
            if bit < maxbits {
                self.set_reset_bit(bit, true);
                Some(bit)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn right_shift(&mut self, nbits: u8) -> u64 { self.bits.right_shift(nbits) }

    pub fn get_max_contiguous_reset_bits(&self, start: u8) -> (u8, u8) {
        assert!(start < Self::word_size());

        let mut max_count = 0u8;
        let mut offset = start;
        let mut e = self.extract(start, Self::word_size());
        let mut start_largest_group = u8::MAX;

        while offset < Self::word_size() {
            if e == 0 {
                // Shortcut for all zeros
                let num_reset_bits = Self::word_size() - offset;
                if num_reset_bits > max_count {
                    max_count = num_reset_bits;
                    start_largest_group = offset;
                }
                break;
            } else {
                // Find the first 0th bit in the word
                let first_0bit = get_trailing_zeros(!e);
                if first_0bit >= Self::word_size() {
                    // No more zero's here in our range
                    break;
                } else {
                    if first_0bit > 0 {
                        // remove all the 1's group
                        e >>= first_0bit;
                        offset += first_0bit;
                    }
                    let num_reset_bits = if e > 0 { get_trailing_zeros(e) } else { Self::word_size() - offset };

                    if num_reset_bits > max_count {
                        max_count = num_reset_bits;
                        start_largest_group = offset;
                    }

                    // remove the all 0's group
                    offset += num_reset_bits;
                    e >>= num_reset_bits;
                }
            }
        }

        (start_largest_group, max_count)
    }

    pub fn to_integer(&self) -> u64 { self.bits.get() }

    fn extract(&self, start: u8, nbits: u8) -> u64 {
        let wanted_bits = std::cmp::min(Self::word_size() - start, nbits);
        assert!(wanted_bits > 0);
        let mask = (CONSECUTIVE_BITMASK[(wanted_bits - 1) as usize]) << start;
        (self.bits.get() & mask) >> start
    }
}

impl<T: BitStorage<WordType = u64>> Clone for Bitword<T>
where
    T: Clone,
{
    fn clone(&self) -> Self { Bitword { bits: self.bits.clone() } }
}

impl<T: BitStorage<WordType = u64>> PartialEq for Bitword<T>
where
    T: PartialEq,
{
    fn eq(&self, other: &Self) -> bool { self.bits == other.bits }
}

impl<T: BitStorage<WordType = u64>> fmt::Display for Bitword<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut result = String::new();
        let e = self.bits.get();
        for bit in 0..Self::word_size() {
            result.push(if (e & BIT_MASK[bit as usize]) == BIT_MASK[bit as usize] { '1' } else { '0' });
        }
        write!(f, "{}", result)
    }
}

// Type aliases for convenience
pub type UnsafeBitword = Bitword<UnsafeBits<u64>>;
pub type SafeBitword = Bitword<SafeBits>;

impl From<u64> for UnsafeBits<u64> {
    fn from(value: u64) -> Self { UnsafeBits::new(value) }
}

impl From<u64> for SafeBits {
    fn from(value: u64) -> Self { SafeBits::new(value) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_bit_utilities() {
        assert_eq!(log_base2(0), 255);
        assert_eq!(log_base2(1), 0);
        assert_eq!(log_base2(2), 1);
        assert_eq!(log_base2(8), 3);

        assert_eq!(get_trailing_zeros(0), 64);
        assert_eq!(get_trailing_zeros(1), 0);
        assert_eq!(get_trailing_zeros(2), 1);
        assert_eq!(get_trailing_zeros(8), 3);

        assert_eq!(get_set_bit_count(0), 0);
        assert_eq!(get_set_bit_count(1), 1);
        assert_eq!(get_set_bit_count(3), 2);
        assert_eq!(get_set_bit_count(u64::MAX), 64);
    }

    #[test]
    fn test_bitword_basic() {
        let mut bitword = UnsafeBitword::from_value(0);
        assert_eq!(bitword.get_set_count(), 0);
        assert_eq!(bitword.get_reset_count(), 64);

        bitword.set_reset_bit(0, true);
        assert_eq!(bitword.to_integer(), 1);
        assert!(bitword.get_bitval(0));
        assert!(!bitword.get_bitval(1));

        bitword.set_reset_bit(63, true);
        assert_eq!(bitword.to_integer(), 0x8000000000000001);
        assert!(bitword.get_bitval(63));
    }
}
