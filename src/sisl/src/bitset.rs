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

use std::{fmt, marker::PhantomData};

use iomgr::IOBuffer;
use zerocopy::{IntoBytes, Ref};

use crate::bitword::BitStorage;

pub const NPOS: u64 = u64::MAX;

// Serialization constants
pub const BITSET_MAGIC: u32 = 0x42495453; // "BITS" in ASCII
pub const BITSET_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug)]
pub struct BitBlock {
    pub start_bit: u64,
    pub nbits: u32,
}

impl BitBlock {
    pub fn new(start_bit: u64, nbits: u32) -> Self { BitBlock { start_bit, nbits } }
}

/// Serialized representation of bitset for direct I/O
/// This structure matches the layout needed for zero-copy serialization
#[repr(C, packed)]
#[derive(Debug, zerocopy::FromBytes, zerocopy::IntoBytes, zerocopy::KnownLayout, zerocopy::Immutable)]
pub struct BitsetSerialized {
    /// Magic number for validation
    pub magic: u32,
    /// Version for compatibility
    pub version: u32,
    /// Bitset ID
    pub id: u64,
    /// Total number of bits
    pub nbits: u64,
    /// Number of bits to skip from the beginning
    pub skip_bits: u64,
    /// Number of 64-bit words following this header
    pub word_count: u64,
    /// Number of set bits in the bitset
    /// NOTE: This field is NOT automatically updated by bitset operations.
    /// It must be manually updated by the user via establish_set_count().
    pub set_count: u64,
    // Note: The actual bit words follow this structure in memory
}

impl BitsetSerialized {
    pub const MAGIC: u32 = 0x42495453; // "BITS" in ASCII
    pub const VERSION: u32 = 1;

    /// Calculate the total size needed for serialization including words
    pub fn calculate_size(word_count: usize) -> usize {
        std::mem::size_of::<BitsetSerialized>() + (word_count * std::mem::size_of::<u64>())
    }

    /// Get the alignment requirement for direct I/O
    pub fn alignment() -> usize {
        // Use 4KB alignment for optimal direct I/O performance
        4096
    }
}

/// Zero-copy bitset using aligned-vec and zerocopy for maximum safety
/// Thread safety is caller's responsibility - use Arc<RwLock<Bitset>> when
/// needed
#[derive(Debug)]
pub struct BitsetImpl<T: BitStorage<WordType = u64>> {
    // Aligned buffer containing header + word data via unified IOBuffer abstraction
    data: IOBuffer,
    // Phantom data to maintain type safety
    _phantom: PhantomData<T>,
}

// Type aliases for convenience
use crate::bitword::{SafeBits, UnsafeBits};

/// Fast bitset with no thread safety - optimal for single-threaded use
/// Operates directly on aligned buffer for zero-copy serialization
pub type Bitset = BitsetImpl<UnsafeBits<u64>>;

/// Bitset with atomic operations on individual bits - suitable for some
/// concurrent scenarios Operates directly on aligned buffer for zero-copy
/// serialization
pub type AtomicBitset = BitsetImpl<SafeBits>;

impl<T: BitStorage<WordType = u64> + Clone + From<u64>> BitsetImpl<T> {
    const WORD_SIZE: u64 = 64;

    fn total_words(nbits: u64) -> usize { ((nbits + Self::WORD_SIZE - 1) / Self::WORD_SIZE) as usize }

    /// Create a new bitset with the specified size and ID
    pub fn new(nbits: u64, id: u64) -> Self {
        let word_count = Self::total_words(nbits);
        let total_size = std::mem::size_of::<BitsetSerialized>() + (word_count * 8);

        // Create aligned buffer using IOBuffer for unified memory management
        let mut data = IOBuffer::new(total_size);

        // Create and initialize header using zerocopy for safety
        let header = BitsetSerialized {
            magic: BITSET_MAGIC,
            version: BITSET_VERSION,
            id,
            nbits,
            skip_bits: 0,
            word_count: word_count as u64,
            set_count: 0,
        };

        // Copy header bytes safely
        let header_bytes = header.as_bytes();
        data.as_mut_slice()[..header_bytes.len()].copy_from_slice(header_bytes);

        // Words are already zero-initialized from data.resize(total_size, 0)

        Self { data, _phantom: PhantomData }
    }

