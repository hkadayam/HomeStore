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

use std::sync::{Arc, Mutex};

use sisl::{
    bitword::{SafeBits, UnsafeBits},
    BitBlock, BitsetImpl, NPOS,
};

type Bitset = BitsetImpl<UnsafeBits<u64>>;
type AtomicBitset = BitsetImpl<SafeBits>;

// Shadow bitset for validation (simplified version)
struct ShadowBitset {
    bits: Arc<Mutex<Vec<bool>>>,
    size: u64,
}

impl ShadowBitset {
    fn new(size: u64) -> Self { ShadowBitset { bits: Arc::new(Mutex::new(vec![false; size as usize])), size } }

    fn set(&self, value: bool, start_bit: u64, total_bits: u32) {
        let mut bits = self.bits.lock().unwrap();
        for b in start_bit..(start_bit + total_bits as u64).min(self.size) {
            if b < bits.len() as u64 {
                bits[b as usize] = value;
            }
        }
    }

    fn get_next_set_bit(&self, start_bit: u64) -> u64 {
        let bits = self.bits.lock().unwrap();
        for b in start_bit..self.size {
            if b < bits.len() as u64 && bits[b as usize] {
                return b;
            }
        }
        NPOS
    }

    fn get_next_reset_bit(&self, start_bit: u64) -> u64 {
        let bits = self.bits.lock().unwrap();
        for b in start_bit..self.size {
            if b < bits.len() as u64 && !bits[b as usize] {
                return b;
            }
        }
        NPOS
    }

    fn get_next_contiguous_n_reset_bits(&self, start_bit: u64, n: u32) -> BitBlock {
        let bits = self.bits.lock().unwrap();
        let mut current_start = NPOS;
        let mut current_count = 0u32;

        for b in start_bit..self.size {
            if b < bits.len() as u64 && !bits[b as usize] {
                if current_count == 0 {
                    current_start = b;
                }
                current_count += 1;
                if current_count == n {
                    return BitBlock::new(current_start, current_count);
                }
            } else {
                current_count = 0;
            }
        }

        BitBlock::new(NPOS, 0)
    }

    fn shrink_head(&self, nbits: u64) {
        let mut bits = self.bits.lock().unwrap();
        if nbits >= self.size {
            bits.clear();
            return;
        }

        // Shift bits left
        for i in 0..(self.size - nbits) as usize {
            if i + (nbits as usize) < bits.len() {
                bits[i] = bits[i + nbits as usize];
            } else {
                bits[i] = false;
            }
        }

        // Remove the shifted bits
        let new_size = (self.size - nbits) as usize;
        bits.truncate(new_size);
    }

    fn resize(&self, new_size: u64) {
        let mut bits = self.bits.lock().unwrap();
        bits.resize(new_size as usize, false);
    }

    fn is_set(&self, bit: u64) -> bool {
        let bits = self.bits.lock().unwrap();
        if bit < bits.len() as u64 {
            bits[bit as usize]
        } else {
            false
        }
    }
}

struct BitsetTest {
    total_bits: u64,
    shadow_bm: ShadowBitset,
    bset: AtomicBitset,
}

impl BitsetTest {
    fn new(total_bits: u64) -> Self {
        BitsetTest { total_bits, shadow_bm: ShadowBitset::new(total_bits), bset: AtomicBitset::new(total_bits, 0) }
    }

    fn fill_random(&mut self, start: u64, nbits: u32) {
        use rand::Rng;
        let mut rng = rand::thread_rng();

        for bit in start..(start + nbits as u64).min(self.total_bits) {
            let set = rng.gen_bool(0.5);
            if set {
                self.bset.set_bit(bit);
                self.shadow_bm.set(true, bit, 1);
            } else {
                self.bset.reset_bit(bit);
                self.shadow_bm.set(false, bit, 1);
            }
        }
    }

    fn set_bits(&mut self, start_bit: u64, nbits: u32) {
        self.bset.set_bits(start_bit, nbits as u64);
        self.shadow_bm.set(true, start_bit, nbits);
    }

    fn reset_bits(&mut self, start_bit: u64, nbits: u32) {
        self.bset.reset_bits(start_bit, nbits as u64);
        self.shadow_bm.set(false, start_bit, nbits);
    }

