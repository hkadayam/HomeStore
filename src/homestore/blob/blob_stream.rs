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

use std::sync::Arc;
use std::sync::atomic::{AtomicU8, AtomicU64, Ordering};
use parking_lot::{RwLock, Mutex};
use std::collections::{HashMap, HashSet};
use std::io;
use iomgr::IOBuffer;
use crate::common::blk::{BlkId, BlkNum, BlkCount, ChunkNum, BlkAllocHints, BlkAllocStatus};
use crate::meta::MetaBlk;

/// ID for a BlobStream
pub type BlobStreamId = u64;

/// Segment ID - identifies a write segment within a stream
pub type SegmentId = u32;

/// Maximum number of segments per stream
pub const MAX_SEGMENTS: usize = 256;

/// Maximum number of concurrent checkpoints in the system
pub const MAX_CPS: usize = 2;

/// Configuration for BlobStream
#[derive(Debug, Clone)]
pub struct BlobStreamConfig {
    /// Standard chunk size for new chunks (in bytes)
    pub chunk_size: u64,
    
    /// Maximum write unit allocation size (in blocks)
    pub unit_max_size: u32,
    
    /// Minimum write unit allocation size (in blocks)
    pub unit_min_size: u32,
}

impl Default for BlobStreamConfig {
    fn default() -> Self {
        Self {
            chunk_size: 128 * 1024 * 1024,  // 128MB default chunk size
            unit_max_size: 256,             // 256 blocks = 1MB at 4KB/block
            unit_min_size: 64,                // 64 blocks = 256KB at 4KB/block
        }
    }
}

/// Checkpoint context for managing WriteUnits during a checkpoint cycle
/// 
/// Each checkpoint maintains its own set of WriteUnits to ensure isolation
/// between concurrent checkpoints.
struct BlobCPContext {
    /// All write units created during this checkpoint (historical + active)
    /// Index: SegmentId, Value: List of WriteUnits (ordered by creation)
    all_write_units: Vec<Vec<Arc<WriteUnit>>>,
    
    /// Current active WriteUnit per segment for this checkpoint
    /// Index: SegmentId, Value: Current WriteUnit accepting appends
    active_write_unit: Vec<Option<Arc<WriteUnit>>>,
    
    /// Set of chunk IDs that were modified during this checkpoint
    /// Only these chunks need their allocator bitmaps flushed
    modified_chunks: HashSet<u32>,
}

impl BlobCPContext {
    fn new() -> Self {
        Self {
            all_write_units: vec![Vec::new(); MAX_SEGMENTS],
            active_write_unit: vec![None; MAX_SEGMENTS],
            modified_chunks: HashSet::new(),
        }
    }
    
    fn reset(&mut self) {
        for i in 0..MAX_SEGMENTS {
            self.all_write_units[i].clear();
            self.active_write_unit[i] = None;
        }
        self.modified_chunks.clear();
    }
    
    /// Try to append to the active unit for this segment (read lock path)
    /// 
    /// Returns Ok(blk_id) if append succeeded
    /// Returns Err(()) if unit is None or sealed (caller should take write lock)
    fn try_append(&self, segment_id: SegmentId, buffer: &IOBuffer) -> Result<BlkId, ()> {
        if let Some(unit) = &self.active_write_unit[segment_id as usize] {
            if let Some(blk_id) = unit.append(buffer.clone()) {
                return Ok(blk_id);
            }
        }
        Err(())
    }
    
    /// Handle sealed unit and set new active unit (write lock path)
    /// 
    /// If current active unit is sealed, moves it to all_write_units.
    /// Sets the new_unit as the active unit and tracks the modified chunk.
    fn swap_active_unit(&mut self, segment_id: SegmentId, new_unit: Arc<WriteUnit>) {
        let idx = segment_id as usize;
        
        // Track the chunk this new unit writes to
        let chunk_num = new_unit.chunk_num();
        self.modified_chunks.insert(chunk_num as u32);
        
        // Move current active unit to all_write_units if it exists
        // (it's already sealed by WriteUnit::append() returning None)
        if let Some(old_unit) = self.active_write_unit[idx].take() {
            self.all_write_units[idx].push(old_unit);
        }
        
        self.active_write_unit[idx] = Some(new_unit);
    }
    
    /// Get the set of modified chunk IDs
    fn get_modified_chunks(&self) -> &HashSet<u32> {
        &self.modified_chunks
    }
}