    // NOTE: Deserialization moved to TryFrom<IOBuffer> to emphasize zero-copy
    // semantics.

    /// Get the aligned data (this IS the serialization - zero copy!)
    pub fn data(&self) -> &[u8] { self.data.as_slice() }

    /// Get alignment of the underlying buffer
    pub fn alignment(&self) -> usize { 4096 } // IOBuffer currently guarantees 4K alignment

    /// Get header reference using zerocopy for safe access
    fn header(&self) -> Ref<&[u8], BitsetSerialized> {
        Ref::<_, BitsetSerialized>::from_bytes(&self.data.as_slice()[..std::mem::size_of::<BitsetSerialized>()])
            .unwrap()
    }

    /// Get mutable header reference using zerocopy for safe access
    fn header_mut(&mut self) -> &mut [u8] { &mut self.data.as_mut_slice()[..std::mem::size_of::<BitsetSerialized>()] }

    /// Basic accessors that read safely using zerocopy
    pub fn get_id(&self) -> u64 { self.header().id }

    pub fn set_id(&mut self, id: u64) {
        let header_ptr = self.data.as_mut_slice().as_mut_ptr() as *mut BitsetSerialized;
        unsafe {
            (*header_ptr).id = id;
        }
    }

    pub fn size(&self) -> u64 {
        let h = self.header();
        h.nbits - h.skip_bits
    }

    fn nbits(&self) -> u64 { self.header().nbits }

    fn skip_bits(&self) -> u64 { self.header().skip_bits }

    /// Calculate the number of 64-bit words needed for the given number of bits
    fn calculate_word_count(nbits: u64) -> usize { ((nbits + 63) / 64) as usize }

    fn set_skip_bits(&mut self, skip_bits: u64) {
        let header_ptr = self.data.as_mut_slice().as_mut_ptr() as *mut BitsetSerialized;
        unsafe {
            (*header_ptr).skip_bits = skip_bits;
        }
    }

    fn word_count(&self) -> usize { self.header().word_count as usize }

    fn word_index(&self, bit: u64) -> Option<usize> {
        let offset = bit + self.skip_bits();
        if offset >= self.nbits() {
            None
        } else {
            Some((offset / Self::WORD_SIZE) as usize)
        }
    }

    fn word_offset(&self, bit: u64) -> u8 { ((bit + self.skip_bits()) % Self::WORD_SIZE) as u8 }

    /// Get word slice for safe access to word data
    fn words_slice(&self) -> &[u8] {
        let words_offset = std::mem::size_of::<BitsetSerialized>();
        &self.data.as_slice()[words_offset..]
    }

    /// Get mutable word slice for safe access to word data
    fn words_slice_mut(&mut self) -> &mut [u8] {
        let words_offset = std::mem::size_of::<BitsetSerialized>();
        &mut self.data.as_mut_slice()[words_offset..]
    }

    fn get_word(&self, word_idx: usize) -> Option<u64> {
        if word_idx < self.word_count() {
            let words_bytes = self.words_slice();
            let start = word_idx * 8;
            let end = start + 8;
            if end <= words_bytes.len() {
                let word_bytes = &words_bytes[start..end];
                Some(u64::from_le_bytes(word_bytes.try_into().unwrap()))
            } else {
                None
            }
        } else {
            None
        }
    }

    fn set_word(&mut self, word_idx: usize, value: u64) {
        if word_idx < self.word_count() {
            let words_bytes = self.words_slice_mut();
            let start = word_idx * 8;
            let end = start + 8;
            if end <= words_bytes.len() {
                words_bytes[start..end].copy_from_slice(&value.to_le_bytes());
            }
        }
    }

