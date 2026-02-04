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
//! - **Auto-expansion**: Automatically allocates new chunks as the stream grows
//!
//! ### Session Management
//! - **Caller-managed sessions**: Does not maintain checkpoint sessions internally
//! - **Session isolation**: Caller is responsible for ensuring appends happen in 
//!   appropriate sessions and calling `flush()` with the correct `session_id`
//! - **No cross-session validation**: The vdev trusts the caller to manage session 
//!   boundaries and ordering correctly
//!
//! ### Read Operations  
//! - **No runtime reads**: Read operations are not supported during normal operation
//! - **Recovery-only access**: Data can only be read during recovery via `recovery_read_at()`
//! - **No caching**: Does not employ any read or write caching mechanisms
//!
//! ### Data Lifecycle
//! - **No invalidation**: Individual writes cannot be invalidated or rolled back
//! - **Stream truncation**: Supports `truncate()` to reset the entire stream to offset 0,
//!   allowing reuse of existing chunks without reallocation
//! - **Fast destruction**: `destroy()` provides efficient one-shot cleanup of the entire stream
//!
//! ### Architecture
//! - **Minimal metadata**: Persists only `tail_offset` in a single metablk
//! - **Chunk ordering**: Relies on VirtualDev's `creation_order`-based chunk management
//! - **ChunkPool integration**: Reuses freed chunks via `VdevChunkTracker` for efficiency
//! - **Block-aligned I/O**: Internally aligns writes to block boundaries while presenting
//!   a byte-stream interface to callers
//!
//! ## Use Cases
//!
//! Ideal for high-performance logging scenarios where:
//! - Small, arbitrary-sized appends are frequent
//! - Data is primarily write-once, read-during-recovery
//! - Lock-free concurrent appends are critical for throughput
//! - Minimal metadata overhead is desired
//! - Upper layer manages checkpoint/session coordination
//!
//! Examples: Write-ahead logs (WAL), journals, audit logs, transaction logs

use std::io;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use crossbeam::queue::SegQueue;
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};

use crate::device::virtual_dev::{VirtualDev, VDevParameters};
use crate::meta::{MetaBlkWrapper, MetaClient};
use crate::common::BlkId;
use iomgr::IOBuffer;

use crate::device::FlushSessionId;

/// Runtime constant: Maximum chunks to keep in pool before returning to VDev
const MAX_CHUNKS_IN_POOL: usize = 8;

/// Per-session buffer context
struct SessionBuffer {
    /// Lock-free queue of data buffers
    queue: SegQueue<Vec<u8>>,
    
    /// Residual buffer from last flush (unaligned portion)
    residual_buf: Mutex<Vec<u8>>,
    
    /// Buffered size (tracked during append)
    buffered_size: AtomicUsize,
    
    /// Block size
    block_size: usize,
}

impl SessionBuffer {
    fn new(block_size: u32) -> Self {
        Self {
            queue: SegQueue::new(),
            residual_buf: Mutex::new(Vec::new()),
            buffered_size: AtomicUsize::new(0),
            block_size: block_size as usize,
        }
    }
    
    /// Reset buffer (called at end of flush automatically)
    fn reset(&self) {
        while self.queue.pop().is_some() {}
        self.buffered_size.store(0, Ordering::Release);
    }
}

unsafe impl Sync for SessionBuffer {}

/// Simple Log Stream Virtual Device - specialized VDev for simple append-only log stream operations
pub struct SimpleLogStreamVdev {
    /// Underlying VirtualDev for I/O operations
    /// VirtualDev already maintains chunks in creation order and manages ChunkPool
    vdev: Arc<VirtualDev>,
    
    /// Logical stream offset (tail position)
    tail_offset: AtomicU64,
    
    /// Target size - size to maintain/shrink back to on truncate
    target_size: u64,
    
    /// Per-session buffers (for concurrent appends)
    session_buffers: Vec<SessionBuffer>,
    
    /// Metadata block wrapper (stores tail_offset and target_size)
    append_meta_blk: MetaBlkWrapper,
}