impl Default for BlobCPContext {
    fn default() -> Self {
        Self::new()
    }
}

/// A BlobStream represents a logical grouping of writes with automatic expansion
/// 
/// Key characteristics:
/// - VirtualDev manages chunks internally via stream_id
/// - Manages WriteUnits per checkpoint for isolation (array-based, O(1) lookup)
/// - Uses optimistic RwLock: read lock for hot path, write lock only when swapping units
/// - Automatically expands VirtualDev when allocation fails
/// - WriteUnits created on-demand and flushed at CP flush
/// - Only modified chunks have their allocator bitmaps flushed
pub struct BlobStream {
    /// Unique ID for this stream
    id: BlobStreamId,
    
    /// Checkpoint contexts (fixed array of MAX_CPS)
    /// Index by (cp_id % MAX_CPS)
    /// Uses RwLock for optimistic append path
    cp_contexts: [RwLock<BlobCPContext>; MAX_CPS],
    
    /// Reference to VirtualDev for chunk/block allocation
    vdev: Arc<crate::device::VirtualDev>,
    
    /// Meta client for creating/updating chunk metablks
    meta_client: Arc<crate::meta::MetaClient>,
    
    /// Chunk metablks for this stream (one per chunk)
    /// Key: chunk_id, Value: MetaBlk containing allocator bitmap
    chunk_metablks: RwLock<HashMap<u32, MetaBlk>>,
    
    /// Configuration
    config: BlobStreamConfig,
}

impl BlobStream {
    /// Create a new BlobStream
    /// 
    /// Creates a new stream and expands it with an initial chunk.
    /// 
    /// # Arguments
    /// * `stream_id` - Unique ID for this stream
    /// * `vdev` - VirtualDev for chunk/block allocation
    /// * `meta_client` - Meta client for creating/updating chunk metablks
    /// * `config` - Configuration (optional, uses default if None)
    pub async fn new(
        stream_id: BlobStreamId,
        vdev: Arc<crate::device::VirtualDev>,
        meta_client: Arc<crate::meta::MetaClient>,
        config: Option<BlobStreamConfig>,
    ) -> io::Result<Self> {
        let config = config.unwrap_or_default();
        
        println!("BlobStream {}: Creating new stream", stream_id);
        
        let stream = Self {
            id: stream_id,
            cp_contexts: [
                RwLock::new(BlobCPContext::new()),
                RwLock::new(BlobCPContext::new()),
            ],
            vdev,
            meta_client,
            chunk_metablks: RwLock::new(HashMap::new()),
            config,
        };
        
        // Expand to create initial chunk
        let device_mgr = crate::common::managers::device_mgr();
        let chunk = device_mgr.expand_vdev(&stream.vdev, stream_id, stream.config.chunk_size).await?;
        
        // Create metablk for the initial chunk
        stream.create_chunk_metablk(&chunk).await?;
        
        println!("BlobStream {}: Created with initial chunk {}", stream_id, chunk.chunk_id());
        
        Ok(stream)
    }
    
    /// Load an existing BlobStream
    /// 
    /// Loads a stream with its associated chunk metablks.
    /// 
    /// # Arguments
    /// * `stream_id` - Stream ID to load
    /// * `vdev` - VirtualDev reference
    /// * `meta_client` - Meta client for creating/updating chunk metablks
    /// * `chunk_metablks` - Mapping of chunk_id -> MetaBlk containing allocator bitmaps
    /// * `config` - Configuration (optional, uses default if None)
    pub fn load(
        stream_id: BlobStreamId,
        vdev: Arc<crate::device::VirtualDev>,
        meta_client: Arc<crate::meta::MetaClient>,
        chunk_metablks: HashMap<u32, MetaBlk>,
        config: Option<BlobStreamConfig>,
    ) -> Self {
        let config = config.unwrap_or_default();
        
        println!("BlobStream {}: Loading stream with {} chunk metablks", 
                 stream_id, chunk_metablks.len());
        
        Self {
            id: stream_id,
            cp_contexts: [
                RwLock::new(BlobCPContext::new()),
                RwLock::new(BlobCPContext::new()),
            ],
            vdev,
            meta_client,
            chunk_metablks: RwLock::new(chunk_metablks),
            config,
        }
    }
    
    /// Get stream ID
    pub fn id(&self) -> BlobStreamId {
        self.id
    }
    
    
    /// Get reference to VirtualDev
    pub fn vdev(&self) -> &Arc<crate::device::VirtualDev> {
        &self.vdev
    }
    