    pub fn get_bitval(&self, bit: u64) -> bool {
        if bit >= self.size() {
            return false;
        }

        let skip_bits = self.skip_bits();
        let actual_bit = bit + skip_bits;
        let word_idx = (actual_bit / Self::WORD_SIZE) as usize;
        let bit_offset = (actual_bit % Self::WORD_SIZE) as u8;

        if let Some(word_val) = self.get_word(word_idx) {
            (word_val & (1u64 << bit_offset)) != 0
        } else {
            false
        }
    }

    pub fn set_bit(&mut self, bit: u64) { self.set_reset_bit(bit, true); }

    pub fn reset_bit(&mut self, bit: u64) { self.set_reset_bit(bit, false); }

    fn set_reset_bit(&mut self, bit: u64, value: bool) {
        if bit >= self.size() {
            return;
        }

        let skip_bits = self.skip_bits();
        let actual_bit = bit + skip_bits;
        let word_idx = (actual_bit / Self::WORD_SIZE) as usize;
        let bit_offset = (actual_bit % Self::WORD_SIZE) as u8;

        if let Some(word_val) = self.get_word(word_idx) {
            let mask = 1u64 << bit_offset;
            let new_word = if value { word_val | mask } else { word_val & !mask };
            self.set_word(word_idx, new_word);
        }
    }

    pub fn set_bits(&mut self, start: u64, nbits: u64) { self.set_reset_bits(start, nbits, true); }

    pub fn reset_bits(&mut self, start: u64, nbits: u64) { self.set_reset_bits(start, nbits, false); }

    fn set_reset_bits(&mut self, start: u64, nbits: u64, value: bool) {
        if nbits == 0 || start >= self.size() {
            return;
        }

        let end_bit = std::cmp::min(start + nbits, self.size());
        for bit in start..end_bit {
            self.set_reset_bit(bit, value);
        }
    }

    pub fn get_next_set_bit(&self, start_bit: u64) -> u64 {
        if start_bit >= self.size() {
            return NPOS;
        }

        for bit in start_bit..self.size() {
            if self.get_bitval(bit) {
                return bit;
            }
        }

        NPOS
    }

    pub fn get_next_reset_bit(&self, start_bit: u64) -> u64 {
        if start_bit >= self.size() {
            return NPOS;
        }

        for bit in start_bit..self.size() {
            if !self.get_bitval(bit) {
                return bit;
            }
        }

        NPOS
    }

    pub fn get_set_count(&self, start_bit: u64, end_bit: Option<u64>) -> u64 {
        let end_bit = end_bit.unwrap_or(self.size() - 1);
        if start_bit > end_bit || start_bit >= self.size() {
            return 0;
        }

        let last_bit = std::cmp::min(self.size() - 1, end_bit);
        let mut count = 0;

        for bit in start_bit..=last_bit {
            if self.get_bitval(bit) {
                count += 1;
            }
        }

        count
    }

    /// Establish (update) the set_count field in the serialized header.
    /// 
    /// # Arguments
    /// * `count` - Optional count value:
    ///   - `Some(n)` - Use the provided count value directly
    ///   - `None` - Calculate count by calling get_set_count() on entire bitset
    /// 
    /// # Note
    /// This method modifies the serialized header's set_count field. The set_count
    /// is NOT automatically maintained by bitset operations and must be explicitly
    /// updated by calling this method when needed.
    pub fn establish_set_count(&mut self, count: Option<usize>) {
        let set_count = match count {
            Some(n) => n as u64,
            None => self.get_set_count(0, None),
        };

        let header_ptr = self.data.as_mut_slice().as_mut_ptr() as *mut BitsetSerialized;
        unsafe {
            (*header_ptr).set_count = set_count;
        }
    }

    pub fn is_bits_set(&self, start: u64, nbits: u64) -> bool { self.is_bits_set_reset(start, nbits, true) }

    pub fn is_bits_reset(&self, start: u64, nbits: u64) -> bool { self.is_bits_set_reset(start, nbits, false) }

