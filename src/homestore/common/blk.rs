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
    cmp::Ordering,
    fmt,
    hash::{Hash, Hasher},
};

use smallvec::SmallVec;

// Type aliases matching C++ definitions
pub type ChunkNum = u16;
pub type BlkCount = u16;
pub type BlkNum = u32;
pub type BlkTemp = u32;
pub type AllocatorId = u16;
pub type StreamId = u64;

/// Type alias for BlkId collection that can hold up to 4 BlkIds inline
pub type BlkIds = SmallVec<[BlkId; 4]>;

// Constants
pub const fn max_addressable_chunks() -> usize { 1usize << (8 * std::mem::size_of::<ChunkNum>()) }

pub const fn max_blks_per_chunk() -> usize { 1usize << (8 * std::mem::size_of::<BlkNum>()) }

pub const fn max_blks_per_blkid() -> usize { (1usize << (8 * std::mem::size_of::<BlkCount>())) - 1 }

/// Serialized representation of BlkId
#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
struct BlkIdSerialized {
    // Bitfield: is_multi (1 bit) + blk_num (31 bits)
    blk_num_with_flag: u32,
    nblks: BlkCount,
    chunk_num: ChunkNum,
}

impl BlkIdSerialized {
    fn new(is_multi: bool, blk_num: BlkNum, nblks: BlkCount, chunk_num: ChunkNum) -> Self {
        let flag = if is_multi { 1u32 } else { 0u32 };
        let blk_num_with_flag = (blk_num & 0x7FFFFFFF) | (flag << 31);
        Self { blk_num_with_flag, nblks, chunk_num }
    }

    fn is_multi(&self) -> bool { (self.blk_num_with_flag & 0x80000000) != 0 }

    fn blk_num(&self) -> BlkNum { self.blk_num_with_flag & 0x7FFFFFFF }

    fn blk_count(&self) -> BlkCount { self.nblks }

    fn chunk_num(&self) -> ChunkNum { self.chunk_num }
}

impl Default for BlkIdSerialized {
    fn default() -> Self { Self { blk_num_with_flag: 0, nblks: 0, chunk_num: 0 } }
}

/// Block Identifier
/// Represents a contiguous range of blocks within a chunk
#[derive(Clone, Copy, Debug)]
pub struct BlkId {
    s: BlkIdSerialized,
}

impl BlkId {
    /// Create a new BlkId from an integer representation
    pub fn from_integer(id_int: u64) -> Self {
        let s = unsafe { std::mem::transmute::<u64, BlkIdSerialized>(id_int) };
        debug_assert!(!s.is_multi(), "MultiBlkId is set on BlkId constructor");
        Self { s }
    }

    /// Create a new BlkId with specified parameters
    pub fn new(blk_num: BlkNum, nblks: BlkCount, chunk_num: ChunkNum) -> Self {
        Self { s: BlkIdSerialized::new(false, blk_num, nblks, chunk_num) }
    }

    /// Get the block number
    pub fn blk_num(&self) -> BlkNum { self.s.blk_num() }

    /// Get the block count
    pub fn blk_count(&self) -> BlkCount { self.s.blk_count() }

    /// Get the chunk number
    pub fn chunk_num(&self) -> ChunkNum { self.s.chunk_num() }

    /// Check if this is a multi-block ID
    pub fn is_multi(&self) -> bool { self.s.is_multi() }

    /// Convert to integer representation
    pub fn to_integer(&self) -> u64 { unsafe { std::mem::transmute::<BlkIdSerialized, u64>(self.s) } }

    /// Serialize to bytes (returns a slice, zero allocation and zero copy)
    /// Returns a direct pointer to the internal structure's memory
    pub fn serialize(&self) -> &[u8] {
        unsafe {
            std::slice::from_raw_parts(
                &self.s as *const BlkIdSerialized as *const u8,
                std::mem::size_of::<BlkIdSerialized>(),
            )
        }
    }

    /// Get serialized size
    pub fn serialized_size(&self) -> usize { std::mem::size_of::<BlkId>() }