    /// Get configuration
    pub fn config(&self) -> &BlobStreamConfig {
        &self.config
    }
    
    /// Create a new WriteUnit with automatic expansion via DeviceManager
    /// 
    /// Allocation strategy:
    /// 1. Try alloc_contiguous for unit_max_size (max)
    /// 2. If fails, try alloc_contiguous for unit_min_size
    /// 3. If still fails, call DeviceManager to expand VirtualDev and retry
    /// 
    /// # Arguments
    /// * `segment_id` - Segment ID for this WriteUnit
    /// * `blk_size` - Block size (blocks per write)
    pub async fn create_write_unit(
        &self,
        segment_id: SegmentId,
        blk_size: u32,
    ) -> io::Result<Arc<WriteUnit>>
    {
        let hints = BlkAllocHints::default();
        
        // Try to allocate contiguous blocks - first try max size
        let (status, blk_id_opt) = self.vdev.alloc_contiguous(self.config.unit_max_size, &hints).await;
        
        let base_blk_id = if status == BlkAllocStatus::Success {
            blk_id_opt.ok_or_else(|| io::Error::new(io::ErrorKind::Other, 
                "alloc_contiguous succeeded but returned None"))?
        } else {
            // Max size failed, try min size
            let (status2, blk_id_opt2) = self.vdev.alloc_contiguous(self.config.unit_min_size, &hints).await;
            
            if status2 == BlkAllocStatus::Success {
                blk_id_opt2.ok_or_else(|| io::Error::new(io::ErrorKind::Other, 
                    "alloc_contiguous succeeded but returned None"))?
            } else {
                // Both failed - expand VirtualDev via DeviceManager
                println!("BlobStream {}: Allocation failed (tried {} and {} blocks), expanding with new chunk", 
                         self.id, self.config.unit_max_size, self.config.unit_min_size);
                
                let device_mgr = crate::common::managers::device_mgr();
                let chunk = device_mgr.expand_vdev(&self.vdev, /* stream_id=*/self.id, self.config.chunk_size).await?;
                
                // Create metablk for the new chunk with allocator bitmap
                self.create_chunk_metablk(&chunk).await?;
                
                // Retry allocation after expansion with min size
                let (status3, blk_id_opt3) = self.vdev.alloc_contiguous(self.config.unit_min_size, &hints).await;
                
                if status3 == BlkAllocStatus::Success {
                    blk_id_opt3.ok_or_else(|| 
                        io::Error::new(io::ErrorKind::Other, "alloc_contiguous succeeded but returned None"))?
                } else {
                    return Err(io::Error::new(io::ErrorKind::OutOfMemory, 
                        format!("Failed to allocate blocks even after expansion: {:?}", status3)));
                }
            }
        };
        
        // Create WriteUnit
        let unit = Arc::new(WriteUnit::new(base_blk_id, blk_size));
        
        println!("BlobStream {}: Created WriteUnit for segment {} at blk_id {:?}", 
                 self.id, segment_id, base_blk_id);
        
        Ok(unit)
    }
    