    fn is_bits_set_reset(&self, start: u64, nbits: u64, expected: bool) -> bool {
        if nbits == 0 || start >= self.size() {
            return nbits == 0;
        }

        let end_bit = std::cmp::min(start + nbits, self.size());

        for bit in start..end_bit {
            if self.get_bitval(bit) != expected {
                return false;
            }
        }

        true
    }

    pub fn get_next_contiguous_n_reset_bits(&self, start_bit: u64, n: u32) -> BitBlock {
        self.get_next_contiguous_n_reset_bits_range(start_bit, None, n, n)
    }

    pub fn get_next_contiguous_n_reset_bits_range(
        &self, start_bit: u64, end_bit: Option<u64>, min_needed: u32, max_needed: u32,
    ) -> BitBlock {
        if start_bit >= self.size() {
            return BitBlock::new(NPOS, 0);
        }

        let final_bit = end_bit.unwrap_or(self.size()).min(self.size());
        let mut current_bit = start_bit;

        while current_bit < final_bit {
            if !self.get_bitval(current_bit) {
                // Found start of reset sequence
                let sequence_start = current_bit;
                let mut sequence_length = 0u32;

                // Count contiguous reset bits
                while current_bit < final_bit && !self.get_bitval(current_bit) && sequence_length < max_needed {
                    sequence_length += 1;
                    current_bit += 1;
                }

                if sequence_length >= min_needed {
                    return BitBlock::new(sequence_start, sequence_length);
                }
            } else {
                current_bit += 1;
            }
        }

        BitBlock::new(NPOS, 0)
    }

    pub fn shrink_head(&mut self, nbits: u64) -> Result<(), String> {
        if nbits > self.size() {
            return Err("Right shift out of range".to_string());
        }

        let new_skip_bits = self.skip_bits() + nbits;
        self.set_skip_bits(new_skip_bits);

        // For simplicity, we don't compact the buffer in this implementation
        // In a production version, you might want to compact when skip_bits gets large

        Ok(())
    }

    pub fn resize(&mut self, new_nbits: u64, value: bool) -> Result<(), String> {
        let old_size = self.size();
        let current_skip_bits = self.skip_bits();
        let new_total_nbits = new_nbits + current_skip_bits;

        // If shrinking or staying the same size, just update the header
        if new_total_nbits <= self.nbits() {
            // Update nbits in header directly
            let header_ptr = self.data.as_mut_slice().as_mut_ptr() as *mut BitsetSerialized;
            unsafe {
                (*header_ptr).nbits = new_total_nbits;
            }

            // Set the new bits if value is true and we're expanding within existing
            // capacity
            if value && new_nbits > old_size {
                self.set_bits(old_size, new_nbits - old_size);
            }

            return Ok(());
        }

        // Expanding: Extend the existing IOBuffer (realloc+copy for glommio path)
        let new_word_count = Self::calculate_word_count(new_total_nbits);
        let new_size = BitsetSerialized::calculate_size(new_word_count);
        let current_size = self.data.len();
        if new_size > current_size {
            self.data.resize(new_size);
        }

        // Update header fields directly
        let header_ptr = self.data.as_mut_slice().as_mut_ptr() as *mut BitsetSerialized;
        unsafe {
            (*header_ptr).nbits = new_total_nbits;
            (*header_ptr).word_count = new_word_count as u64;
        }

        // Set the new bits if value is true and we're expanding
        if value && new_nbits > old_size {
            self.set_bits(old_size, new_nbits - old_size);
        }

        Ok(())
    }

    /// Get the raw word value at the specified word index
    pub fn get_word_value(&self, word_idx: usize) -> u64 {
        if let Some(word) = self.get_word(word_idx) {
            word
        } else {
            0
        }
    }