    /// Expected serialized size
    pub fn expected_serialized_size() -> usize { std::mem::size_of::<BlkId>() }

    /// Deserialize from bytes
    pub fn deserialize(bytes: &[u8]) -> Result<Self, &'static str> {
        if bytes.len() < std::mem::size_of::<BlkIdSerialized>() {
            return Err("Insufficient bytes for deserialization");
        }
        let s = unsafe { std::ptr::read_unaligned(bytes.as_ptr() as *const BlkIdSerialized) };
        Ok(Self { s })
    }

    /// Invalidate the block ID
    pub fn invalidate(&mut self) { self.s.nblks = 0; }

    /// Check if the block ID is valid
    pub fn is_valid(&self) -> bool { self.blk_count() > 0 }

    /// Split the block ID at the specified count
    pub fn split(&self, count: BlkCount) -> (BlkId, BlkId) {
        let lb = BlkId::new(self.blk_num(), count, self.chunk_num());
        let rb = BlkId::new(self.blk_num() + count as BlkNum, self.blk_count() - count, self.chunk_num());
        (lb, rb)
    }

    /// Compare two BlkIds
    pub fn compare(one: &BlkId, two: &BlkId) -> Ordering {
        match one.chunk_num().cmp(&two.chunk_num()) {
            Ordering::Equal => {}
            other => return other,
        }

        match one.blk_num().cmp(&two.blk_num()) {
            Ordering::Equal => {}
            other => return other,
        }

        one.blk_count().cmp(&two.blk_count())
    }
}

impl Default for BlkId {
    fn default() -> Self { Self { s: BlkIdSerialized::default() } }
}

impl fmt::Display for BlkId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_valid() {
            write!(f, "blk#={} count={} chunk={}", self.blk_num(), self.blk_count(), self.chunk_num())
        } else {
            write!(f, "Invalid_Blkid")
        }
    }
}

impl PartialEq for BlkId {
    fn eq(&self, other: &Self) -> bool { BlkId::compare(self, other) == Ordering::Equal }
}

impl Eq for BlkId {}

impl PartialOrd for BlkId {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> { Some(BlkId::compare(self, other)) }
}

impl Ord for BlkId {
    fn cmp(&self, other: &Self) -> Ordering { BlkId::compare(self, other) }
}

impl Hash for BlkId {
    fn hash<H: Hasher>(&self, state: &mut H) { self.to_integer().hash(state); }
}

/// Chain block ID for MultiBlkId
#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default)]
struct ChainBlkId {
    blk_num: BlkNum,
    nblks: BlkCount,
}

impl ChainBlkId {
    #[allow(dead_code)]
    fn is_valid(&self) -> bool { self.nblks != 0 }
}

/// Multi-piece Block Identifier
/// Represents multiple non-contiguous block ranges within the same chunk
#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
pub struct MultiBlkId {
    base: BlkId,
    n_addln_piece: u16,
    addln_pieces: [ChainBlkId; Self::MAX_ADDLN_PIECES],
}

impl MultiBlkId {
    pub const MAX_ADDLN_PIECES: usize = 5;
    pub const MAX_PIECES: usize = Self::MAX_ADDLN_PIECES + 1;

    /// Create a new empty MultiBlkId
    pub fn new() -> Self {
        let mut base = BlkId::default();
        base.s.blk_num_with_flag |= 1u32 << 31; // Set is_multi flag
        Self { base, n_addln_piece: 0, addln_pieces: [ChainBlkId::default(); Self::MAX_ADDLN_PIECES] }
    }

    /// Create from a single BlkId
    pub fn from_blkid(b: BlkId) -> Self {
        let mut base = b;
        base.s.blk_num_with_flag |= 1u32 << 31; // Set is_multi flag
        Self { base, n_addln_piece: 0, addln_pieces: [ChainBlkId::default(); Self::MAX_ADDLN_PIECES] }
    }