impl SimpleLogStreamVdev {
    /// Create a new SimpleLogStreamVdev
    ///
    /// This will create an underlying VirtualDev using the provided parameters.
    ///
    /// # Arguments
    /// * `vdev_params` - Parameters for creating the underlying VirtualDev (should use AppendBlkAllocator)
    /// * `meta_client` - MetaClient for metadata persistence
    /// * `num_sessions` - Number of concurrent flush sessions
    pub async fn create(
        vdev_params: VDevParameters,
        meta_client: Arc<MetaClient>,
        num_sessions: usize,
    ) -> io::Result<Self> {
        // Create VirtualDev using DeviceManager
        let device_mgr = crate::common::managers::device_mgr();
        let vdev = device_mgr.create_vdev(vdev_params.clone()).await?;
        
        let block_size = vdev.block_size();
        let vdev_id = vdev.vdev_id();
        let target_size = vdev.size();  // Capture target size after creation
        
        // Allocate metadata block (stores tail_offset and target_size)
        let append_meta_blk = MetaBlkWrapper::create(
            meta_client,
            &format!("simple_log_stream_{}", vdev_id),
            None,
        ).await?;
        
        // Initial metadata: tail_offset = 0, target_size = vdev.size()
        let metadata = SimpleLogStreamMetadata { 
            tail_offset: 0,
            target_size,
        };
        let serialized = bincode::serialize(&metadata)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        append_meta_blk.write(&serialized).await?;
        
        // Create session buffers
        let session_buffers = (0..num_sessions)
            .map(|_| SessionBuffer::new(block_size))
            .collect();
        
        Ok(Self {
            vdev,
            tail_offset: AtomicU64::new(0),
            target_size,
            session_buffers,
            append_meta_blk,
        })
    }
    