    /// Copy logical bits from another bitset ignoring its head shift
    /// (skip_bits) with word-level efficiency.
    ///
    /// Logical bit i of `other` (which maps to physical bit
    /// other.skip_bits()+i) is copied to logical bit i of self
    /// (physical bit self.skip_bits()+i) for i in 0..min(self.size(),
    /// other.size()).
    ///
    /// Fast path: If both skip offsets have identical bit alignment (same
    /// modulo 64) we perform a mostly word-wise copy with only boundary
    /// masking. Otherwise we perform an efficient word assembly using at most
    /// two source words per destination logical word, avoiding per-bit
    /// looping.
    pub fn copy_unshifted(&mut self, other: &BitsetImpl<T>) {
        let min_bits = std::cmp::min(self.size(), other.size());
        if min_bits == 0 {
            return;
        }

        let src_skip = other.skip_bits();
        let dst_skip = self.skip_bits();
        let src_offset = (src_skip % 64) as usize;
        let dst_offset = (dst_skip % 64) as usize;

        let src_words_bytes = other.words_slice();
        let dst_words_bytes = self.words_slice_mut();

        // Helpers to read/write little-endian u64 words from the byte slices.
        fn read_word(words: &[u8], idx: usize) -> u64 {
            let base = idx * 8;
            if base + 8 <= words.len() {
                let mut arr = [0u8; 8];
                arr.copy_from_slice(&words[base..base + 8]);
                u64::from_le_bytes(arr)
            } else {
                0
            }
        }
        fn write_word(words: &mut [u8], idx: usize, val: u64) {
            let base = idx * 8;
            if base + 8 <= words.len() {
                words[base..base + 8].copy_from_slice(&val.to_le_bytes());
            }
        }

        // Fast aligned path: same intra-word offset -> direct word copy with boundary
        // masks.
        if src_offset == dst_offset {
            let logical_start_src_word = (src_skip / 64) as usize;
            let logical_start_dst_word = (dst_skip / 64) as usize;
            let logical_end_bit_exclusive = dst_skip + min_bits as u64; // physical end (exclusive)
            let last_dst_word =
                if logical_end_bit_exclusive == 0 { 0 } else { (logical_end_bit_exclusive - 1) / 64 } as usize;
            let word_count = last_dst_word - logical_start_dst_word + 1;

            for w in 0..word_count {
                let src_word = read_word(src_words_bytes, logical_start_src_word + w);
                let mut dst_word = src_word; // will mask boundaries

                // Mask leading bits before dst_skip
                if w == 0 && dst_offset != 0 {
                    let preserve_mask = (1u64 << dst_offset) - 1; // bits before dst_offset
                    let existing = read_word(dst_words_bytes, logical_start_dst_word);
                    dst_word = (dst_word & !preserve_mask) | (existing & preserve_mask);
                }
                // Mask trailing bits after logical range
                if w == word_count - 1 {
                    let end_bit = dst_skip + min_bits as u64; // exclusive
                    let end_offset_in_word = (end_bit % 64) as usize;
                    if end_offset_in_word != 0 {
                        // partial last word
                        let preserve_mask = !((1u64 << end_offset_in_word) - 1); // bits beyond logical end
                        let existing = read_word(dst_words_bytes, logical_start_dst_word + w);
                        // keep high bits beyond end_offset_in_word
                        dst_word = (dst_word & !preserve_mask) | (existing & preserve_mask);
                    }
                }
                write_word(dst_words_bytes, logical_start_dst_word + w, dst_word);
            }
            return;
        }

        // General misaligned path: build each logical destination 64-bit chunk, then
        // merge into physical destination words.
        let logical_word_count = ((min_bits + 63) / 64) as usize;
        for lw in 0..logical_word_count {
            let logical_bit_start = lw as u64 * 64;
            let bits_remaining = min_bits as u64 - logical_bit_start;
            let valid_bits = std::cmp::min(64, bits_remaining) as usize;

            // Source bit where this logical word starts (physical)
            let src_bit_start = src_skip + logical_bit_start;
            let src_word_index = (src_bit_start / 64) as usize;
            let src_bit_in_word = (src_bit_start % 64) as usize;

            let w0 = read_word(src_words_bytes, src_word_index);
            let w1 = read_word(src_words_bytes, src_word_index + 1); // 0 if out of range

            // Assemble logical word
            let mut assembled =
                if src_bit_in_word == 0 { w0 } else { (w0 >> src_bit_in_word) | (w1 << (64 - src_bit_in_word)) };
            if valid_bits < 64 {
                let mask = (1u64 << valid_bits) - 1;
                assembled &= mask;
            }

            // Destination physical placement
            let dst_bit_start = dst_skip + logical_bit_start;
            let dst_word_index = (dst_bit_start / 64) as usize;
            let dst_bit_in_word = (dst_bit_start % 64) as usize;

            if dst_bit_in_word == 0 {
                // Full (or partial last) word aligned at start.
                let existing = read_word(dst_words_bytes, dst_word_index);
                let mask = if valid_bits == 64 { !0u64 } else { (1u64 << valid_bits) - 1 };
                let new_word = (existing & !mask) | (assembled & mask);
                write_word(dst_words_bytes, dst_word_index, new_word);
            } else {
                // Split across two destination words.
                let first_bits = 64 - dst_bit_in_word; // bits fitting into first word
                let first_mask = (!0u64 >> dst_bit_in_word) << dst_bit_in_word; // bits we overwrite in first word
                let existing_first = read_word(dst_words_bytes, dst_word_index);
                let to_first = (assembled << dst_bit_in_word) & first_mask;
                let new_first = (existing_first & !first_mask) | to_first;
                write_word(dst_words_bytes, dst_word_index, new_first);

                // Remaining bits into next word if any
                if valid_bits > first_bits {
                    let remaining_bits = valid_bits - first_bits;
                    let second_mask = (1u64 << remaining_bits) - 1; // low bits
                    let existing_second = read_word(dst_words_bytes, dst_word_index + 1);
                    let to_second = (assembled >> first_bits) & second_mask;
                    let new_second = (existing_second & !second_mask) | to_second;
                    write_word(dst_words_bytes, dst_word_index + 1, new_second);
                }
            }
        }
    }

