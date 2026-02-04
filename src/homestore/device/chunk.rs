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

use crc::{Crc, Algorithm};

/// CRC-16 T10 DIF algorithm (polynomial 0x8BB7, also known as CRC-16-T10-DIF)
const CRC16_T10DIF_ALGO: Algorithm<u16> = Algorithm {
    width: 16,
    poly: 0x8bb7,
    init: 0x0000,
    refin: false,
    refout: false,
    xorout: 0x0000,
    check: 0x0000,
    residue: 0x0000,
};

const CRC16_T10DIF: Crc<u16> = Crc::<u16>::new(&CRC16_T10DIF_ALGO);

/// Chunk info structure - stored on disk
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct ChunkInfo {
    pub chunk_start_offset: u64, // Start offset of the chunk within a pdev
    pub chunk_size: u64,          // Chunk size
    pub vdev_id: u32,             // Virtual device id this chunk hosts. u32::MAX if chunk is free
    pub chunk_id: u32,            // ID for this chunk - unique for entire homestore
    pub chunk_creation_order: u32, // Creation order - sequential order in which chunks were created
    pub stream_id: u64,           // Stream ID this chunk belongs to (0 = unassigned/default)
    pub chunk_allocated: u8,      // Is chunk allocated or free (0x00 = free, 0x01 = allocated)
    pub checksum: u16,            // checksum of this chunk info
    // Pad the header to a fixed 512 bytes (reduced from 289 to 281 for stream_id)
    pub padding: [u8; 281],
    pub chunk_selector_private: [u8; 64], // Chunk selector private area
    pub user_private: [u8; 128],  // Opaque user of the chunk information
}

impl ChunkInfo {
    pub const SIZE: usize = 512;
    pub const USER_PRIVATE_SIZE: usize = 128;
    pub const SELECTOR_PRIVATE_SIZE: usize = 64;

    pub fn new() -> Self {
        Self {
            chunk_start_offset: 0,
            chunk_size: 0,
            vdev_id: 0,
            chunk_id: 0,
            chunk_creation_order: 0,
            stream_id: 0,  // 0 = unassigned/default
            chunk_allocated: 0x00,
            checksum: 0,
            padding: [0; 281],
            chunk_selector_private: [0; 64],
            user_private: [0; 128],
        }
    }

    pub fn is_allocated(&self) -> bool {
        self.chunk_allocated != 0x00
    }

    pub fn set_allocated(&mut self) {
        self.chunk_allocated = 0x01;
    }

    pub fn set_free(&mut self) {
        self.chunk_allocated = 0x00;
    }

    pub fn set_selector_private(&mut self, data: &[u8]) {
        let len = std::cmp::min(data.len(), Self::SELECTOR_PRIVATE_SIZE);
        self.chunk_selector_private[..len].copy_from_slice(&data[..len]);
    }

    pub fn set_user_private(&mut self, data: &[u8]) {
        if !data.is_empty() {
            let len = std::cmp::min(data.len(), Self::USER_PRIVATE_SIZE);
            self.user_private[..len].copy_from_slice(&data[..len]);
        }
    }

    /// Check if chunk is assigned to a stream
    pub fn has_stream(&self) -> bool {
        self.stream_id != 0
    }
    
    /// Get the stream ID (0 if unassigned)
    pub fn get_stream_id(&self) -> u64 {
        self.stream_id
    }
    
    /// Set the stream ID
    pub fn set_stream_id(&mut self, stream_id: u64) {
        self.stream_id = stream_id;
    }

    pub fn compute_checksum(&mut self) {
        self.checksum = 0;
        let bytes = unsafe {
            std::slice::from_raw_parts(
                self as *const Self as *const u8,
                std::mem::size_of::<ChunkInfo>(),
            )
        };
        self.checksum = CRC16_T10DIF.checksum(bytes);
    }
}

impl Default for ChunkInfo {
    fn default() -> Self {
        Self::new()
    }
}

use std::cell::UnsafeCell;
use std::sync::Arc;

/// Chunk represents a contiguous region of storage on a physical device
pub struct Chunk {
    pub chunk_info: ChunkInfo,
    pub chunk_slot: u32,
    pdev: Arc<super::physical_dev::PhysicalDev>,
    blk_allocator: UnsafeCell<Option<Box<dyn crate::blkalloc::BlkAllocator>>>,
}

impl Chunk {
    pub const MAX_CHUNK_SIZE: u32 = u32::MAX;

    pub fn new(chunk_info: ChunkInfo, chunk_slot: u32, pdev: Arc<super::physical_dev::PhysicalDev>) -> Self {
        Self {
            chunk_info,
            chunk_slot,
            pdev,
            blk_allocator: UnsafeCell::new(None),
        }
    }