    /// Create with initial block parameters
    pub fn with_params(blk_num: BlkNum, nblks: BlkCount, chunk_num: ChunkNum) -> Self {
        let mut base = BlkId::new(blk_num, nblks, chunk_num);
        base.s.blk_num_with_flag |= 1u32 << 31; // Set is_multi flag
        Self { base, n_addln_piece: 0, addln_pieces: [ChainBlkId::default(); Self::MAX_ADDLN_PIECES] }
    }

    /// Add a new block piece
    pub fn add(&mut self, blk_num: BlkNum, nblks: BlkCount, chunk_num: ChunkNum) {
        if self.base.is_valid() {
            assert_eq!(self.base.chunk_num(), chunk_num, "MultiBlkId has to be all from same chunk");
            assert!(
                (self.n_addln_piece as usize) < Self::MAX_ADDLN_PIECES,
                "MultiBlkId cannot support more than {} pieces",
                Self::MAX_PIECES
            );
            self.addln_pieces[self.n_addln_piece as usize] = ChainBlkId { blk_num, nblks };
            self.n_addln_piece += 1;
        } else {
            self.base = BlkId::new(blk_num, nblks, chunk_num);
            self.base.s.blk_num_with_flag |= 1u32 << 31; // Set is_multi flag
        }
    }

    /// Add a BlkId
    pub fn add_blkid(&mut self, b: &BlkId) { self.add(b.blk_num(), b.blk_count(), b.chunk_num()); }

    /// Serialize to bytes (returns a slice, zero allocation and zero copy)
    /// Returns a direct pointer to the internal structure's memory
    pub fn serialize(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self as *const MultiBlkId as *const u8, self.serialized_size()) }
    }

    /// Get serialized size
    pub fn serialized_size(&self) -> usize {
        let mut sz = BlkId::expected_serialized_size();
        if self.n_addln_piece != 0 {
            sz += std::mem::size_of::<u16>() + (self.n_addln_piece as usize * std::mem::size_of::<ChainBlkId>());
        }
        sz
    }

    /// Deserialize from bytes
    pub fn deserialize(bytes: &[u8]) -> Result<Self, &'static str> {
        let base = BlkId::deserialize(bytes)?;
        let mut result = Self::from_blkid(base);

        if bytes.len() == std::mem::size_of::<BlkId>() {
            result.n_addln_piece = 0;
        } else {
            let offset = std::mem::size_of::<BlkId>();
            if bytes.len() < offset + std::mem::size_of::<u16>() {
                return Err("Insufficient bytes for n_addln_piece");
            }

            let n_addln_piece = u16::from_le_bytes([bytes[offset], bytes[offset + 1]]);
            result.n_addln_piece = n_addln_piece;

            let mut offset = offset + std::mem::size_of::<u16>();
            for i in 0..n_addln_piece as usize {
                if bytes.len() < offset + std::mem::size_of::<ChainBlkId>() {
                    return Err("Insufficient bytes for chain_blkid");
                }

                let blk_num =
                    u32::from_le_bytes([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]]);
                offset += 4;

                let nblks = u16::from_le_bytes([bytes[offset], bytes[offset + 1]]);
                offset += 2;

                result.addln_pieces[i] = ChainBlkId { blk_num, nblks };
            }
        }

        Ok(result)
    }

    /// Expected serialized size for a given number of pieces
    pub fn expected_serialized_size(num_pieces: u16) -> usize {
        let mut sz = BlkId::expected_serialized_size();
        if num_pieces > 1 {
            sz += std::mem::size_of::<u16>() + ((num_pieces - 1) as usize * std::mem::size_of::<ChainBlkId>());
        }
        sz
    }

    /// Maximum serialized size
    pub fn max_serialized_size() -> usize { Self::expected_serialized_size(Self::MAX_PIECES as u16) }

    /// Get number of pieces
    pub fn num_pieces(&self) -> u16 {
        if self.base.is_valid() {
            self.n_addln_piece + 1
        } else {
            0
        }
    }

    /// Check if there's room for more pieces
    pub fn has_room(&self) -> bool { (self.n_addln_piece as usize) < Self::MAX_ADDLN_PIECES }

    /// Split at the specified count
    pub fn split(&self, count: BlkCount) -> (MultiBlkId, MultiBlkId) {
        let lb = MultiBlkId::with_params(self.base.blk_num(), count, self.base.chunk_num());
        let rb = MultiBlkId::with_params(
            self.base.blk_num() + count as BlkNum,
            self.base.blk_count() - count,
            self.base.chunk_num(),
        );
        (lb, rb)
    }

    /// Get the total block count across all pieces
    pub fn blk_count(&self) -> BlkCount {
        let mut nblks: u32 = 0;
        for blkid in self.iter() {
            nblks += blkid.blk_count() as u32;
        }
        nblks as BlkCount
    }

    /// Get chunk number
    pub fn chunk_num(&self) -> ChunkNum { self.base.chunk_num() }

    /// Convert to single BlkId (only valid if num_pieces <= 1)
    pub fn to_single_blkid(&self) -> BlkId {
        debug_assert!(self.num_pieces() <= 1, "Can only convert MultiBlkId with one piece to BlkId");
        BlkId::new(self.base.blk_num(), self.base.blk_count(), self.base.chunk_num())
    }

    /// Create an iterator over the pieces
    pub fn iter(&self) -> MultiBlkIdIterator<'_> { MultiBlkIdIterator { mbid: self, next_blk: 0 } }

    /// Compare two MultiBlkIds
    pub fn compare(left: &MultiBlkId, right: &MultiBlkId) -> Ordering {
        match left.chunk_num().cmp(&right.chunk_num()) {
            Ordering::Equal => {}
            other => return other,
        }

        // Shortcut path for simple BlkId search
        if left.num_pieces() == 1 && right.num_pieces() == 1 {
            return BlkId::compare(&left.base, &right.base);
        }

        // For complex comparison, we would need interval set comparison
        // This is a simplified version that compares piece by piece
        match left.num_pieces().cmp(&right.num_pieces()) {
            Ordering::Equal => {}
            other => return other,
        }

        // Compare base blocks
        match BlkId::compare(&left.base, &right.base) {
            Ordering::Equal => {}
            other => return other,
        }

        // Compare additional pieces
        for i in 0..left.n_addln_piece.min(right.n_addln_piece) as usize {
            // Copy values to avoid unaligned access warnings
            let left_blk_num = left.addln_pieces[i].blk_num;
            let right_blk_num = right.addln_pieces[i].blk_num;
            let left_nblks = left.addln_pieces[i].nblks;
            let right_nblks = right.addln_pieces[i].nblks;

            match left_blk_num.cmp(&right_blk_num) {
                Ordering::Equal => {}
                other => return other,
            }
            match left_nblks.cmp(&right_nblks) {
                Ordering::Equal => {}
                other => return other,
            }
        }

        Ordering::Equal
    }
}