    /// Zero-copy file I/O using IOBuffer for safe memory management
    pub fn write_to_file<P: AsRef<std::path::Path>>(&self, path: P) -> Result<(), Box<dyn std::error::Error>> {
        use std::{fs::File, io::Write};

        let mut file = File::create(path)?;
        file.write_all(self.data.as_slice())?;
        file.sync_all()?;
        Ok(())
    }

    pub fn read_from_file<P: AsRef<std::path::Path>>(path: P) -> Result<Self, Box<dyn std::error::Error>> {
        use std::{fs::File, io::Read};

        let mut file = File::open(path)?;
        let file_size = file.metadata()?.len() as usize;

        let mut data = IOBuffer::new(file_size);
        file.read_exact(data.as_mut_slice())?;
        let (bitset, _set_count) = Self::load(data)?;
        Ok(bitset)
    }

    /// Get the size required for serialization (the data size)
    pub fn serialized_size(&self) -> usize { self.data.len() }

    /// Get the underlying aligned buffer for direct I/O
    pub fn buffer(&self) -> &IOBuffer { &self.data }
}

impl<T: BitStorage<WordType = u64> + Clone + From<u64>> BitsetImpl<T> {
    /// Load a bitset from an IOBuffer containing serialized bitset data
    /// 
    /// # Arguments
    /// * `data` - IOBuffer containing BitsetSerialized header followed by bit words
    /// 
    /// # Returns
    /// * `Ok((BitsetImpl, set_count))` - Successfully loaded bitset and the set_count from header
    /// * `Err(&'static str)` - Error message if validation fails
    pub fn load(data: IOBuffer) -> Result<(Self, u64), &'static str> {
        if data.len() < std::mem::size_of::<BitsetSerialized>() {
            return Err("Buffer too small");
        }
        let header_ref =
            Ref::<_, BitsetSerialized>::from_bytes(&data.as_slice()[..std::mem::size_of::<BitsetSerialized>()])
                .map_err(|_| "Invalid header format")?;
        if header_ref.magic != BITSET_MAGIC {
            return Err("Invalid magic");
        }
        if header_ref.version != BITSET_VERSION {
            return Err("Invalid version");
        }
        let expected_size = std::mem::size_of::<BitsetSerialized>() + (header_ref.word_count as usize * 8);
        if data.len() < expected_size {
            return Err("Buffer too small for data");
        }
        let set_count = header_ref.set_count;
        Ok((Self { data, _phantom: PhantomData }, set_count))
    }
}