    /// Load an existing SimpleLogStreamVdev from metadata
    ///
    /// # Arguments
    /// * `vdev` - Underlying VirtualDev (already loaded with chunks)
    /// * `meta_client` - MetaClient for loading metablk
    /// * `num_sessions` - Number of concurrent flush sessions
    pub async fn load(
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
        num_sessions: usize,
    ) -> io::Result<Self> {
        let block_size = vdev.block_size();
        let vdev_id = vdev.vdev_id();
        
        // Load metablk by name
        let metablk_name = format!("simple_log_stream_{}", vdev_id);
        let meta_blk = meta_client.get_meta_blk(&metablk_name).await
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound,
                format!("Metablk {} not found", metablk_name)
            ))?;
        
        let append_meta_blk = MetaBlkWrapper::load(meta_client.clone(), meta_blk);
        
        // Load metadata (tail_offset and target_size) - zero-copy
        let buffer = append_meta_blk.read().await?;
        let metadata: SimpleLogStreamMetadata = bincode::deserialize(
            buffer.as_slice()
        ).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        // Create session buffers
        let session_buffers = (0..num_sessions)
            .map(|_| SessionBuffer::new(block_size))
            .collect();
        
        let append_log = Self {
            vdev,
            tail_offset: AtomicU64::new(metadata.tail_offset),
            target_size: metadata.target_size,
            session_buffers,
            append_meta_blk,
        };
        
        // Restore residual buffer by reading from disk
        let residual_buf = append_log.recovery_read_residual_buffer(
            metadata.tail_offset,
            block_size,
        ).await?;
        
        if !residual_buf.is_empty() {
            for session_buf in &append_log.session_buffers {
                *session_buf.residual_buf.lock() = residual_buf.clone();
            }
        }
        
        Ok(append_log)
    }
    
    /// Append data to the log
    ///
    /// Data is buffered in-memory until `flush()` is called.
    /// Takes ownership of data to avoid copying.
    pub async fn append(&self, session_id: FlushSessionId, data: Vec<u8>) -> io::Result<()> {
        let session_idx = (session_id % self.session_buffers.len() as u64) as usize;
        let session_buf = &self.session_buffers[session_idx];
        let data_len = data.len();
        
        // Track buffered size (lock-free)
        session_buf.buffered_size.fetch_add(data_len, Ordering::Relaxed);
        
        // Move Vec into queue (lock-free)
        session_buf.queue.push(data);
        
        Ok(())
    }
    
    /// Flush a session's buffered data to disk
    pub async fn flush(&self, session_id: FlushSessionId) -> io::Result<()> {
        let session_idx = (session_id % self.session_buffers.len() as u64) as usize;
        let session_buf = &self.session_buffers[session_idx];
        let block_size = session_buf.block_size;
        
        let mut residual_buf = session_buf.residual_buf.lock();
        let buffered_size = session_buf.buffered_size.load(Ordering::Relaxed);
        
        if buffered_size == 0 && residual_buf.is_empty() {
            return Ok(());
        }
        
        let current_tail = self.tail_offset.load(Ordering::Acquire);
        let mut write_offset = Self::round_down(current_tail, block_size as u64);
        let chunk_size = self.vdev.incremental_chunk_size();
        
        let mut current_residual = residual_buf.clone();
        residual_buf.clear();
        
        while !session_buf.queue.is_empty() || !current_residual.is_empty() {
            // Calculate space left in current chunk
            let offset_in_chunk = write_offset % chunk_size;
            let chunk_space = (chunk_size - offset_in_chunk) as usize;
            
            // Create IOBuffer for this chunk (rounded up to blocks)
            let physical_len = Self::round_up(chunk_space, block_size);
            let mut chunk_buf = IOBuffer::new(physical_len);
            let mut chunk_offset = 0;
            
            // First, add residual
            if !current_residual.is_empty() {
                let to_copy = current_residual.len().min(chunk_space);
                chunk_buf.as_mut_slice()[..to_copy].copy_from_slice(&current_residual[..to_copy]);
                chunk_offset += to_copy;
                current_residual.drain(..to_copy);
            }
            
            // Pop and append from queue
            while chunk_offset < chunk_space {
                if let Some(buf) = session_buf.queue.pop() {
                    let remaining_space = chunk_space - chunk_offset;
                    
                    if buf.len() <= remaining_space {
                        chunk_buf.as_mut_slice()[chunk_offset..chunk_offset + buf.len()]
                            .copy_from_slice(&buf);
                        chunk_offset += buf.len();
                    } else {
                        chunk_buf.as_mut_slice()[chunk_offset..chunk_space]
                            .copy_from_slice(&buf[..remaining_space]);
                        chunk_offset = chunk_space;
                        current_residual = buf[remaining_space..].to_vec();
                        break;
                    }
                } else {
                    break;
                }
            }
            
            if chunk_offset == 0 {
                break;
            }
            
            // Calculate which chunk index we need (0-based position in creation order)
            let nth_chunk = (write_offset / chunk_size) as usize;
            
            // Get or create the nth chunk (VirtualDev expands if needed). Note: SimpleLogStreamVdev 
            // doesn't use block allocators, so ignore is_new flag
            let (chunk, _is_new) = self.vdev.get_or_create_nth_chunk(nth_chunk).await?;
            self.write_to_chunk(&chunk, offset_in_chunk, chunk_buf).await?;
            
            write_offset += chunk_offset as u64;
        }
        
        // Save final residual
        *residual_buf = current_residual;
        
        let new_tail = current_tail + buffered_size as u64;
        self.tail_offset.store(new_tail, Ordering::Release);
        self.persist_metadata(new_tail).await?;
        
        drop(residual_buf);
        session_buf.reset();
        
        Ok(())
    }
    
    /// Read data from a specific offset (recovery only - called during initialization)
    ///
    /// WARNING: This is for recovery/initialization only, not for normal operations
    pub async fn recovery_read_at(&self, offset: u64, size: usize) -> io::Result<Vec<u8>> {
        let chunk_size = self.vdev.incremental_chunk_size();
        let block_size = self.vdev.block_size() as u64;
        
        // Round down to block boundary
        let block_aligned_offset = (offset / block_size) * block_size;
        let offset_adjustment = (offset - block_aligned_offset) as usize;
        
        let nth_chunk = (block_aligned_offset / chunk_size) as usize;
        let offset_in_chunk = block_aligned_offset % chunk_size;
        
        // Calculate blocks to read
        let total_size = offset_adjustment + size;
        let nblks = ((total_size as u64 + block_size - 1) / block_size) as u16;
        
        // Get nth chunk (sorted by creation_order)
        let chunk = self.vdev.get_nth_chunk(nth_chunk)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::InvalidInput,
                "Offset beyond stream size",
            ))?;
        
        let chunk_id = chunk.chunk_id();
        let blk_num = (offset_in_chunk / block_size) as u32;
        let blk_id = BlkId::new(blk_num, nblks, chunk_id as u16);
        
        // Read from VDev
        let buf = IOBuffer::new((nblks as usize) * (block_size as usize));
        let (res, buf) = self.vdev.read(buf, &blk_id).await;
        res?;
        
        // Extract requested portion
        Ok(buf.as_slice()[offset_adjustment..offset_adjustment + size].to_vec())
    }
    
    /// Truncate the log - reset tail to 0 and shrink back to target size
    /// 
    /// This removes any chunks that were added beyond the target allocation,
    /// effectively returning the vdev to its baseline capacity.
    pub async fn truncate(&self) -> io::Result<()> {
        self.tail_offset.store(0, Ordering::Release);
        
        // Clear residual buffers
        for session_buf in &self.session_buffers {
            session_buf.residual_buf.lock().clear();
            session_buf.buffered_size.store(0, Ordering::Release);
        }
        
        // Shrink back to target size by removing excess chunks from the end
        while self.vdev.size() > self.target_size {          
            // Shrink last chunk (safest - LIFO order)
            let before_size = self.vdev.size();
            let removed_chunk_id = self.vdev.shrink(crate::device::ChunkToShrink::Last).await?;
            
            let after_size = self.vdev.size();
            println!("SimpleLogStreamVdev: Removed chunk {} (size: {} -> {} bytes)",
                     removed_chunk_id, before_size, self.vdev.size());
            
            // Safety check: ensure we're making progress
            if after_size >= before_size {
                return Err(io::Error::new(io::ErrorKind::Other, 
                    "Shrink failed to reduce size - infinite loop prevention"));
            }
        }
        
        // Persist metadata
        self.persist_metadata(0).await?;
        Ok(())
    }
    
    /// Destroy this SimpleLogStreamVdev
    ///
    /// This will destroy the underlying VirtualDev (self-sufficient - handles everything).
    pub async fn destroy(self) -> io::Result<()> {
        // Free metadata block
        self.append_meta_blk.meta_client()
            .remove_meta_blk(self.append_meta_blk.meta_blk())
            .await?;
        
        // Destroy underlying VirtualDev (self-sufficient - handles everything)
        self.vdev.destroy().await?;
        
        Ok(())
    }
    
    // ===== Accessors =====
    
    pub fn tail_offset(&self) -> u64 {
        self.tail_offset.load(Ordering::Acquire)
    }
    
    pub fn vdev(&self) -> &Arc<VirtualDev> {
        &self.vdev
    }
    
    pub fn vdev_id(&self) -> u32 {
        self.vdev.vdev_id()
    }
    
    // ===== Private Helpers =====
    
    #[inline]
    fn round_up(size: usize, block_size: usize) -> usize {
        (size + block_size - 1) / block_size * block_size
    }
    
    #[inline]
    fn round_down(offset: u64, block_size: u64) -> u64 {
        (offset / block_size) * block_size
    }
    
    async fn write_to_chunk(
        &self,
        chunk: &Arc<crate::device::Chunk>,
        offset_in_chunk: u64,
        io_buffer: IOBuffer,
    ) -> io::Result<()> {
        let chunk_id = chunk.chunk_id();
        let block_size = self.vdev.block_size() as u64;
        
        let blk_num = (offset_in_chunk / block_size) as u32;
        let nblks = (Self::round_up(io_buffer.len(), block_size as usize) / block_size as usize) as u16;
        let blk_id = BlkId::new(blk_num, nblks, chunk_id as u16);
        
        self.vdev.write(&io_buffer, &blk_id).await?;
        Ok(())
    }
    
    async fn persist_metadata(&self, tail_offset: u64) -> io::Result<()> {
        let metadata = SimpleLogStreamMetadata { 
            tail_offset,
            target_size: self.target_size,
        };
        let serialized = bincode::serialize(&metadata)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        self.append_meta_blk.write(&serialized).await?;
        Ok(())
    }
    
    /// Read residual buffer from disk (recovery only - called during load())
    async fn recovery_read_residual_buffer(&self, tail_offset: u64, block_size: u32) -> io::Result<Vec<u8>> {
        let block_aligned_offset = (tail_offset / block_size as u64) * block_size as u64;
        let residual_size = (tail_offset - block_aligned_offset) as usize;
        
        if residual_size == 0 {
            return Ok(Vec::new());
        }
        
        let chunk_size = self.vdev.incremental_chunk_size();
        let nth_chunk = (block_aligned_offset / chunk_size) as usize;
        
        let chunk = self.vdev.get_nth_chunk(nth_chunk)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Chunk not found"))?;
        
        let chunk_id = chunk.chunk_id();
        let chunk_start_offset = nth_chunk as u64 * chunk_size;
        let offset_in_chunk = block_aligned_offset - chunk_start_offset;
        
        let blk_num = (offset_in_chunk / block_size as u64) as u32;
        let blk_id = BlkId::new(blk_num, 1, chunk_id as u16);
        
        let buf = IOBuffer::new(block_size as usize);
        let (res, buf) = self.vdev.read(buf, &blk_id).await;
        res?;
        
        Ok(buf.as_slice()[..residual_size].to_vec())
    }
}

/// Metadata for SimpleLogStreamVdev (minimal - tail_offset and target_size)
#[derive(Serialize, Deserialize)]
struct SimpleLogStreamMetadata {
    tail_offset: u64,
    target_size: u64,
}