    /// Append a buffer to a segment
    /// 
    /// Optimized write path using optimistic locking:
    /// 1. **Hot path (read lock)**: Try to append to existing active unit
    /// 2. **Cold path (write lock)**: If sealed/missing, create new unit and swap atomically
    /// 
    /// # Arguments
    /// * `segment_id` - Segment ID to append to (must be < MAX_SEGMENTS)
    /// * `buffer` - Data buffer to write
    /// 
    /// # Returns
    /// The BlkId where the buffer will be written
    pub async fn append(&self, segment_id: SegmentId, buffer: IOBuffer) -> io::Result<BlkId> {
        assert!((segment_id as usize) < MAX_SEGMENTS, "segment_id {} exceeds MAX_SEGMENTS", segment_id);
        
        let cp_guard = crate::common::managers::cp_mgr().cp_guard();
        let cp_id = cp_guard.id();
        let cp_idx = (cp_id % MAX_CPS as u64) as usize;
        
        let blk_size = ((buffer.len() + self.vdev.block_size() as usize - 1) 
                       / self.vdev.block_size() as usize) as u32;
        
        loop {
            // **OPTIMISTIC PATH**: Try with read lock first (hot path - 99% of appends)
            {
                let context = self.cp_contexts[cp_idx].read();
                match context.try_append(segment_id, &buffer) {
                    Ok(blk_id) => {
                        return Ok(blk_id);
                    }
                    Err(_) => {} // Need write lock
                }
            } // Release read lock
            
            // **PESSIMISTIC PATH**: Take write lock, create unit, swap, append - all atomically
            {
                let mut context = self.cp_contexts[cp_idx].write();
                
                // Double-check: maybe another thread created/swapped while we waited
                match context.try_append(segment_id, &buffer) {
                    Ok(blk_id) => {
                        return Ok(blk_id);
                    }
                    Err(_) => {} // Still need to create unit
                }
                
                // Create new WriteUnit WHILE HOLDING WRITE LOCK (prevents race conditions)
                let new_unit = self.create_write_unit(segment_id, blk_size).await?;
                
                // Swap and track modified chunk (still holding write lock)
                context.swap_active_unit(segment_id, new_unit.clone());
                
                println!("BlobStream {}: CP {} created new WriteUnit for segment {}", 
                         self.id, cp_id, segment_id);
                
                // Try append with new unit
                match new_unit.append(buffer.clone()) {
                    Some(blk_id) => return Ok(blk_id),
                    None => {
                        // Rare: new unit immediately full, loop to create another
                        println!("BlobStream {}: CP {} new WriteUnit for segment {} immediately full", 
                                 self.id, cp_id, segment_id);
                    }
                }
            } // Release write lock
        }
    }
    
    
    /// Get statistics for this stream
    pub fn stats(&self) -> BlobStreamStats {
        // Get chunks from VirtualDev
        let num_chunks = self.vdev.get_stream_chunks(self.id)
            .map(|chunks| chunks.len())
            .unwrap_or(0);
        
        BlobStreamStats {
            stream_id: self.id,
            num_chunks,
            num_active_units: 0,
            total_buffered_size: 0,
            pending_writes: 0,
        }
    }
    
    /// Flush all WriteUnits for a specific checkpoint
    /// 
    /// This is called during checkpoint flush to:
    /// 1. Persist all buffered writes (iterating directly, no intermediate collection)
    /// 2. Write allocator bitmaps ONLY for modified chunks
    /// 
    /// # Arguments
    /// * `cp_id` - Checkpoint ID to flush
    pub async fn cp_flush(&self, cp_id: u64) -> io::Result<()> {
        let cp_idx = (cp_id % MAX_CPS as u64) as usize;
        
        println!("BlobStream {}: CP {} starting flush", self.id, cp_id);
        
        // Hold read lock for entire flush - cp_flush is single-threaded per checkpoint
        let context = self.cp_contexts[cp_idx].read();
        let chunk_metablks = self.chunk_metablks.read();
        
        // Iterate over all segments and flush all write units
        for segment_units in &context.all_write_units {
            for unit in segment_units {
                let (buffers, used_blk_id, excess_blk_id) = unit.finalize();
                
                if !buffers.is_empty() {
                    self.vdev.writev(buffers, &used_blk_id).await?;
                    self.vdev.commit_blk(&used_blk_id);
                }
                
                if let Some(excess) = excess_blk_id {
                    self.vdev.free_blk(&[excess])?;
                }
            }
        }
        
        // Flush active_write_unit - they may have data too
        for unit_opt in &context.active_write_unit {
            if let Some(unit) = unit_opt {
                let (buffers, used_blk_id, excess_blk_id) = unit.finalize();
                
                if !buffers.is_empty() {
                    self.vdev.writev(buffers, &used_blk_id).await?;
                    self.vdev.commit_blk(&used_blk_id);
                }
                
                if let Some(excess) = excess_blk_id {
                    self.vdev.free_blk(&[excess])?;
                }
            }
        }
        
        // Write allocator bitmaps ONLY for modified chunks
        println!("BlobStream {}: CP {} writing allocator bitmaps for {} modified chunks", 
                 self.id, cp_id, context.get_modified_chunks().len());
        
        for &chunk_id in context.get_modified_chunks() {
            if let Some(metablk) = chunk_metablks.get(&chunk_id) {
                let allocator_data = self.vdev.get_chunk_allocator_data(chunk_id)?;
                metablk.write_data(&allocator_data, &self.meta_client.meta_vdev).await?;
                
                println!("BlobStream {}: CP {} wrote allocator bitmap for chunk {} ({} bytes)", 
                         self.id, cp_id, chunk_id, allocator_data.len());
            } else {
                eprintln!("BlobStream {}: CP {} missing metablk for modified chunk {}", 
                         self.id, cp_id, chunk_id);
            }
        }
        
        println!("BlobStream {}: CP {} flush complete", self.id, cp_id);
        Ok(())
    }
    