unsafe impl<T: BitStorage<WordType = u64>> Send for BitsetImpl<T> {}
unsafe impl<T: BitStorage<WordType = u64>> Sync for BitsetImpl<T> {}

impl<T: BitStorage<WordType = u64> + Clone + From<u64>> PartialEq for BitsetImpl<T> {
    fn eq(&self, other: &Self) -> bool {
        if self.size() != other.size() {
            return false;
        }

        for bit in 0..self.size() {
            if self.get_bitval(bit) != other.get_bitval(bit) {
                return false;
            }
        }

        true
    }
}

impl<T: BitStorage<WordType = u64> + Clone + From<u64>> fmt::Display for BitsetImpl<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut output = String::new();
        if self.size() == 0 {
            return write!(f, "{}", output);
        }

        // Print bits from high to low (reverse order)
        for bit in (0..self.size()).rev() {
            output.push(if self.get_bitval(bit) { '1' } else { '0' });
        }

        write!(f, "{}", output)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bitword::{SafeBits, UnsafeBits};

    type TestBitset = BitsetImpl<UnsafeBits<u64>>;
    type TestAtomicBitset = BitsetImpl<SafeBits>;

    #[test]
    fn test_bitset_basic() {
        let mut bitset = TestBitset::new(128, 12345);
        assert_eq!(bitset.get_set_count(0, None), 0);

        bitset.set_bit(0);
        bitset.set_bit(99);
        assert!(bitset.get_bitval(0));
        assert!(bitset.get_bitval(99));
        assert!(!bitset.get_bitval(50));
        assert_eq!(bitset.get_set_count(0, None), 2);

        bitset.set_bits(10, 5);
        for i in 10..15 {
            assert!(bitset.get_bitval(i));
        }
        assert!(!bitset.get_bitval(9));
        assert!(!bitset.get_bitval(15));

        assert_eq!(bitset.get_set_count(0, None), 7);
    }

    #[test]
    fn test_bitset_next_bits() {
        let mut bitset = TestBitset::new(256, 9999);
        bitset.set_bits(10, 5);

        assert_eq!(bitset.get_next_set_bit(0), 10);
        assert_eq!(bitset.get_next_set_bit(12), 12);
        assert_eq!(bitset.get_next_set_bit(15), NPOS);

        assert_eq!(bitset.get_next_reset_bit(0), 0);
        assert_eq!(bitset.get_next_reset_bit(10), 15);
    }

    #[test]
    fn test_bitset_contiguous() {
        let mut bitset = TestBitset::new(128, 5555);
        bitset.set_bits(0, 20);
        bitset.reset_bits(30, 20);

        let result = bitset.get_next_contiguous_n_reset_bits(0, 10);
        assert_eq!(result.start_bit, 20);
        assert_eq!(result.nbits, 10);
    }

    #[test]
    fn test_zero_copy_serialization() {
        let mut original = TestBitset::new(256, 12345);
        original.set_bit(10);
        original.set_bit(100);
        original.set_bit(200);

        // Get the data (this IS the serialization)
        let data_slice = original.data().to_vec();

        // Reconstruct bitset from raw serialization (zero-copy) using load()
        let mut new_data = IOBuffer::new(data_slice.len());
        new_data.as_mut_slice().copy_from_slice(&data_slice);
        let (loaded, _set_count) = TestBitset::load(new_data).unwrap();

        // Verify all data matches
        assert_eq!(loaded.get_id(), 12345);
        assert_eq!(loaded.size(), 256);
        assert!(loaded.get_bitval(10));
        assert!(loaded.get_bitval(100));
        assert!(loaded.get_bitval(200));
        assert!(!loaded.get_bitval(11));
    }

    #[test]
    fn test_zero_copy_file_io() {
        use std::fs;

        let temp_path = "/tmp/test_zero_copy_bitset.dat";

        // Create and populate bitset
        let mut original = TestBitset::new(128, 9999);
        original.set_bit(0);
        original.set_bit(64);
        original.set_bit(127);

        // Write to file (zero-copy)
        original.write_to_file(temp_path).unwrap();

        // Read from file (zero-copy)
        let loaded = TestBitset::read_from_file(temp_path).unwrap();

        // Verify
        assert_eq!(loaded.get_id(), 9999);
        assert_eq!(loaded.size(), 128);
        assert!(loaded.get_bitval(0));
        assert!(loaded.get_bitval(64));
        assert!(loaded.get_bitval(127));
        assert!(!loaded.get_bitval(1));

        // Clean up
        let _ = fs::remove_file(temp_path);
    }

    #[test]
    fn test_atomic_bitset() {
        let mut bitset = TestAtomicBitset::new(64, 42);

        // Basic operations should work the same
        bitset.set_bit(10);
        bitset.set_bit(63);
        assert_eq!(bitset.get_next_set_bit(0), 10);
        assert_eq!(bitset.get_next_set_bit(11), 63);
        assert_eq!(bitset.get_id(), 42);

        assert_eq!(bitset.size(), 64);

        // Original bits should still be set
        assert_eq!(bitset.get_next_set_bit(0), 10);
        assert_eq!(bitset.get_next_set_bit(11), 63);
    }

    #[test]
    fn test_bitset_resize_expansion() {
        let mut bitset = TestBitset::new(64, 1);

        // Set some bits in the original range
        bitset.set_bit(5);
        bitset.set_bit(10);
        bitset.set_bit(63);

        assert_eq!(bitset.size(), 64);
        assert!(bitset.get_bitval(5));
        assert!(bitset.get_bitval(10));
        assert!(bitset.get_bitval(63));

        // Expand to 128 bits
        bitset.resize(128, false).unwrap();

        assert_eq!(bitset.size(), 128);

        // Original bits should still be set
        assert!(bitset.get_bitval(5));
        assert!(bitset.get_bitval(10));
        assert!(bitset.get_bitval(63));

        // New bits should be clear (value=false)
        assert!(!bitset.get_bitval(64));
        assert!(!bitset.get_bitval(100));
        assert!(!bitset.get_bitval(127));

        // Set a bit in the new range
        bitset.set_bit(100);
        assert!(bitset.get_bitval(100));

        // Test expansion with value=true
        bitset.resize(192, true).unwrap();
        assert_eq!(bitset.size(), 192);

        // Original bits should still be set
        assert!(bitset.get_bitval(5));
        assert!(bitset.get_bitval(10));
        assert!(bitset.get_bitval(63));
        assert!(bitset.get_bitval(100));

        // New bits should be set (value=true)
        assert!(bitset.get_bitval(128));
        assert!(bitset.get_bitval(150));
        assert!(bitset.get_bitval(191));
    }

    #[test]
    fn test_bitset_efficient_expansion() {
        // Test that expansion is efficient and preserves memory layout
        let mut bitset = TestBitset::new(64, 42);

        // Set a pattern of bits
        for i in (0..64).step_by(5) {
            bitset.set_bit(i);
        }

        let original_set_count = bitset.get_set_count(0, None);
        let original_id = bitset.get_id();

        // Expand multiple times to test efficiency
        bitset.resize(128, false).unwrap();
        bitset.resize(256, false).unwrap();
        bitset.resize(1024, false).unwrap();

        // Verify all original data is preserved
        assert_eq!(bitset.size(), 1024);
        assert_eq!(bitset.get_id(), original_id);
        assert_eq!(bitset.get_set_count(0, Some(64)), original_set_count);

        // Verify the original pattern is still intact
        for i in (0..64).step_by(5) {
            assert!(bitset.get_bitval(i), "Bit {} should still be set", i);
        }

        // Verify new space is available
        bitset.set_bit(500);
        bitset.set_bit(1000);
        assert!(bitset.get_bitval(500));
        assert!(bitset.get_bitval(1000));
    }
}
