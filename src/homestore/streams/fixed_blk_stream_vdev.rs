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
//!
//! ### Session Management
//! - **Session-based**: Uses explicit session IDs for write isolation
//! - **WriteUnit per segment**: Each segment has its own WriteUnit per session
//! - **Optimistic locking**: Read lock for hot path, write lock for unit creation
//!
//! ### Read Operations
//! - **Runtime reads**: Supports reading committed blocks during normal operation
//! - **Random access**: Can read any committed block by BlkId
//! - **No caching**: Direct read-through to VirtualDev (no read cache)
//!
//! ### Data Lifecycle
//! - **Block invalidation**: Can invalidate (free) individual blocks or ranges
//! - **Session cleanup**: `flush()` resets session context after flushing writes
//! - **Fast destruction**: Efficient one-shot cleanup via `destroy()`
//!
//! ### Architecture
//! - **Large metadata**: Persists all allocated block bitmap in allocator
//! - **ChunkPool integration**: Reuses freed chunks via `VdevChunkTracker`
//!
//! ## Use Cases
//!
//! Ideal for block-oriented storage where:
//! - Fixed-size block appends are common
//! - Random reads of committed blocks are needed
//! - Selective block invalidation is required
//! - Session-based persistence is used
//!
//! Examples: Btree Index nodes written during sessions

use std::io;
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};
use parking_lot::{RwLock, Mutex};
use std::collections::{HashSet, HashMap};

use crate::device::virtual_dev::{VirtualDev, VDevParameters};
use crate::common::{BlkId, BlkIds, BlkAllocHints, BlkAllocStatus};
use iomgr::IOBuffer;


/// Segment ID - identifies a write segment within a stream
pub type SegmentId = u32;

/// Maximum number of segments per stream
pub const MAX_SEGMENTS: usize = 256;

/// Runtime constant: Maximum chunks to keep in pool
const MAX_CHUNKS_IN_POOL: usize = 8;

/// Configuration for FixedBlkStreamVdev
#[derive(Debug, Clone)]
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

/// Session context for managing WriteUnits during a session
/// 
/// Each session maintains its own set of WriteUnits to ensure isolation
/// between concurrent sessions.
struct SessionContext {
    /// All write units created during this session (historical + active)
    /// Index: SegmentId, Value: List of WriteUnits (ordered by creation)
    all_write_units: Vec<Vec<Arc<WriteUnit>>>,
    
    /// Current active WriteUnit per segment for this session
    /// Index: SegmentId, Value: Current WriteUnit accepting appends
    active_write_unit: Vec<Option<Arc<WriteUnit>>>,
    
    /// Set of chunk IDs that were modified during this session
    /// Only these chunks need their allocator bitmaps flushed
    modified_chunks: HashSet<u32>,
}