    /// Cleanup a checkpoint context after successful flush
    /// 
    /// This resets the checkpoint context, clearing all WriteUnits.
    /// Called during cp_cleanup phase.
    /// 
    /// # Arguments
    /// * `cp_id` - Checkpoint ID to cleanup
    pub fn cp_cleanup(&self, cp_id: u64) {
        let cp_idx = (cp_id % MAX_CPS as u64) as usize;
        
        let mut context = self.cp_contexts[cp_idx].write();
        context.reset();
        
        println!("BlobStream {}: CP {} cleanup completed", self.id, cp_id);
    }
    
    /// Create a MetaBlk for a newly expanded chunk
    /// 
    /// This is called after VirtualDev::expand() creates a new chunk.
    /// The metablk stores the allocator bitmap for this chunk.
    /// 
    /// # Arguments
    /// * `chunk` - The newly created chunk
    pub async fn create_chunk_metablk(
        &self, 
        chunk: &std::sync::Arc<crate::device::Chunk>,
    ) -> io::Result<()> {
        let chunk_id = chunk.chunk_id();
        let metablk_name = format!("blob_chunk_{}", chunk_id);
        
        // Get initial allocator data for this chunk
        let allocator_data = self.vdev.get_chunk_allocator_data(chunk_id)?;
        
        // Create new metablk with estimated size
        let metablk = self.meta_client.create_meta_blk(
            Some(&metablk_name),
            Some(allocator_data.len()),
        ).await?;
        
        // Write the allocator data to the metablk
        self.meta_client.write_meta_blk(metablk.clone(), &allocator_data).await?;
        
        // Store the metablk
        self.chunk_metablks.write().insert(chunk_id, metablk);
        
        println!("BlobStream {}: Created chunk metablk '{}' ({} bytes)", 
                 self.id, metablk_name, allocator_data.len());
        
        Ok(())
    }
}

/// Metadata stored in the MetaBlk for a BlobStream
/// This is serialized and persisted
/// WriteUnit status
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteUnitStatus {
    /// Accepting writes
    Open = 0,
    /// No more writes accepted (sealed)
    Sealed = 1,
}

/// Per-segment write buffer within a chunk
/// 
/// Key characteristics:
/// - One per segment (not per chunk!)
/// - Created on first write to segment with allocated base_blk_id
/// - Destroyed at CP flush
/// - Buffers IOBuffer references (zero-copy)
/// - Flat array of IOBuffers for append-only writes
pub struct WriteUnit {
    /// Base block ID for this unit (allocated at creation)
    /// This is the full allocation, may have excess at the end
    base_blk_id: BlkId,
    
    /// Block size (blocks per write)
    blk_size: BlkNum,
    
    /// Inner state protected by mutex for consistency
    inner: Mutex<WriteUnitInner>,
    
    /// Status: Open or Sealed
    status: AtomicU8,  // 0 = Open, 1 = Sealed
}

/// Inner mutable state of WriteUnit
struct WriteUnitInner {
    /// Flat array of IOBuffers (append-only)
    pending_buffers: Vec<IOBuffer>,
    
    /// Next offset in blocks from base_blk_id
    next_offset_blocks: BlkNum,
    
    /// Total buffered size in bytes
    buffered_size: usize,
}

impl WriteUnit {
    /// Create a new WriteUnit with pre-allocated base block ID
    /// 
    /// # Arguments
    /// * `base_blk_id` - Base block ID (already allocated)
    /// * `blk_size` - Block size (blocks per write)
    pub fn new(base_blk_id: BlkId, blk_size: BlkNum) -> Self {
        Self {
            base_blk_id,
            blk_size,
            inner: Mutex::new(WriteUnitInner {
                pending_buffers: Vec::new(),
                next_offset_blocks: 0,
                buffered_size: 0,
            }),
            status: AtomicU8::new(WriteUnitStatus::Open as u8),
        }
    }
    