    /// Get reference to the physical device this chunk belongs to
    pub fn physical_dev(&self) -> &Arc<super::physical_dev::PhysicalDev> {
        &self.pdev
    }

    /// Get the start offset of this chunk within the physical device
    pub fn start_offset(&self) -> u64 {
        self.chunk_info.chunk_start_offset
    }

    /// Set the block allocator for this chunk
    /// SAFETY: This should only be called once during initialization
    pub fn set_block_allocator(&self, allocator: Box<dyn crate::blkalloc::BlkAllocator>) {
        unsafe {
            *self.blk_allocator.get() = Some(allocator);
        }
    }

    /// Check if block allocator is set
    pub fn has_blk_allocator(&self) -> bool {
        unsafe { (*self.blk_allocator.get()).is_some() }
    }

    /// Get immutable reference to the block allocator
    /// 
    /// # Panics
    /// Panics if block allocator has not been set (programming error)
    /// 
    /// # Safety
    /// Caller must ensure no mutable access is occurring
    #[inline]
    pub fn blk_allocator<'a>(&'a self) -> &'a dyn crate::blkalloc::BlkAllocator {
        unsafe {
            (*self.blk_allocator.get())
                .as_ref()
                .map(|ba| &**ba as &'a dyn crate::blkalloc::BlkAllocator)
                .expect("Block allocator not initialized")
        }
    }

    /// Get mutable reference to the block allocator
    /// 
    /// # Panics
    /// Panics if block allocator has not been set (programming error)
    /// 
    /// # Safety
    /// Caller must ensure exclusive access
    #[inline]
    pub fn blk_allocator_mut<'a>(&'a self) -> &'a mut dyn crate::blkalloc::BlkAllocator {
        unsafe {
            (*self.blk_allocator.get())
                .as_mut()
                .map(|ba| &mut **ba as &'a mut dyn crate::blkalloc::BlkAllocator)
                .expect("Block allocator not initialized")
        }
    }

    pub fn info(&self) -> &ChunkInfo {
        &self.chunk_info
    }
    
    /// Update chunk info in-place (used during reactivation from pool)
    /// 
    /// SAFETY: This is safe because:
    /// 1. Called only during reactivation when chunk is not in active use
    /// 2. Protected by VirtualDev's chunk_mgmt_mutex
    pub(crate) fn update_info(&self, new_info: ChunkInfo) {
        unsafe {
            let info_ptr = &self.chunk_info as *const ChunkInfo as *mut ChunkInfo;
            *info_ptr = new_info;
        }
    }

    pub fn slot_number(&self) -> u32 {
        self.chunk_slot
    }

    pub fn chunk_id(&self) -> u32 {
        self.chunk_info.chunk_id
    }

    /// Get creation order of this chunk
    /// 
    /// Creation order represents the sequential order in which chunks were created.
    /// This is used to maintain logical ordering of chunks within a VirtualDev.
    pub fn creation_order(&self) -> u32 {
        self.chunk_info.chunk_creation_order
    }

    pub fn to_string(&self) -> String {
        // Copy packed fields to avoid unaligned references
        let chunk_id = self.chunk_info.chunk_id;
        let chunk_start_offset = self.chunk_info.chunk_start_offset;
        let chunk_size = self.chunk_info.chunk_size;
        
        format!(
            "Chunk[id={}, slot={}, offset={}, size={}]",
            chunk_id,
            self.chunk_slot,
            chunk_start_offset,
            chunk_size
        )
    }
}

// Safety: Chunk is Sync because:
// 1. blk_allocator is only set once during initialization (set_block_allocator)
// 2. After initialization, it's read-only (accessed via blk_allocator())
// 3. The UnsafeCell is used for interior mutability during init, not concurrent mutation
// 4. The BlkAllocator trait requires Send + Sync
unsafe impl Sync for Chunk {}

//
// ================ Chunk Interval Management ================
//

use std::collections::BTreeSet;
use std::ops::Range;

/// Chunk interval type for tracking allocated chunk areas
pub type ChunkInterval = Range<u64>;

/// Set of chunk intervals using BTreeSet for efficient range queries
#[derive(Debug, Clone)]
pub struct ChunkIntervalSet {
    intervals: BTreeSet<(u64, u64)>,
}

impl ChunkIntervalSet {
    pub fn new() -> Self {
        Self {
            intervals: BTreeSet::new(),
        }
    }

    pub fn insert(&mut self, range: ChunkInterval) {
        self.intervals.insert((range.start, range.end));
    }

    pub fn erase(&mut self, range: ChunkInterval) {
        self.intervals.remove(&(range.start, range.end));
    }

    pub fn iter(&self) -> impl Iterator<Item = ChunkInterval> + '_ {
        self.intervals.iter().map(|(start, end)| *start..*end)
    }
}

impl Default for ChunkIntervalSet {
    fn default() -> Self {
        Self::new()
    }
}
