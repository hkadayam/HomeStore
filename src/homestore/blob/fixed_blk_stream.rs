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
use std::sync::atomic::{AtomicU8, Ordering};
use parking_lot::{RwLock, Mutex};
use std::collections::{HashMap, HashSet};
use std::io;
use iomgr::IOBuffer;
use serde::{Deserialize, Serialize};

use crate::device::virtual_dev::VirtualDev;
use crate::meta::meta_blk::MetaBlk;
use crate::meta::meta_client::MetaClient;
use crate::blob::types::{BlkId, BlkNum, BlkCount, BlkAllocHints, BlkAllocStatus};

use super::stream_base::StreamBase;
use super::new_blob_dev::StreamMetadata;

/// Segment ID - identifies a write segment within a stream
pub type SegmentId = u32;

/// Maximum number of segments per stream
pub const MAX_SEGMENTS: usize = 256;

/// Maximum number of concurrent checkpoints in the system
pub const MAX_CPS: usize = 2;

/// Configuration for FixedBlkStream
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FixedBlkStreamConfig {
    /// Maximum write unit allocation size (in blocks)
    pub unit_max_size: u32,
    
    /// Minimum write unit allocation size (in blocks)
    pub unit_min_size: u32,
}

impl Default for FixedBlkStreamConfig {
    fn default() -> Self {
        Self {
            unit_max_size: 256,  // 256 blocks = 1MB at 4KB/block
            unit_min_size: 64,   // 64 blocks = 256KB at 4KB/block
        }
    }
}

/// Checkpoint context for managing WriteUnits during a checkpoint cycle
/// 
/// Each checkpoint maintains its own set of WriteUnits to ensure isolation
/// between concurrent checkpoints.
struct FixedBlkCPContext {
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

impl FixedBlkCPContext {
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
        let chunk_id = new_unit.chunk_id();
        self.modified_chunks.insert(chunk_id);
        
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

/// FixedBlkStream - Block-level stream with append-only WriteUnits
/// 
/// Key characteristics:
/// - Uses StreamBase for chunk management
/// - Manages WriteUnits per checkpoint for isolation
/// - Operates on block boundaries (all APIs use BlkId)
/// - No caching, no write*, simple append-only buffering
pub struct FixedBlkStream {
    /// Base stream infrastructure
    stream_base: StreamBase,
    
    /// Checkpoint contexts (fixed array of MAX_CPS)
    /// Index by (cp_id % MAX_CPS)
    /// Uses RwLock for optimistic append path
    cp_contexts: [RwLock<FixedBlkCPContext>; MAX_CPS],
    
    /// Meta client for metadata operations
    meta_client: Arc<MetaClient>,
    
    /// Configuration
    config: FixedBlkStreamConfig,
}

impl FixedBlkStream {
    /// Create a new FixedBlkStream
    pub async fn create(
        vdev: Arc<VirtualDev>,
        metadata: StreamMetadata,
        mut stream_mblk: MetaBlk,
        meta_client: Arc<MetaClient>,
        config: Option<FixedBlkStreamConfig>,
    ) -> io::Result<Self> {
        let config = config.unwrap_or_default();
        
        // Serialize metadata with config
        let stream_metadata_with_config = FixedBlkStreamMetadata {
            base: metadata.clone(),
            config: config.clone(),
        };
        
        let metadata_bytes = bincode::serialize(&stream_metadata_with_config)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        stream_mblk.write_data(&metadata_bytes, &meta_client.meta_vdev).await?;
        
        // Create StreamBase (takes ownership of stream_mblk)
        let stream_base = StreamBase::new(vdev, metadata, stream_mblk);
        
        println!("FixedBlkStream {}: Created new stream", stream_base.stream_id());
        
        Ok(Self {
            stream_base,
            cp_contexts: [
                RwLock::new(FixedBlkCPContext::new()),
                RwLock::new(FixedBlkCPContext::new()),
            ],
            meta_client,
            config,
        })
    }
    