impl Default for MultiBlkId {
    fn default() -> Self { Self::new() }
}

impl fmt::Display for MultiBlkId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "[")?;
        for blkid in self.iter() {
            write!(f, "{{{}}},", blkid)?;
        }
        write!(f, "]")
    }
}

impl PartialEq for MultiBlkId {
    fn eq(&self, other: &Self) -> bool { MultiBlkId::compare(self, other) == Ordering::Equal }
}

impl Eq for MultiBlkId {}

impl PartialOrd for MultiBlkId {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> { Some(MultiBlkId::compare(self, other)) }
}

impl Ord for MultiBlkId {
    fn cmp(&self, other: &Self) -> Ordering { MultiBlkId::compare(self, other) }
}

impl Hash for MultiBlkId {
    fn hash<H: Hasher>(&self, state: &mut H) {
        const START_SEED: u64 = 0xB504F333;
        let mut seed = START_SEED;
        for blkid in self.iter() {
            // Simple hash combine similar to boost::hash_combine
            seed ^= blkid.to_integer().wrapping_add(0x9e3779b9).wrapping_add(seed << 6).wrapping_add(seed >> 2);
        }
        seed.hash(state);
    }
}

/// Iterator for MultiBlkId pieces
pub struct MultiBlkIdIterator<'a> {
    mbid: &'a MultiBlkId,
    next_blk: u16,
}