impl SessionContext {
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
    /// Returns Ok(blk_id) if append succeeded (buffer is consumed)
    /// Returns Err(buffer) if unit is None or sealed, returning the buffer back
    fn try_append(&self, segment_id: SegmentId, buffer: IOBuffer) -> Result<BlkId, IOBuffer> {
        if let Some(unit) = &self.active_write_unit[segment_id as usize] {
            // Try to append - WriteUnit returns the buffer back if sealed/full
            unit.append(buffer)
        } else {
            Err(buffer)
        }
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

/// Fixed Block Stream Virtual Device
pub struct FixedBlkStreamVdev {
    /// Underlying VirtualDev for I/O and block allocation
    /// VirtualDev manages ChunkPool internally
    vdev: Arc<VirtualDev>,
    
    /// Session contexts (dynamic based on num_sessions)
    /// Index by (session_id % num_sessions)
    /// Uses RwLock for optimistic append path
    session_contexts: Vec<RwLock<SessionContext>>,
    
    /// Per-chunk metadata blocks (for persisting allocator state)
    chunk_metablks: RwLock<std::collections::HashMap<u32, crate::meta::MetaBlkWrapper>>,
    
    /// Meta client for creating metablks
    meta_client: Arc<crate::meta::MetaClient>,
    
    /// Configuration
    config: FixedBlkStreamConfig,
}

impl FixedBlkStreamVdev {
   /// Create a new FixedBlkStreamVdev
    ///
    /// This will create an underlying VirtualDev using the provided parameters.
    ///
    /// # Arguments
    /// * `vdev_params` - Parameters for creating the underlying VirtualDev
    /// * `meta_client` - MetaClient for creating metablks
    /// * `num_sessions` - Number of concurrent sessions to support
    /// * `config` - Configuration for write units
    pub async fn create(
        vdev_params: VDevParameters,
        meta_client: Arc<crate::meta::MetaClient>,
        num_sessions: usize,
        config: Option<FixedBlkStreamConfig>,
    ) -> io::Result<Self> {
        let config = config.unwrap_or_default();
        
        // Create VirtualDev using DeviceManager
        let device_mgr = crate::common::managers::device_mgr();
        let vdev = device_mgr.create_vdev(vdev_params.clone()).await?;
        let chunk_metablks = RwLock::new(std::collections::HashMap::new());
        
        // Initialize all initial chunks
        let chunks = vdev.get_chunks();
        for chunk in chunks.iter() {
            Self::on_chunk_created(&vdev, chunk, &meta_client, &chunk_metablks).await?;
        }
        
        // Create session contexts (dynamic based on num_sessions)
        let session_contexts = (0..num_sessions)
            .map(|_| RwLock::new(SessionContext::new()))
            .collect();
        
        println!("FixedBlkStreamVdev {}: Created with {} sessions and {} chunks", 
                 vdev.vdev_id(), num_sessions, chunks.len());
        
        Ok(Self {
            vdev,
            session_contexts,
            chunk_metablks,
            meta_client,
            config,
        })
    }
    
    /// Load an existing FixedBlkStreamVdev
    ///
    /// # Arguments
    /// * `vdev` - Existing VirtualDev (already loaded with chunks by DeviceManager)
    /// * `meta_client` - MetaClient for loading metablks
    /// * `num_sessions` - Number of concurrent sessions to support
    /// * `config` - Configuration for write units
    pub async fn load(
        vdev: Arc<VirtualDev>,
        meta_client: Arc<crate::meta::MetaClient>,
        num_sessions: usize,
        config: Option<FixedBlkStreamConfig>,
    ) -> io::Result<Self> {
        let config = config.unwrap_or_default();
        
        // Load metablks and allocator buffers for each chunk
        let mut chunk_metablks = std::collections::HashMap::new();
        let mut chunk_buffers = std::collections::HashMap::new();
        
        // Walk through all chunks and load their metablks and allocator buffers
        let chunks = vdev.get_chunks();
        for chunk in chunks.iter() {
            let chunk_id = chunk.chunk_id();
            
            // Load existing metablk for this chunk
            let metablk_name = format!("fixed_blk_stream_chunk_{}", chunk_id);
            let meta_blk = meta_client.get_meta_blk(&metablk_name).await
                .ok_or_else(|| io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("Metablk {} not found", metablk_name)
                ))?;
            
            let metablk = crate::meta::MetaBlkWrapper::load(meta_client.clone(), meta_blk);
            
            // Read allocator buffer from metablk (zero-copy)
            let buffer = metablk.read().await?;
            
            chunk_buffers.insert(chunk_id, buffer);
            chunk_metablks.insert(chunk_id, metablk);
        }
        
        // Load block allocators from the buffers
        vdev.load_blk_allocator(Some(chunk_buffers))?;
        
        // Create session contexts (dynamic based on num_sessions)
        let session_contexts = (0..num_sessions)
            .map(|_| RwLock::new(SessionContext::new()))
            .collect();
        
        println!("FixedBlkStreamVdev {}: Loaded with {} sessions and {} chunks", 
                 vdev.vdev_id(), num_sessions, chunks.len());
        
        Ok(Self {
            vdev,
            session_contexts,
            chunk_metablks: RwLock::new(chunk_metablks),
            meta_client,
            config,
        })
    }

    /// Destroy this FixedBlkStreamVdev
    ///
    /// This will remove all chunk metablks and destroy the underlying VirtualDev.
    pub async fn destroy(self) -> io::Result<()> {
        // Remove all per-chunk metadata blocks
        {
            let chunk_metablks = self.chunk_metablks.read();
            for (chunk_id, metablk) in chunk_metablks.iter() {
                self.meta_client
                    .remove_meta_blk(metablk.meta_blk())
                    .await?;
                println!("FixedBlkStreamVdev: Removed metablk for chunk {}", chunk_id);
            }
        }
        
        // Destroy underlying VirtualDev (self-sufficient - handles everything)
        self.vdev.destroy().await?;
        
        Ok(())
    }
    
    /// Append a buffer to a segment
    /// 
    /// Optimized write path using optimistic locking:
    /// 1. **Hot path (read lock)**: Try to append to existing active unit
    /// 2. **Cold path (write lock)**: If sealed/missing, create new unit and swap atomically
    /// 
    /// # Arguments
    /// * `session_id` - Session ID (must be valid for this vdev's num_sessions)
    /// * `segment_id` - Segment ID to append to (must be < MAX_SEGMENTS)
    /// * `buffer` - Data buffer to write, it should be sized in number of blocks
    /// 
    /// # Returns
    /// The BlkId where the buffer will be written
    pub async fn append(&self, session_id: u64, segment_id: SegmentId, buffer: IOBuffer) -> io::Result<BlkId> {
        assert!((segment_id as usize) < MAX_SEGMENTS, "segment_id {} exceeds MAX_SEGMENTS", segment_id);
        
        let session_idx = (session_id % self.session_contexts.len() as u64) as usize;
        
        let blk_size = ((buffer.len() + self.vdev.block_size() as usize - 1) 
                       / self.vdev.block_size() as usize) as u32;
        
        let mut buf = buffer;
        loop {
            // **OPTIMISTIC PATH**: Try with read lock first (hot path - 99% of appends)
            {
                let context = self.session_contexts[session_idx].read();
                match context.try_append(segment_id, buf) {
                    Ok(blk_id) => {
                        return Ok(blk_id);
                    }
                    Err(returned_buf) => {
                        buf = returned_buf; // Get buffer back for retry
                    }
                }
            } // Release read lock
            
            // **PESSIMISTIC PATH**: Take write lock, create unit, swap, append - all atomically
            {
                let mut context = self.session_contexts[session_idx].write();
                
                // Double-check: maybe another thread created/swapped while we waited
                match context.try_append(segment_id, buf) {
                    Ok(blk_id) => {
                        return Ok(blk_id);
                    }
                    Err(returned_buf) => {
                        buf = returned_buf; // Get buffer back
                    }
                }
                
                // Create new WriteUnit WHILE HOLDING WRITE LOCK (prevents race conditions) and 
                // swap, track modified chunk
                let new_unit = self.create_write_unit(segment_id, blk_size).await?;
                context.swap_active_unit(segment_id, new_unit.clone());
                
                // Try append with new unit (move buffer, no clone!)
                match new_unit.append(buf) {
                    Ok(blk_id) => return Ok(blk_id),
                    Err(returned_buf) => {
                        // Rare: new unit immediately full, loop to create another
                        buf = returned_buf;
                        println!("FixedBlkStreamVdev {}: Session {} new WriteUnit for segment {} immediately full", 
                                 self.vdev_id(), session_id, segment_id);
                    }
                }
            } // Release write lock
        }
    }
    
    /// Read from a block (written data only)
    pub async fn read(&self, blk_id: &BlkId) -> io::Result<IOBuffer> {
        let size = (blk_id.blk_count() as usize) * (self.vdev.block_size() as usize);
        let buf = IOBuffer::new(size);
        let (res, buf) = self.vdev.read(buf, blk_id).await;
        res?;
        Ok(buf)
    }
    
    /// Invalidate a single block (mark as free)
    /// 
    /// # Arguments
    /// * `blk_id` - Block ID to invalidate
    pub async fn invalidate_blk(&self, blk_id: &BlkId) {
        self.vdev.free(blk_id).await;
    }
    
    /// Invalidate multiple blocks efficiently (mark as free)
    /// 
    /// Takes blocks organized by chunk_id for efficient batch processing.
    /// This avoids repeated chunk lookups and improves performance for large
    /// invalidation operations.
    /// 
    /// # Arguments
    /// * `blks_by_chunk` - HashMap of chunk_id -> BlkIds to invalidate
    pub async fn invalidate_blks(&self, blks_by_chunk: HashMap<u32, BlkIds>) {
        self.vdev.free_blks(blks_by_chunk).await;
    }
    
    /// Flush all WriteUnits for a specific session
    /// 
    /// This is called to:
    /// 1. Persist all buffered writes
    /// 2. Commit allocated blocks
    /// 3. Free excess blocks from WriteUnits
    /// 4. Reset the session context (cleanup happens at end)
    /// 
    /// # Arguments
    /// * `session_id` - Session ID to flush
    pub async fn flush(&self, session_id: u64) -> io::Result<()> {
        let session_idx = (session_id % self.session_contexts.len() as u64) as usize;
        
        println!("FixedBlkStreamVdev {}: Session {} starting flush", self.vdev_id(), session_id);
        
        // Hold WRITE lock for entire flush - we'll reset at the end
        let mut context = self.session_contexts[session_idx].write();
        
        // Iterate over all segments and flush all write units
        for segment_units in &context.all_write_units {
            for unit in segment_units {
                let (buffers, used_blk_id, excess_blk_id) = unit.finalize();
                
                if !buffers.is_empty() {
                    self.vdev.writev(buffers, &used_blk_id).await?;
                    self.vdev.commit_blk(&used_blk_id);
                }
                
                if let Some(excess) = excess_blk_id {
                    self.vdev.free(&excess).await;
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
                    self.vdev.free(&excess).await;
                }
            }
        }
        
        let num_modified_chunks = context.get_modified_chunks().len();
        
        // CLEANUP: Reset context at end of flush
        context.reset();
        
        println!("FixedBlkStreamVdev {}: Session {} flush complete ({} modified chunks)", 
                 self.vdev_id(), session_id, num_modified_chunks);
        Ok(())
    }
       
    /// Create a new WriteUnit with automatic expansion
    /// 
    /// Allocation strategy:
    /// 1. Try alloc_contiguous for unit_max_size (max)
    /// 2. If fails, try alloc_contiguous for unit_min_size
    /// 3. If still fails, expand VirtualDev and retry
    async fn create_write_unit(
        &self,
        segment_id: SegmentId,
        blk_size: u32,
    ) -> io::Result<Arc<WriteUnit>> {
        let mut hints = BlkAllocHints::default();
        hints.desired_temp = segment_id;

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
                // Allocation failed - expand and retry
                println!("FixedBlkStreamVdev: Allocation failed, expanding");
                
                // Expand VDev and initialize the new chunk and then retry allocation with max size again
                let new_chunk = self.vdev.expand(self.vdev.incremental_chunk_size()).await?;
                Self::on_chunk_created(&self.vdev, &new_chunk, &self.meta_client, &self.chunk_metablks).await?;
                let (status3, blk_id_opt3) = self.vdev.alloc_contiguous(self.config.unit_max_size, &hints).await;
                
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
    
    /// Handle a newly created chunk (initialize allocator, create metablk, persist)
    /// 
    /// This is called for:
    /// - Initial chunks during create()
    /// - New chunks during expand()
    async fn on_chunk_created(
        vdev: &Arc<VirtualDev>,
        chunk: &Arc<crate::device::Chunk>,
        meta_client: &Arc<crate::meta::MetaClient>,
        chunk_metablks: &RwLock<std::collections::HashMap<u32, crate::meta::MetaBlkWrapper>>,
    ) -> io::Result<()> {
        let chunk_id = chunk.chunk_id();
        
        // Initialize allocator for this chunk
        vdev.init_blk_allocator(Some(chunk.clone()))?;
        
        // Create metablk for this chunk
        let metablk = crate::meta::MetaBlkWrapper::create(
            meta_client.clone(),
            &format!("fixed_blk_stream_chunk_{}", chunk_id),
            None,
        ).await?;
        
        // Get allocator buffer and persist (zero-copy)
        if let Some(guard) = vdev.get_allocator_buffer(chunk_id).await? {
            let buffer = guard.buffer();
            metablk.write(buffer.as_slice()).await?;
        }
        
        // Store metablk
        chunk_metablks.write().insert(chunk_id, metablk);
        
        println!("FixedBlkStreamVdev: Initialized chunk {} with metablk", chunk_id);
        Ok(())
    }
    
    // ===== Accessors =====
    
    pub fn vdev(&self) -> &Arc<VirtualDev> {
        &self.vdev
    }
    
    pub fn vdev_id(&self) -> u32 {
        self.vdev.vdev_id()
    }
}

/// WriteUnit status
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteUnitStatus {
    /// Accepting writes
    Open = 0,
    /// No more writes accepted (sealed)
    Sealed = 1,
}

/// Per-segment write buffer within a session
/// 
/// Key characteristics:
/// - One per segment (not per chunk!)
/// - Created on first write to segment with allocated base_blk_id
/// - Destroyed at session flush
/// - Buffers IOBuffer references (zero-copy)
/// - Flat array of IOBuffers for append-only writes
pub struct WriteUnit {
    /// Base block ID for this unit (allocated at creation)
    /// This is the full allocation, may have excess at the end
    base_blk_id: BlkId,
    
    /// Block size (blocks per write)
    blk_size: u32,
    
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
    next_offset_blocks: u32,
    
    /// Total buffered size in bytes
    buffered_size: usize,
}

impl WriteUnit {
    /// Create a new WriteUnit with pre-allocated base block ID
    pub fn new(base_blk_id: BlkId, blk_size: u32) -> Self {
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
    /// 
    /// Returns Ok(blk_id) if buffer was appended successfully
    /// Returns Err(buffer) if unit is sealed/full, returning the buffer back
    pub fn append(&self, buffer: IOBuffer) -> Result<BlkId, IOBuffer> {
        // Quick check if already sealed (without locking)
        if self.status() == WriteUnitStatus::Sealed {
            return Err(buffer);
        }
        
        // Lock for atomic operation
        let mut inner = self.inner.lock();
        
        // Double-check status after acquiring lock
        if self.status() == WriteUnitStatus::Sealed {
            return Err(buffer);
        }
        
        // Check if we have space for this buffer
        let space_available = self.base_blk_id.blk_count() as u32 - inner.next_offset_blocks;
        if space_available < self.blk_size {
            // No space - seal and return buffer back
            drop(inner);  // Release lock before sealing
            self.seal();
            return Err(buffer);
        }
        
        // Calculate BlkId for this buffer
        let blk_id = BlkId::new(
            self.base_blk_id.blk_num() + inner.next_offset_blocks,
            self.blk_size as u16,
            self.base_blk_id.chunk_num()
        );
        
        // Append buffer to vector
        inner.buffered_size += buffer.len();
        inner.pending_buffers.push(buffer);
        
        // Update offset for next append
        inner.next_offset_blocks += self.blk_size;
        
        Ok(blk_id)
    }
    
    /// Get chunk ID from base_blk_id
    pub fn chunk_id(&self) -> u32 {
        self.base_blk_id.chunk_num() as u32
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
            used_blocks as u16,
            self.base_blk_id.chunk_num()
        );
        
        // Calculate excess blocks (if any)
        let total_allocated = self.base_blk_id.blk_count() as u32;
        let excess_blk_id = if used_blocks < total_allocated {
            let excess_blocks = total_allocated - used_blocks;
            Some(BlkId::new(
                self.base_blk_id.blk_num() + used_blocks,
                excess_blocks as u16,
                self.base_blk_id.chunk_num()
            ))
        } else {
            None
        };
        
        (buffers, adjusted_base_blk_id, excess_blk_id)
    }
}