    /// Append a buffer to this unit
    /// Returns the BlkId where this buffer will be written
    /// 
    /// If there's no space, automatically seals the unit and returns None.
    /// 
    /// **Critical**: This operation is atomic - the returned BlkId corresponds
    /// exactly to the position in pending_buffers where the buffer was added.
    pub fn append(&self, buffer: IOBuffer) -> Option<BlkId> {
        // Quick check if already sealed (without locking)
        if self.status() == WriteUnitStatus::Sealed {
            return None;
        }
        
        // Lock for atomic operation
        let mut inner = self.inner.lock();
        
        // Double-check status after acquiring lock
        if self.status() == WriteUnitStatus::Sealed {
            return None;
        }
        
        // Check if we have space for this buffer
        let space_available = self.base_blk_id.blk_count() as BlkNum - inner.next_offset_blocks;
        if space_available < self.blk_size {
            // No space - seal and return None
            drop(inner);  // Release lock before sealing
            self.seal();
            return None;
        }
        
        // Calculate BlkId for this buffer
        let blk_id = BlkId::new(
            self.base_blk_id.blk_num() + inner.next_offset_blocks,
            self.blk_size as BlkCount,
            self.base_blk_id.chunk_num()
        );
        
        // Append buffer to vector
        inner.buffered_size += buffer.len();
        inner.pending_buffers.push(buffer);
        
        // Update offset for next append
        inner.next_offset_blocks += self.blk_size;
        
        Some(blk_id)
    }
    
    /// Get base block ID (full allocation)
    pub fn base_blk_id(&self) -> &BlkId {
        &self.base_blk_id
    }
    
    /// Get chunk number from base_blk_id
    pub fn chunk_num(&self) -> ChunkNum {
        self.base_blk_id.chunk_num()
    }
    
    /// Get the number of pending buffers
    pub fn pending_count(&self) -> usize {
        self.inner.lock().pending_buffers.len()
    }
    
    /// Get total buffered size
    pub fn buffered_size(&self) -> usize {
        self.inner.lock().buffered_size
    }
    
    /// Check if this unit is empty
    pub fn is_empty(&self) -> bool {
        self.pending_count() == 0
    }
    
    /// Get status
    pub fn status(&self) -> WriteUnitStatus {
        match self.status.load(Ordering::Acquire) {
            0 => WriteUnitStatus::Open,
            _ => WriteUnitStatus::Sealed,
        }
    }
    
    /// Seal this unit (no more writes accepted)
    pub fn seal(&self) {
        self.status.store(WriteUnitStatus::Sealed as u8, Ordering::Release);
    }
    
    /// Finalize this WriteUnit for flushing
    /// 
    /// This:
    /// 1. Seals the unit (no more appends)
    /// 2. Takes all pending buffers
    /// 3. Adjusts base_blk_id to only include used blocks
    /// 4. Returns excess blocks for freeing
    /// 
    /// # Returns
    /// - `Vec<IOBuffer>` - Buffers to write (in order)
    /// - `BlkId` - Adjusted base_blk_id (only used blocks)
    /// - `Option<BlkId>` - Excess blocks to free (tail blocks not used)
    pub fn finalize(&self) -> (Vec<IOBuffer>, BlkId, Option<BlkId>) {
        // Seal first
        self.seal();
        
        // Take all pending state
        let mut inner = self.inner.lock();
        let buffers = std::mem::take(&mut inner.pending_buffers);
        let used_blocks = inner.next_offset_blocks;
        
        // Reset state
        inner.next_offset_blocks = 0;
        inner.buffered_size = 0;
        drop(inner);
        
        // Calculate adjusted base_blk_id (only used blocks)
        let adjusted_base_blk_id = BlkId::new(
            self.base_blk_id.blk_num(),
            used_blocks as BlkCount,
            self.base_blk_id.chunk_num()
        );
        
        // Calculate excess blocks (if any)
        let total_allocated = self.base_blk_id.blk_count() as BlkNum;
        let excess_blk_id = if used_blocks < total_allocated {
            let excess_blocks = total_allocated - used_blocks;
            Some(BlkId::new(
                self.base_blk_id.blk_num() + used_blocks,
                excess_blocks as BlkCount,
                self.base_blk_id.chunk_num()
            ))
        } else {
            None
        };
        
        (buffers, adjusted_base_blk_id, excess_blk_id)
    }
}

/// Statistics for a BlobStream
#[derive(Debug, Clone)]
pub struct BlobStreamStats {
    pub stream_id: BlobStreamId,
    pub num_chunks: usize,
    pub num_active_units: usize,
    pub total_buffered_size: usize,
    pub pending_writes: usize,
}