impl<'a> Iterator for MultiBlkIdIterator<'a> {
    type Item = BlkId;

    fn next(&mut self) -> Option<Self::Item> {
        if self.next_blk == 0 {
            self.next_blk += 1;
            if self.mbid.base.is_valid() {
                Some(BlkId::new(self.mbid.base.blk_num(), self.mbid.base.blk_count(), self.mbid.base.chunk_num()))
            } else {
                None
            }
        } else if self.next_blk < self.mbid.num_pieces() {
            let idx = (self.next_blk - 1) as usize;
            let cbid = &self.mbid.addln_pieces[idx];
            self.next_blk += 1;
            Some(BlkId::new(cbid.blk_num, cbid.nblks, self.mbid.base.chunk_num()))
        } else {
            None
        }
    }
}

/// Block allocation status
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlkAllocStatus {
    None = 0,               // No Action taken
    Success = 1 << 0,       // Success
    Failed = 1 << 1,        // Failed to alloc/free
    ReqMore = 1 << 2,       // Indicate that we need more
    SpaceFull = 1 << 3,     // Space is full
    InvalidDev = 1 << 4,    // Invalid Device provided for alloc
    Partial = 1 << 5,       // In case of multiple blks, only partial is alloced/freed
    InvalidThread = 1 << 6, // Not possible to alloc in this thread
    InvalidInput = 1 << 7,  // Invalid input
    TooManyPieces = 1 << 8, // Allocation results in more pieces than passed on
}

/// Block allocation hints
#[derive(Debug, Clone)]
pub struct BlkAllocHints {
    pub desired_temp: BlkTemp,
    pub reserved_blks: Option<u32>,
    pub pdev_id_hint: Option<u32>,
    pub chunk_id_hint: Option<ChunkNum>,
    pub application_hint: Option<u64>,
    pub can_look_for_other_chunk: bool,
    pub is_contiguous: bool,
    pub partial_alloc_ok: bool,
    pub bypass_slab_cache: bool,
    pub min_blks_per_piece: u32,
    pub max_blks_per_piece: u32,
}