    /// Load an existing FixedBlkStream
    pub async fn load(
        vdev: Arc<VirtualDev>,
        metadata: StreamMetadata,
        stream_mblk: MetaBlk,
        chunk_metablks: Vec<MetaBlk>,
        meta_client: Arc<MetaClient>,
    ) -> io::Result<Self> {
        // Read full metadata (includes config)
        let metadata_bytes = stream_mblk.read_data(&meta_client.meta_vdev).await?;
        let stream_metadata_with_config: FixedBlkStreamMetadata = 
            bincode::deserialize(&metadata_bytes)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        // Load StreamBase with existing chunks and their metadata
        let stream_base = StreamBase::load(vdev.clone(), metadata, stream_mblk, chunk_metablks.clone())?;
        
        // Initialize block allocators for each chunk from their metadata
        let mut chunk_buffers = HashMap::new();
        let stream_chunks = stream_base.chunks();
        
        for stream_chunk in stream_chunks.iter() {
            if let Some(ref chunk_meta_blk) = stream_chunk.chunk_meta_blk {
                // Read the raw allocator data as IOBuffer (no deserialization, no copy)
                let allocator_data = chunk_meta_blk.read_data(&meta_client.meta_vdev).await?;
                chunk_buffers.insert(stream_chunk.chunk_id(), allocator_data);
            }
        }
        
        // Let VirtualDev initialize block allocators for all chunks
        vdev.load_blk_allocator(Some(chunk_buffers))?;
        
        println!("FixedBlkStream {}: Loaded stream with {} chunks", 
                 stream_base.stream_id(), stream_chunks.len());
        
        Ok(Self {
            stream_base,
            cp_contexts: [
                RwLock::new(FixedBlkCPContext::new()),
                RwLock::new(FixedBlkCPContext::new()),
            ],
            meta_client,
            config: stream_metadata_with_config.config,
        })
    }
    
    /// Get stream ID
    pub fn stream_id(&self) -> u64 {
        self.stream_base.stream_id()
    }
    
    /// Get current chunk list
    pub fn get_chunk_list(&self) -> Vec<u32> {
        self.stream_base.get_chunk_ids()
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
        
        let cp_guard = crate::homestore::common::managers::cp_mgr().cp_guard();
        let cp_id = cp_guard.id();
        let cp_idx = (cp_id % MAX_CPS as u64) as usize;
        
        let blk_size = ((buffer.len() + self.stream_base.vdev().block_size() as usize - 1) 
                       / self.stream_base.vdev().block_size() as usize) as u32;
        
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
                
                // Try append with new unit
                match new_unit.append(buffer.clone()) {
                    Some(blk_id) => return Ok(blk_id),
                    None => {
                        // Rare: new unit immediately full, loop to create another
                        println!("FixedBlkStream {}: CP {} new WriteUnit for segment {} immediately full", 
                                 self.stream_id(), cp_id, segment_id);
                    }
                }
            } // Release write lock
        }
    }
    
    /// Read from a block (committed data only)
    pub async fn read(&self, blk_id: &BlkId, buf: &mut [u8]) -> io::Result<usize> {
        self.stream_base.vdev().read(blk_id, buf).await
    }
    
    /// Invalidate a block (mark as free)
    pub fn invalidate(&self, blk_id: &BlkId) -> io::Result<()> {
        self.stream_base.vdev().free_blk(&[blk_id.clone()])
    }
    
    /// Flush all WriteUnits for a specific checkpoint
    /// 
    /// This is called during checkpoint flush to:
    /// 1. Persist all buffered writes
    /// 2. Write allocator bitmaps ONLY for modified chunks
    /// 3. Persist stream metadata with updated chunk_list
    /// 
    /// # Arguments
    /// * `cp_id` - Checkpoint ID to flush
    pub async fn cp_flush(&self, cp_id: u64) -> io::Result<()> {
        let cp_idx = (cp_id % MAX_CPS as u64) as usize;
        
        println!("FixedBlkStream {}: CP {} starting flush", self.stream_id(), cp_id);
        
        // Hold read lock for entire flush - cp_flush is single-threaded per checkpoint
        let context = self.cp_contexts[cp_idx].read();
        
        // Iterate over all segments and flush all write units
        for segment_units in &context.all_write_units {
            for unit in segment_units {
                let (buffers, used_blk_id, excess_blk_id) = unit.finalize();
                
                if !buffers.is_empty() {
                    self.stream_base.vdev().writev(buffers, &used_blk_id).await?;
                    self.stream_base.vdev().commit_blk(&used_blk_id);
                }
                
                if let Some(excess) = excess_blk_id {
                    self.stream_base.vdev().free_blk(&[excess])?;
                }
            }
        }
        
        // Flush active_write_unit - they may have data too
        for unit_opt in &context.active_write_unit {
            if let Some(unit) = unit_opt {
                let (buffers, used_blk_id, excess_blk_id) = unit.finalize();
                
                if !buffers.is_empty() {
                    self.stream_base.vdev().writev(buffers, &used_blk_id).await?;
                    self.stream_base.vdev().commit_blk(&used_blk_id);
                }
                
                if let Some(excess) = excess_blk_id {
                    self.stream_base.vdev().free_blk(&[excess])?;
                }
            }
        }
        
        // Write allocator bitmaps ONLY for modified chunks
        println!("FixedBlkStream {}: CP {} writing allocator bitmaps for {} modified chunks", 
                 self.stream_id(), cp_id, context.get_modified_chunks().len());
        
        let stream_chunks = self.stream_base.chunks();
        for &chunk_id in context.get_modified_chunks() {
            // Find the chunk's metadata block
            let chunk_meta_blk = stream_chunks.iter()
                .find(|sc| sc.chunk_id() == chunk_id)
                .and_then(|sc| sc.chunk_meta_blk.as_ref());
            
            if let Some(metablk) = chunk_meta_blk {
                let allocator_data = self.stream_base.vdev().get_chunk_allocator_data(chunk_id)?;
                metablk.write_data(&allocator_data, &self.meta_client.meta_vdev).await?;
                
                println!("FixedBlkStream {}: CP {} wrote allocator bitmap for chunk {} ({} bytes)", 
                         self.stream_id(), cp_id, chunk_id, allocator_data.len());
            } else {
                eprintln!("FixedBlkStream {}: CP {} missing metablk for modified chunk {}", 
                         self.stream_id(), cp_id, chunk_id);
            }
        }
        
        // Persist stream metadata with updated chunk_list
        self.persist_metadata().await?;
        
        println!("FixedBlkStream {}: CP {} flush complete", self.stream_id(), cp_id);
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
        
        println!("FixedBlkStream {}: CP {} cleanup completed", self.stream_id(), cp_id);
    }
    