    fn shrink_head(&mut self, nbits: u64) {
        self.bset.shrink_head(nbits).unwrap();
        self.shadow_bm.shrink_head(nbits);
        self.total_bits -= nbits;
    }

    fn expand_tail(&mut self, nbits: u64) {
        self.bset.resize(self.total_bits + nbits, false).unwrap();
        self.shadow_bm.resize(self.total_bits + nbits);
        self.total_bits += nbits;
    }

    fn validate_by_simple_get(&self) -> bool {
        for i in 0..self.total_bits {
            if self.bset.get_bitval(i) != self.shadow_bm.is_set(i) {
                println!("Bit mismatch for bit={}", i);
                return false;
            }
        }
        true
    }

    fn validate_by_next_bits(&self, by_set: bool) -> bool {
        let mut next_shadow_bit = NPOS;
        let mut next_bset_bit = 0;

        loop {
            let expected_bit = if by_set {
                if next_shadow_bit == NPOS {
                    self.shadow_bm.get_next_set_bit(0)
                } else {
                    self.shadow_bm.get_next_set_bit(next_shadow_bit + 1)
                }
            } else {
                if next_shadow_bit == NPOS {
                    self.shadow_bm.get_next_reset_bit(0)
                } else {
                    self.shadow_bm.get_next_reset_bit(next_shadow_bit + 1)
                }
            };

            let actual_bit = if by_set {
                self.bset.get_next_set_bit(next_bset_bit)
            } else {
                self.bset.get_next_reset_bit(next_bset_bit)
            };

            if expected_bit == NPOS {
                if actual_bit != NPOS {
                    println!(
                        "Next {} bit after {} is expected to be EOB, but got {}",
                        if by_set { "set" } else { "reset" },
                        next_bset_bit,
                        actual_bit
                    );
                    return false;
                }
                break;
            }

            if expected_bit != actual_bit {
                println!(
                    "Next {} bit after {} is expected to be {}, but got {}",
                    if by_set { "set" } else { "reset" },
                    next_bset_bit,
                    expected_bit,
                    actual_bit
                );
                return false;
            }

            next_bset_bit = actual_bit + 1;
            next_shadow_bit = expected_bit;
        }

        true
    }

    fn validate_by_next_continuous_bits(&self, n_continuous: u32) -> bool {
        let mut next_start_bit = 0;

        loop {
            let expected = self.shadow_bm.get_next_contiguous_n_reset_bits(next_start_bit, n_continuous);
            let actual = self.bset.get_next_contiguous_n_reset_bits(next_start_bit, n_continuous);

            if expected.nbits != n_continuous {
                if actual.nbits == n_continuous {
                    println!(
                        "Next continuous reset bit after {} is expected to be EOB, but got valid result",
                        next_start_bit
                    );
                    return false;
                }
                break;
            }

            if expected.start_bit != actual.start_bit {
                println!(
                    "Next continuous reset bit after {} start_bit mismatch: expected {} got {}",
                    next_start_bit, expected.start_bit, actual.start_bit
                );
                return false;
            }

            if expected.nbits != actual.nbits {
                println!(
                    "Next continuous reset bit after {} nbits mismatch: expected {} got {}",
                    next_start_bit, expected.nbits, actual.nbits
                );
                return false;
            }

            next_start_bit = expected.start_bit + expected.nbits as u64;
        }

        true
    }

    fn validate_all(&self, n_continuous_expected: u32) -> bool {
        self.validate_by_simple_get()
            && self.validate_by_next_bits(true)
            && self.validate_by_next_bits(false)
            && self.validate_by_next_continuous_bits(n_continuous_expected)
    }

    fn total_bits(&self) -> u64 { self.total_bits }
}

#[cfg(test)]
mod tests {
    use super::*;

    const G_TOTAL_BITS: u64 = 1000;
    const G_NUM_THREADS: usize = 4;
    const G_SET_PCT: u32 = 25;
    const G_MAX_BITS_IN_GROUP: u32 = 72;

    #[test]
    fn test_set_count_with_shift() {
        let mut bset = Bitset::new(G_TOTAL_BITS, 0);
        bset.set_bits(0, G_TOTAL_BITS);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS);

        // Test same first word
        assert_eq!(bset.get_set_count(0, Some(0)), 1);
        assert_eq!(bset.get_set_count(63, Some(63)), 1);
        assert_eq!(bset.get_set_count(60, Some(63)), 4);
        assert_eq!(bset.get_set_count(0, Some(63)), 64);