impl Default for BlkAllocHints {
    fn default() -> Self {
        Self {
            desired_temp: 0,
            reserved_blks: None,
            pdev_id_hint: None,
            chunk_id_hint: None,
            application_hint: None,
            can_look_for_other_chunk: true,
            is_contiguous: true,
            partial_alloc_ok: false,
            bypass_slab_cache: false,
            min_blks_per_piece: 1,
            max_blks_per_piece: max_blks_per_blkid() as u32,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_blkid_creation() {
        let blkid = BlkId::new(100, 50, 1);
        assert_eq!(blkid.blk_num(), 100);
        assert_eq!(blkid.blk_count(), 50);
        assert_eq!(blkid.chunk_num(), 1);
        assert!(blkid.is_valid());
    }

    #[test]
    fn test_blkid_serialization() {
        let blkid = BlkId::new(100, 50, 1);
        // Test that serialize returns &[u8] (zero allocation, zero copy)
        let bytes: &[u8] = blkid.serialize();
        assert_eq!(bytes.len(), 8);

        let deserialized = BlkId::deserialize(bytes).unwrap();
        assert_eq!(blkid, deserialized);
    }

    #[test]
    fn test_blkid_split() {
        let blkid = BlkId::new(100, 50, 1);
        let (left, right) = blkid.split(20);
        assert_eq!(left.blk_num(), 100);
        assert_eq!(left.blk_count(), 20);
        assert_eq!(right.blk_num(), 120);
        assert_eq!(right.blk_count(), 30);
    }

    #[test]
    fn test_blkid_to_integer() {
        let blkid = BlkId::new(100, 50, 1);
        let int_val = blkid.to_integer();
        let from_int = BlkId::from_integer(int_val);
        assert_eq!(blkid, from_int);
    }

    #[test]
    fn test_multiblkid_creation() {
        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        mblkid.add(200, 30, 1);
        assert_eq!(mblkid.num_pieces(), 2);
        assert_eq!(mblkid.blk_count(), 80);
    }

    #[test]
    fn test_multiblkid_serialization() {
        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        mblkid.add(200, 30, 1);

        // Test that serialize returns &[u8] (zero allocation, zero copy)
        let bytes: &[u8] = mblkid.serialize();
        let expected_size =
            std::mem::size_of::<BlkId>() + std::mem::size_of::<u16>() + std::mem::size_of::<ChainBlkId>();
        assert_eq!(bytes.len(), expected_size);

        let deserialized = MultiBlkId::deserialize(bytes).unwrap();
        assert_eq!(mblkid.num_pieces(), deserialized.num_pieces());
        assert_eq!(mblkid.blk_count(), deserialized.blk_count());
    }

    #[test]
    fn test_multiblkid_iterator() {
        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        mblkid.add(200, 30, 1);

        let pieces: Vec<BlkId> = mblkid.iter().collect();
        assert_eq!(pieces.len(), 2);
        assert_eq!(pieces[0].blk_num(), 100);
        assert_eq!(pieces[0].blk_count(), 50);
        assert_eq!(pieces[1].blk_num(), 200);
        assert_eq!(pieces[1].blk_count(), 30);
    }

    #[test]
    fn test_multiblkid_single_piece() {
        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        assert_eq!(mblkid.num_pieces(), 1);

        let single = mblkid.to_single_blkid();
        assert_eq!(single.blk_num(), 100);
        assert_eq!(single.blk_count(), 50);
        assert_eq!(single.chunk_num(), 1);
    }

    #[test]
    fn test_blkid_compare() {
        let blkid1 = BlkId::new(100, 50, 1);
        let blkid2 = BlkId::new(100, 50, 1);
        let blkid3 = BlkId::new(200, 50, 1);

        assert_eq!(blkid1, blkid2);
        assert!(blkid1 < blkid3);
        assert!(blkid3 > blkid1);
    }

    #[test]
    fn test_multiblkid_display() {
        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        mblkid.add(200, 30, 1);

        let display_str = format!("{}", mblkid);
        assert!(display_str.contains("blk#=100"));
        assert!(display_str.contains("blk#=200"));
    }

    #[test]
    fn test_zero_allocation_serialization() {
        // This test verifies that serialize returns &[u8] (zero allocation, zero copy)
        // Just a raw pointer and size - no heap allocation at all
        let blkid = BlkId::new(100, 50, 1);
        let bytes: &[u8] = blkid.serialize();
        assert_eq!(bytes.len(), 8);

        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        let bytes: &[u8] = mblkid.serialize();
        assert!(bytes.len() > 0);

        // &[u8] is just a pointer + length - no allocation
        // Can be copied without allocating (copies pointer, not data)
        let bytes_copy = bytes;
        assert_eq!(bytes.len(), bytes_copy.len());
    }

    #[test]
    fn test_serialize_returns_raw_pointer() {
        // Verify that serialize() returns &[u8] pointing to the struct itself
        // This is true zero allocation - just a pointer to existing memory
        let blkid = BlkId::new(100, 50, 1);
        let bytes = blkid.serialize();

        // Verify the slice has correct length
        assert_eq!(bytes.len(), std::mem::size_of::<BlkId>());

        // Verify the slice points to the BlkId's memory
        let blkid_addr = &blkid as *const BlkId as usize;
        let slice_addr = bytes.as_ptr() as usize;
        assert_eq!(slice_addr, blkid_addr);

        // Verify MultiBlkId serialization
        let mut mblkid = MultiBlkId::new();
        mblkid.add(100, 50, 1);
        mblkid.add(200, 30, 1);

        let bytes = mblkid.serialize();
        assert_eq!(bytes.len(), mblkid.serialized_size());

        // Verify the slice points to the MultiBlkId's memory
        let mblkid_addr = &mblkid as *const MultiBlkId as usize;
        let slice_addr = bytes.as_ptr() as usize;
        assert_eq!(slice_addr, mblkid_addr);

        // &[u8] can be sliced using standard Rust slice operations
        let slice = &bytes[0..8];
        assert_eq!(slice.len(), 8);
    }
}