    /// Destroy stream - completely removes the stream and its metadata
    pub async fn destroy(self) -> io::Result<()> {
        self.stream_base.destroy(&self.meta_client).await
    }
    
    /// Create a new WriteUnit with automatic expansion via DeviceManager
    /// 
    /// Allocation strategy:
    /// 1. Try alloc_contiguous for unit_max_size (max)
    /// 2. If fails, try alloc_contiguous for unit_min_size
    /// 3. If still fails, expand VirtualDev and retry
    async fn create_write_unit(
        &self,
        segment_id: SegmentId,
        blk_size: u32,
    ) -> io::Result<Arc<WriteUnit>>
    {
        let hints = BlkAllocHints::default();
        
        // Try to allocate contiguous blocks - first try max size
        let (status, blk_id_opt) = self.stream_base.vdev().alloc_contiguous(self.config.unit_max_size, &hints).await;
        
        let base_blk_id = if status == BlkAllocStatus::Success {
            blk_id_opt.ok_or_else(|| io::Error::new(io::ErrorKind::Other, 
                "alloc_contiguous succeeded but returned None"))?
        } else {
            // Max size failed, try min size
            let (status2, blk_id_opt2) = self.stream_base.vdev().alloc_contiguous(self.config.unit_min_size, &hints).await;
            
            if status2 == BlkAllocStatus::Success {
                blk_id_opt2.ok_or_else(|| io::Error::new(io::ErrorKind::Other, 
                    "alloc_contiguous succeeded but returned None"))?
            } else {
                // Both failed - expand VirtualDev
                println!("FixedBlkStream {}: Allocation failed, expanding with new chunk", self.stream_id());
                
                // Ensure chunk exists at next index
                let current_chunks = self.stream_base.chunks();
                let next_chunk_idx = current_chunks.len();
                
                // Expand will create chunk and initialize its block allocator
                self.stream_base.ensure_chunk_exists(next_chunk_idx, Some(|chunk_id: u32| -> Option<MetaBlk> {
                    // Create metablk for new chunk (will be written during cp_flush)
                    None // TODO: Create metablk asynchronously or defer to flush
                })).await?;
                
                // Retry allocation after expansion with min size
                let (status3, blk_id_opt3) = self.stream_base.vdev().alloc_contiguous(self.config.unit_min_size, &hints).await;
                
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
        
        Ok(unit)
    }
    
    /// Persist stream metadata
    async fn persist_metadata(&self) -> io::Result<()> {
        let metadata = StreamMetadata {
            stream_id: self.stream_base.stream_id(),
            stream_type: super::new_blob_dev::StreamType::FixedBlkStream,
            chunk_list: self.stream_base.get_chunk_ids(),
            chunk_size: self.stream_base.chunk_size(),
            max_chunks_in_pool: self.stream_base.metadata.max_chunks_in_pool,
        };
        
        let stream_metadata_with_config = FixedBlkStreamMetadata {
            base: metadata,
            config: self.config.clone(),
        };
        
        let serialized = bincode::serialize(&stream_metadata_with_config)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        self.stream_base.stream_mblk().write_data(&serialized, &self.meta_client.meta_vdev).await?;
        Ok(())
    }
}

/// Metadata for FixedBlkStream (includes base + config)
#[derive(Debug, Clone, Serialize, Deserialize)]
struct FixedBlkStreamMetadata {
    base: StreamMetadata,
    config: FixedBlkStreamConfig,
}

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
    
    /// Get chunk ID from base_blk_id
    pub fn chunk_id(&self) -> u32 {
        self.base_blk_id.chunk_num()
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