        let start1 = 32u64;
        assert_eq!(bset.get_set_count(start1, None), G_TOTAL_BITS - start1);
        let start2 = 64u64;
        assert_eq!(bset.get_set_count(start2, None), G_TOTAL_BITS - start2);
        assert_eq!(bset.get_set_count(0, Some(G_TOTAL_BITS - 1 - start1)), G_TOTAL_BITS - start1);

        // Offset right a partial word
        let offset1 = 4u64;
        bset.shrink_head(offset1).unwrap();

        // Offset right more than a word
        let offset2 = 128u64;
        bset.shrink_head(offset2).unwrap();
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - (offset1 + offset2));

        // Offset right an exact multiple of a word
        let offset3 = 64 - ((offset1 + offset2) % 64);
        bset.shrink_head(offset3).unwrap();
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - (offset1 + offset2 + offset3));
    }

    #[test]
    fn test_set_count() {
        let mut bset = Bitset::new(G_TOTAL_BITS, 0);
        bset.set_bits(0, G_TOTAL_BITS);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS);

        // Reset word bits aligned to word size
        let word_size = 64u64;
        bset.reset_bits(0, word_size);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - word_size);

        // Reset word bits beginning and end of word and middle of word
        bset.reset_bits(2 * word_size, word_size / 2);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - word_size - word_size / 2);

        bset.reset_bits(3 * word_size + word_size / 2, word_size / 2);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - 2 * word_size);

        bset.reset_bits(4 * word_size + word_size / 4, word_size / 2);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - 2 * word_size - word_size / 2);

        // Reset multiple words
        bset.reset_bits(10 * word_size, 2 * word_size);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - 4 * word_size - word_size / 2);

        bset.reset_bits(13 * word_size + word_size / 2, 2 * word_size);
        assert_eq!(bset.get_set_count(0, None), G_TOTAL_BITS - 6 * word_size - word_size / 2);
    }

    #[test]
    fn test_get_word_value() {
        // Initialize a bitset and verify initial low byte values are zero
        let mut bset1 = Bitset::new(128, 0);
        assert_eq!(bset1.get_word_value(0) & 0xFF, 0x00);
        assert_eq!(bset1.get_word_value(8) & 0xFF, 0x00);

        // Set a few bits (outside the low 8 bits) so post-shrink we have a non-zero
        // word value
        bset1.set_bit(10); // bit within first word but not affecting low byte
        bset1.set_bit(70); // bit in second word

        // Shrink and ensure word still non-zero
        let shrink_bits1 = 4u64;
        bset1.shrink_head(shrink_bits1).unwrap();

        let val = bset1.get_word_value(0);
        assert!(val != 0 || bset1.size() == 0); // Should have some value or be
                                                // empty
    }

    #[test]
    fn test_print() {
        let bset = Bitset::new(64, 0);
        let str1 = format!("{}", bset);
        for c in str1.chars() {
            assert_eq!(c, '0');
        }

        let mut bset2 = Bitset::new(64, 0);
        bset2.set_bits(0, 64);
        let str2 = format!("{}", bset2);
        for c in str2.chars() {
            assert_eq!(c, '1');
        }
    }

    #[test]
    fn test_is_set_reset() {
        let mut bset = Bitset::new(128, 0);
        let word_size = 64u64;

        // Test partial word lower half
        bset.set_bits(0, word_size / 2);
        assert!(bset.is_bits_set(0, word_size / 2));

        // Test partial word upper half
        bset.reset_bits(0, 128);
        let start_bit1 = word_size - (word_size / 2);
        bset.set_bits(start_bit1, word_size / 2);
        assert!(bset.is_bits_set(start_bit1, word_size / 2));

        // Test half upper/lower next word
        bset.set_bits(word_size, word_size / 2);
        assert!(bset.is_bits_set(start_bit1, word_size));
    }

    #[test]
    fn test_copy_unshifted() {
        let mut bset = Bitset::new(200, 0);

        // Fill bitset with some pattern
        for i in (0..200).step_by(3) {
            bset.set_bit(i);
        }

        // Shift and make unshifted copy
        bset.shrink_head(5).unwrap();
        let mut tmp_bset = Bitset::new(bset.size(), 0);
        tmp_bset.copy_unshifted(&bset);

        for bit in 0..bset.size() {
            assert_eq!(bset.get_bitval(bit), tmp_bset.get_bitval(bit));
        }
    }

    #[test]
    fn test_get_next_contiguous_upto_n_reset_bits() {
        let mut bset = Bitset::new(200, 0);
        bset.set_bits(0, 200);

        bset.reset_bits(1, 2);
        bset.reset_bits(64, 4);
        bset.reset_bits(127, 8);

        let result0 = bset.get_next_contiguous_n_reset_bits(1, 1);
        assert_eq!(result0.start_bit, 1);
        assert_eq!(result0.nbits, 1);

        let result1 = bset.get_next_contiguous_n_reset_bits(0, 2);
        assert_eq!(result1.start_bit, 1);
        assert_eq!(result1.nbits, 2);

        let result2 = bset.get_next_contiguous_n_reset_bits(1, 2);
        assert_eq!(result2.start_bit, 1);
        assert_eq!(result2.nbits, 2);

        let result3 = bset.get_next_contiguous_n_reset_bits(0, 4);
        assert_eq!(result3.start_bit, 64);
        assert_eq!(result3.nbits, 4);

        let result4 = bset.get_next_contiguous_n_reset_bits(8, 4);
        assert_eq!(result4.start_bit, 64);
        assert_eq!(result4.nbits, 4);

        let result5 = bset.get_next_contiguous_n_reset_bits(0, 8);
        assert_eq!(result5.start_bit, 127);
        assert_eq!(result5.nbits, 8);

        let result6 = bset.get_next_contiguous_n_reset_bits(4, 8);
        assert_eq!(result6.start_bit, 127);
        assert_eq!(result6.nbits, 8);

        let result7 = bset.get_next_contiguous_n_reset_bits(70, 8);
        assert_eq!(result7.start_bit, 127);
        assert_eq!(result7.nbits, 8);

        // Test null result
        let result10 = bset.get_next_contiguous_n_reset_bits_range(0, None, 16, 16);
        assert_eq!(result10.start_bit, NPOS);
        assert_eq!(result10.nbits, 0);
    }

    #[test]
    fn test_equality_logic_check() {
        let mut bset1 = Bitset::new(200, 0);
        let mut bset2 = Bitset::new(200, 0);

        // Fill with same random pattern
        use rand::{Rng, SeedableRng};
        let mut rng = rand::rngs::StdRng::seed_from_u64(42);

        for i in 0..100 {
            let set = rng.gen_bool(0.5);
            if set {
                bset1.set_bit(i);
                bset2.set_bit(i);
            }
        }

        assert_eq!(bset1, bset2);

        // Flip a bit and test inequality
        bset2.set_bit(150);
        assert_ne!(bset1, bset2);

        bset1.set_bit(150);
        assert_eq!(bset1, bset2);
    }

    #[test]
    fn test_random_set_and_shrink() {
        let mut test = BitsetTest::new(500);

        // Set some random bits
        use rand::Rng;
        let mut rng = rand::thread_rng();

        for i in 0..250 {
            if rng.gen_bool(0.25) {
                // 25% set probability
                test.set_bits(i, 1);
            } else {
                test.reset_bits(i, 1);
            }
        }

        assert!(test.validate_all(1));

        // Shrink the bitset
        test.shrink_head(100);

        // Set more random bits
        for i in 0..test.total_bits().min(200) {
            if rng.gen_bool(0.25) {
                test.set_bits(i, 1);
            } else {
                test.reset_bits(i, 1);
            }
        }

        assert!(test.validate_all(3));
    }

    #[test]
    fn test_resize() {
        let mut bset = Bitset::new(50, 0);
        bset.set_bits(0, 50);

        bset.resize(100, false).unwrap();
        assert_eq!(bset.size(), 100);

        // Original bits should still be set
        for i in 0..50 {
            assert!(bset.get_bitval(i));
        }

        // New bits should be reset
        for i in 50..100 {
            assert!(!bset.get_bitval(i));
        }

        // Test resize with set value
        bset.resize(150, true).unwrap();
        assert_eq!(bset.size(), 150);

        // New bits should be set
        for i in 100..150 {
            assert!(bset.get_bitval(i));
        }
    }
}
