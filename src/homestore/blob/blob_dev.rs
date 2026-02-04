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

use std::io;
use std::sync::Arc;
use std::collections::HashMap;
use crate::common::blk::{BlkId, BlkIds, MultiBlkId, BlkCount};
use crate::device::VirtualDev;
use crate::meta::{MetaClient, MetaBlk};
use iomgr::IOBuffer;
use super::{BlkReadTracker, BlobStreamId, SegmentId};
use crate::sisl::SimpleCache;

// Re-export device and common types for convenience
pub use crate::device::{HSDevType, VDevParameters, BlkAllocatorType, ChunkSelectorType};
pub use crate::common::{BlkAllocStatus, BlkAllocHints};

/// Default cache capacity for Cached mode (10 million entries)
const DEFAULT_CACHE_CAPACITY: usize = 10_000_000;

/// Blob device mode (set at creation/load time)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlobDeviceMode {
    /// Cached/Stream mode: writes cached and flushed at CP
    /// 
    /// Use for:
    /// - append_to_stream() caches writes, flushed at cp_flush()
    /// - read() checks cache first, then disk
    /// - free() removes from cache and frees blocks
    /// - Automatic checkpoint-based persistence
    Cached,
    
    /// Direct mode: writes immediate, user controls commit
    /// 
    /// Use for:
    /// - alloc_in_stream() + write() for explicit allocation/writing
    /// - User must call commit_blk() to commit
    /// - read() goes directly to disk (no caching)
    /// - free() directly frees blocks (no cache)
    /// - Full control over allocation and commit timing
    Direct,
}

/// Blob Device
/// 
/// Manages blob data storage with two modes:
/// - **Cached Mode**: Writes buffered and flushed at checkpoint
/// - **Direct Mode**: Writes immediate with explicit commit
/// 
/// Features:
/// - Virtual device management
/// - Block allocation and deallocation
/// - Asynchronous read/write operations
/// - Block tracking for safe concurrent access
/// - BlobStream management with per-chunk metadata
pub struct BlobDevice {
    /// Device name
    name: String,
    
    /// The underlying virtual device
    vdev: Arc<VirtualDev>,
    
    /// Block read tracker for managing concurrent reads
    blk_read_tracker: BlkReadTracker,
    
    /// MetaClient for managing metadata
    meta_client: Arc<MetaClient>,
    
    /// Active blob streams (stream_id -> BlobStream)
    streams: parking_lot::RwLock<HashMap<BlobStreamId, Arc<super::BlobStream>>>,
    
    /// Next stream ID
    next_stream_id: std::sync::atomic::AtomicU64,
    
    /// Device mode (immutable after creation)
    mode: BlobDeviceMode,
    
    /// Write cache (only used in Cached mode)
    /// BlkId -> IOBuffer mapping for cached writes
    write_cache: Option<Arc<SimpleCache<BlkId, IOBuffer>>>,
}

impl BlobDevice {
    /// Create a new blob device
    /// 
    /// This is the first-time boot path. It:
    /// 1. Registers as a meta client with MetaBlkManager
    /// 2. Takes an already-created vdev from DeviceManager
    /// 3. Initializes stream management
    /// 4. Sets up cache if in Cached mode
    /// 
    /// # Arguments
    /// * `dev_name` - Name for this blob device
    /// * `vdev` - The virtual device (already created by DeviceManager)
    /// * `mode` - Device mode (Cached or Direct)
    /// * `cache_capacity` - Cache capacity (only used if mode == Cached)
    /// 
    /// # Returns
    /// The created BlobDevice instance
    pub async fn create(
        dev_name: &str, 
        vdev: Arc<VirtualDev>,
        mode: BlobDeviceMode,
        cache_capacity: Option<usize>,
    ) -> io::Result<Self> {
        // Register as a meta client
        let meta_mgr = crate::common::managers::metablk_mgr();
        let meta_client = meta_mgr.register_client(format!("blob_dev_{}", dev_name)).await?;
        
        // Create cache only if Cached mode
        let write_cache = match mode {
            BlobDeviceMode::Cached => {
                let capacity = cache_capacity.unwrap_or(DEFAULT_CACHE_CAPACITY);
                Some(Arc::new(SimpleCache::new(capacity)))
            }
            BlobDeviceMode::Direct => None,
        };
        
        println!("Created BlobDevice '{}' in {:?} mode and registered as meta client", dev_name, mode);
        
        // Create the device instance
        Ok(Self {
            name: dev_name.to_string(),
            vdev,
            blk_read_tracker: BlkReadTracker::new(),
            meta_client: Arc::new(meta_client),
            streams: parking_lot::RwLock::new(HashMap::new()),
            next_stream_id: std::sync::atomic::AtomicU64::new(1),
            mode,
            write_cache,
        })
    }
    
    /// Load an existing blob device during recovery
    /// 
    /// This is the recovery path. It:
    /// 1. Registers as a meta client with MetaBlkManager (recovers if exists)
    /// 2. Takes an already-loaded vdev from DeviceManager
    /// 3. Loads all BlobStreams from recovered metadata
    /// 4. Sets up cache if in Cached mode
    /// 
    /// # Arguments
    /// * `dev_name` - Name for this blob device
    /// * `vdev` - The virtual device (already loaded by DeviceManager)
    /// * `mode` - Device mode (Cached or Direct)
    /// * `cache_capacity` - Cache capacity (only used if mode == Cached)
    /// 
    /// # Returns
    /// The loaded BlobDevice instance
    pub async fn load(
        dev_name: &str,
        vdev: Arc<VirtualDev>,
        mode: BlobDeviceMode,
        cache_capacity: Option<usize>,
    ) -> io::Result<Self> {
        // Register as a meta client (will recover if exists)
        let meta_mgr = crate::common::managers::metablk_mgr();
        let meta_client = meta_mgr.register_client(format!("blob_dev_{}", dev_name)).await?;
        
        // Step 1: Load all chunk metablks (blob_chunk_{id}) with allocator bitmaps
        let mut chunk_metablks = std::collections::HashMap::new();
        
        use futures::StreamExt;
        let mut recovered_chunk_mblks = meta_client.recovered_blocks();
        
        while let Some(result) = recovered_chunk_mblks.next().await {
            let (meta_blk, data) = result?;
            let header = meta_blk.header();
            let name = header.get_name();
            
            // Check if this is a chunk MetaBlk (format: "blob_chunk_<id>")
            if let Some(chunk_id_str) = name.strip_prefix("blob_chunk_") {
                if let Ok(chunk_id) = chunk_id_str.parse::<u32>() {
                    println!("Loaded chunk metablk for chunk {}", chunk_id);
                    chunk_metablks.insert(chunk_id, (meta_blk, data));
                }
            }
        }
        
        // Step 2: Load block allocators for all chunks
        let mut chunk_buffers = std::collections::HashMap::new();
        for (chunk_id, (_meta_blk, data)) in &chunk_metablks {
            chunk_buffers.insert(*chunk_id, data.clone());
        }
        vdev.load_blk_allocator(Some(chunk_buffers))?;

        // Step 3: Get stream-to-chunk mapping from VirtualDev and create BlobStream for each stream
        let mut max_stream_id = 0u64;
        let mut streams = std::collections::HashMap::new();

        let stream_chunks_map = vdev.get_all_stream_chunks();
        for (stream_id, chunk_ids) in stream_chunks_map {
            // Collect MetaBlks for this stream's chunks as HashMap
            let mut stream_chunk_metablks = std::collections::HashMap::new();
            for chunk_id in &chunk_ids {
                if let Some((meta_blk, _data)) = chunk_metablks.get(chunk_id) {
                    stream_chunk_metablks.insert(*chunk_id, meta_blk.clone());
                }
            }
            
            println!("Loading BlobStream {} with {} chunks: {:?}", stream_id, chunk_ids.len(), chunk_ids);
            
            // Load the stream
            let stream = super::BlobStream::load(
                stream_id,
                Arc::clone(&vdev),
                Arc::clone(&meta_client),
                stream_chunk_metablks,
                None,  // Use default config
            );
            
            streams.insert(stream_id, Arc::new(stream));
            max_stream_id = max_stream_id.max(stream_id);
        }
        
        // Create cache only if Cached mode
        let write_cache = match mode {
            BlobDeviceMode::Cached => {
                let capacity = cache_capacity.unwrap_or(DEFAULT_CACHE_CAPACITY);
                Some(Arc::new(SimpleCache::new(capacity)))
            }
            BlobDeviceMode::Direct => None,
        };
        
        println!("Loaded BlobDevice '{}' in {:?} mode with {} streams", dev_name, mode, streams.len());
        
        Ok(Self {
            name: dev_name.to_string(),
            vdev,
            blk_read_tracker: BlkReadTracker::new(),
            meta_client: Arc::new(meta_client),
            streams: parking_lot::RwLock::new(streams),
            next_stream_id: std::sync::atomic::AtomicU64::new(max_stream_id + 1),
            mode,
            write_cache,
        })
    }
    
    /// Get the device name
    pub fn name(&self) -> &str {
        &self.name
    }
    
    /// Get the underlying virtual device
    pub fn vdev(&self) -> &Arc<VirtualDev> {
        &self.vdev
    }
    
    /// Get the meta client
    pub fn meta_client(&self) -> &Arc<MetaClient> {
        &self.meta_client
    }
    
    /// Get the device mode
    pub fn mode(&self) -> BlobDeviceMode {
        self.mode
    }
    
    /// Create a new BlobStream
    /// 
    /// # Arguments
    /// * `config` - Optional configuration (uses default if None)
    /// 
    /// # Returns
    /// The new stream ID
    pub async fn create_stream(&self, config: Option<super::BlobStreamConfig>) -> io::Result<super::BlobStreamId> {
        let stream_id = self.next_stream_id.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        
        // Create the stream (expands with initial chunk)
        let stream = super::BlobStream::new(
            stream_id,
            Arc::clone(&self.vdev),
            Arc::clone(&self.meta_client),
            config,
        ).await?;
        
        // Add to streams map
        self.streams.write().insert(stream_id, Arc::new(stream));
        
        println!("BlobDevice '{}': Created stream {}", self.name, stream_id);
        Ok(stream_id)
    }
    
    /// Get a BlobStream by ID
    pub fn get_stream(&self, stream_id: super::BlobStreamId) -> Option<Arc<super::BlobStream>> {
        self.streams.read().get(&stream_id).cloned()
    }

    // ============================================================================
    // CACHED MODE API (only valid when mode == Cached)
    // ============================================================================
    
    /// Append data to a stream (CACHED MODE ONLY)
    /// 
    /// Flow:
    /// 1. Call BlobStream::append() to queue the write
    /// 2. Add (BlkId, IOBuffer) to SimpleCache
    /// 3. Actual flush happens during cp_flush()
    /// 
    /// # Panics
    /// Panics if called on a Direct mode device
    /// 
    /// # Arguments
    /// * `stream_id` - Stream to append to
    /// * `segment_id` - Segment within the stream (for temperature/priority)
    /// * `data` - Data buffer to write
    /// 
    /// # Returns
    /// BlkId where data will be written
    pub async fn append_to_stream(
        &self,
        stream_id: BlobStreamId,
        segment_id: SegmentId,
        data: IOBuffer,
    ) -> io::Result<BlkId> {
        assert_eq!(self.mode, BlobDeviceMode::Cached,
                   "append_to_stream() only valid in Cached mode");
        
        let stream = self.get_stream(stream_id)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, format!("Stream {} not found", stream_id)))?;
        
        // Append to stream (queues in WriteUnit)
        let blk_id = stream.append(segment_id, data.clone()).await?;
        
        // Cache the write (BlkId -> IOBuffer mapping)
        if let Some(cache) = &self.write_cache {
            cache.insert(blk_id.clone(), data);
        }
        
        Ok(blk_id)
    }
    
    // ============================================================================
    // DIRECT MODE API (only valid when mode == Direct)
    // ============================================================================
    
    /// Allocate blocks in a stream (DIRECT MODE ONLY)
    /// 
    /// User must call write() to persist data, then commit_blk() to commit.
    /// No CP protection - user ensures write+commit happen in same CP.
    /// 
    /// # Panics
    /// Panics if called on a Cached mode device
    /// 
    /// # Arguments
    /// * `stream_id` - Stream to allocate in
    /// * `nblks` - Number of blocks to allocate
    /// * `hints` - Allocation hints (optional)
    /// 
    /// # Returns
    /// Allocated BlkId
    pub async fn alloc_in_stream(
        &self,
        stream_id: BlobStreamId,
        nblks: BlkCount,
        hints: Option<BlkAllocHints>,
    ) -> io::Result<BlkId> {
        assert_eq!(self.mode, BlobDeviceMode::Direct, "alloc_in_stream() only valid in Direct mode");
        
        let _stream = self.get_stream(stream_id)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, format!("Stream {} not found", stream_id)))?;
        
        let hints = hints.unwrap_or_default();
        
        // Allocate in stream's chunks (via vdev)
        // This may trigger expansion if needed
        loop {
            let (status, blk_id_opt) = self.vdev.alloc_contiguous(nblks, &hints).await;
            
            match status {
                BlkAllocStatus::Success => {
                    return blk_id_opt.ok_or_else(||
                        io::Error::new(io::ErrorKind::Other, "alloc succeeded but no BlkId")
                    );
                }
                BlkAllocStatus::SpaceUnavail => {
                    // Expand stream and retry
                    let device_mgr = crate::common::managers::device_mgr();
                    device_mgr.expand_vdev(&self.vdev, stream_id, self.vdev.chunk_size()).await?;
                }
                _ => {
                    return Err(io::Error::new(
                        io::ErrorKind::Other,
                        format!("Allocation failed: {:?}", status)
                    ));
                }
            }
        }
    }

    /// Write data to allocated blocks (DIRECT MODE ONLY, NO AUTO-COMMIT)
    /// 
    /// User must call commit_blk() after write to commit the allocation.
    /// 
    /// # Panics
    /// Panics if called on a Cached mode device
    /// 
    /// # Arguments
    /// * `buf` - Data buffer to write
    /// * `bid` - Block ID to write to
    pub async fn write(&self, buf: &IOBuffer, bid: &BlkId) -> io::Result<()> {
        assert_eq!(self.mode, BlobDeviceMode::Direct,
                   "write() only valid in Direct mode");
        
        self.vdev.write(buf, bid).await?;
        // NO AUTO-COMMIT - user must call commit_blk()
        Ok(())
    }

    /// Write multiple buffers (DIRECT MODE ONLY, NO AUTO-COMMIT)
    /// 
    /// # Panics
    /// Panics if called on a Cached mode device
    /// 
    /// # Arguments
    /// * `buffers` - Data buffers to write
    /// * `bid` - Block ID to write to
    pub async fn writev(&self, buffers: Vec<IOBuffer>, bid: &BlkId) -> io::Result<()> {
        assert_eq!(self.mode, BlobDeviceMode::Direct, "writev() only valid in Direct mode");
        self.vdev.writev(buffers, bid).await?;
        // NO AUTO-COMMIT - user must call commit_blk()
        Ok(())
    }
    
    /// Commit allocated/written blocks (DIRECT MODE ONLY)
    /// 
    /// Must be called after write() to make the allocation permanent.
    /// 
    /// # Panics
    /// Panics if called on a Cached mode device
    /// 
    /// # Arguments
    /// * `bid` - Block ID to commit
    pub fn commit_blk(&self, bid: &BlkId) {
        assert_eq!(self.mode, BlobDeviceMode::Direct, "commit_blk() only valid in Direct mode");
        self.vdev.commit_blk(bid);
    }

    /// Write single buffer to multiple block IDs (DIRECT MODE ONLY, NO AUTO-COMMIT)
    /// 
    /// Splits the buffer across multiple block IDs and writes each portion.
    /// Uses zero-copy slicing - no data is copied.
    /// 
    /// # Panics
    /// Panics if called on a Cached mode device or if buffer size doesn't match block capacity
    /// 
    /// # Arguments
    /// * `buf` - Data buffer to write (will be split)
    /// * `bids` - Vector of block IDs to write to
    pub async fn write_multi(&self, buf: &IOBuffer, bids: &BlkIds) -> io::Result<()> {
        assert_eq!(self.mode, BlobDeviceMode::Direct, "write_multi() only valid in Direct mode");
        
        let blk_size = self.vdev.block_size() as usize;
        let buf_len = buf.len();
        
        // Calculate total capacity of all blocks
        let total_capacity: usize = bids.iter()
            .map(|bid| bid.blk_count() as usize * blk_size)
            .sum();
        
        assert_eq!(buf_len, total_capacity, "Buffer size {} doesn't match total block capacity {}", 
            buf_len, total_capacity);
        
        // Zero-copy slices, direct write, no commit
        let mut offset = 0;
        for bid in bids {
            let chunk_size = bid.blk_count() as usize * blk_size;
            let chunk_view = buf.slice(offset, chunk_size);
            self.vdev.write(&chunk_view, bid).await?;
            offset += chunk_size;
        }
        
        Ok(())
    }

    /// Write multiple buffers to multiple block IDs (DIRECT MODE ONLY, NO AUTO-COMMIT)
    /// 
    /// Writes buffers to corresponding block IDs. Total buffer size must match
    /// total block capacity. Uses zero-copy slicing when buffers need to be split.
    /// 
    /// # Panics
    /// Panics if called on a Cached mode device or if buffer size doesn't match block capacity
    /// 
    /// # Arguments
    /// * `buffers` - Vector of data buffers to write
    /// * `bids` - Vector of block IDs to write to
    pub async fn writev_multi(&self, buffers: Vec<IOBuffer>, bids: &BlkIds) -> io::Result<()> {
        assert_eq!(self.mode, BlobDeviceMode::Direct, "writev_multi() only valid in Direct mode");
        
        let blk_size = self.vdev.block_size() as usize;
        
        // Calculate total sizes
        let total_buf_size: usize = buffers.iter().map(|b| b.len()).sum();
        let total_capacity: usize = bids.iter()
            .map(|bid| bid.blk_count() as usize * blk_size)
            .sum();
        
        assert_eq!(total_buf_size, total_capacity, "Total buffer size {} doesn't match total block capacity {}",
            total_buf_size, total_capacity);
        
        // Distribute buffers across block IDs and write immediately, no commit
        let mut buf_iter = buffers.into_iter();
        let mut current_buf: Option<IOBuffer> = None;
        let mut buf_offset = 0;
        
        for bid in bids {
            let mut remaining = bid.blk_count() as usize * blk_size;
            let mut chunks = Vec::new();
            
            while remaining > 0 {
                if current_buf.is_none() {
                    current_buf = buf_iter.next();
                    buf_offset = 0;
                }
                
                if let Some(ref buf) = current_buf {
                    let available = buf.len() - buf_offset;
                    let to_take = remaining.min(available);
                    
                    let chunk = buf.slice(buf_offset, to_take);
                    chunks.push(chunk);
                    
                    buf_offset += to_take;
                    remaining -= to_take;
                    
                    if buf_offset >= buf.len() {
                        current_buf = None;
                        buf_offset = 0;
                    }
                } else {
                    return Err(io::Error::new(io::ErrorKind::InvalidInput, "Not enough buffer data for block IDs"));
                }
            }
            
            if chunks.len() == 1 {
                self.vdev.write(&chunks[0], bid).await?;
            } else {
                self.vdev.writev(chunks, bid).await?;
            }
        }
        
        Ok(())
    }

    // ============================================================================
    // COMMON API (Both modes, behavior depends on mode)
    // ============================================================================
    
    /// Read data from blocks
    /// 
    /// - **Cached mode**: Checks cache first, then disk. Populates cache on miss.
    /// - **Direct mode**: Reads directly from disk, no caching.
    /// 
    /// **BlkReadTracker**: Tracks this read to prevent concurrent frees
    /// 
    /// # Arguments
    /// * `bid` - Block ID to read
    /// * `buf` - Buffer to read into
    /// 
    /// # Returns
    /// Number of bytes read
    pub async fn read(&self, bid: &BlkId, buf: &mut [u8]) -> io::Result<usize> {
        // Lambda: Check cache (Cached mode only)
        let check_cache = || -> Option<usize> {
            if self.mode == BlobDeviceMode::Cached {
                if let Some(cache) = &self.write_cache {
                    if let Some(cached_data) = cache.get(bid) {
                        let len = cached_data.len().min(buf.len());
                        buf[..len].copy_from_slice(&cached_data.as_bytes()[..len]);
                        return Some(len);
                    }
                }
            }
            None
        };
      
        // Lambda: Tracked disk read (COMMON - both modes)
        let tracked_disk_read = || async {
            self.blk_read_tracker.insert(bid);
            let result = self.vdev.read(bid, buf).await;
            self.blk_read_tracker.remove(bid);
            result
        };
        
        // Lambda: Populate cache on miss (Cached mode only)
        let populate_cache = |len: usize| {
            if self.mode == BlobDeviceMode::Cached {
                if let Some(cache) = &self.write_cache {
                    let data = IOBuffer::from(&buf[..len]);
                    cache.insert(bid.clone(), data);
                }
            }
        };
        
        // Fast path: cache hit
        if let Some(len) = check_cache() {
            return Ok(len);
        }

        // Execute: disk read + cache populate
        let len = tracked_disk_read().await?;
        populate_cache(len);
        
        Ok(len)
    }
    
    /// Free blocks
    /// 
    /// - **Cached mode**: Removes from cache and frees blocks
    /// - **Direct mode**: Directly frees blocks (no cache interaction)
    /// 
    /// **BlkReadTracker**: Waits for all pending reads to complete before freeing
    /// 
    /// # Arguments
    /// * `bid` - Block ID to free
    pub async fn free(&self, bid: &BlkId) -> io::Result<()> {
        // Lambda: Remove from cache (Cached mode only)
        let remove_from_cache = || {
            if self.mode == BlobDeviceMode::Cached {
                if let Some(cache) = &self.write_cache {
                    cache.remove(bid);
                }
            }
        };
        
        // Lambda: Safe free with tracking (COMMON - both modes)
        let safe_free = || async {
            // Wait for all pending reads to complete
            self.blk_read_tracker.wait_on(bid).await
                .map_err(|_| io::Error::new(
                    io::ErrorKind::Other,
                    "Failed to wait for pending reads"
                ))?;
            
            // Free in vdev (safe now - no pending reads)
            self.vdev.free_blk(&[bid.clone()])?;
            
            Ok::<(), io::Error>(())
        };
        
        // Execute: cache removal + safe free
        remove_from_cache();
        safe_free().await
    }
    
    /// Checkpoint flush - flush all BlobStreams (CACHED MODE ONLY)
    /// 
    /// This is called during checkpoint to persist all buffered writes
    /// and write allocator bitmaps to chunk metablks.
    /// 
    /// No-op in Direct mode.
    /// 
    /// # Arguments
    /// * `cp_id` - Checkpoint ID to flush
    pub async fn cp_flush(&self, cp_id: u64) -> io::Result<()> {
        if self.mode != BlobDeviceMode::Cached {
            return Ok(()); // No-op in Direct mode
        }
        
        let streams: Vec<Arc<super::BlobStream>> = self.streams
            .read()
            .values()
            .cloned()
            .collect();
        
        println!("BlobDevice '{}': CP {} flush starting for {} streams", self.name, cp_id, streams.len());
        
        for stream in streams {
            stream.cp_flush(cp_id).await?;
        }
        
        println!("BlobDevice '{}': CP {} flush complete", self.name, cp_id);
        Ok(())
    }
    
    /// Checkpoint cleanup (CACHED MODE ONLY)
    /// 
    /// This is called during checkpoint cleanup phase to reset the checkpoint context.
    /// 
    /// No-op in Direct mode.
    /// 
    /// # Arguments
    /// * `cp_id` - Checkpoint ID to cleanup
    pub fn cp_cleanup(&self, cp_id: u64) {
        if self.mode != BlobDeviceMode::Cached {
            return; // No-op in Direct mode
        }
        
        let streams: Vec<Arc<super::BlobStream>> = self.streams
            .read()
            .values()
            .cloned()
            .collect();
        
        println!("BlobDevice '{}': CP {} cleanup starting for {} streams", self.name, cp_id, streams.len());
        
        for stream in streams {
            stream.cp_cleanup(cp_id);
        }
        
        println!("BlobDevice '{}': CP {} cleanup complete", self.name, cp_id);
    }

    /// Allocates blocks with a single MultiBlkId result
    /// 
    /// # Arguments
    /// * `size` - Size in bytes to allocate
    /// * `hints` - Allocation hints
    /// 
    /// # Returns
    /// Status and allocated block IDs
    pub fn alloc_blks(
        &self,
        _size: u32,
        _hints: &BlkAllocHints,
    ) -> (BlkAllocStatus, MultiBlkId) {
        // TODO: Implement allocation
        todo!("alloc_blks implementation")
    }

    /// Allocates blocks across potentially different chunks
    /// 
    /// # Arguments
    /// * `size` - Size in bytes to allocate
    /// * `hints` - Allocation hints
    /// 
    /// # Returns
    /// Status and vector of allocated block IDs
    pub fn alloc_blks_vec(
        &self,
        _size: u32,
        _hints: &BlkAllocHints,
    ) -> (BlkAllocStatus, Vec<BlkId>) {
        // TODO: Implement allocation to vector
        todo!("alloc_blks_vec implementation")
    }

    /// Asynchronously frees blocks
    /// 
    /// Waits for pending reads to complete before freeing
    /// 
    /// # Arguments
    /// * `blkid` - Block ID(s) to free
    pub async fn async_free_blk(&self, _blkid: &MultiBlkId) -> io::Result<()> {
        // TODO: Implement async free
        todo!("async_free_blk implementation")
    }

    /// Gets the block size for this service
    pub fn get_blk_size(&self) -> u32 {
        self.vdev.block_size()
    }

    /// Gets the alignment size
    pub fn get_align_size(&self) -> u32 {
        self.vdev.align_size()
    }

    /// Gets the block read tracker
    pub fn read_blk_tracker(&self) -> &BlkReadTracker {
        &self.blk_read_tracker
    }

    /// Starts the blob device service
    /// 
    /// Registers with checkpoint manager for flush operations
    pub async fn start(&self) -> io::Result<()> {
        // TODO: Implement start/registration
        todo!("start implementation")
    }

    /// Stops the blob device service
    pub async fn stop(&self) -> io::Result<()> {
        // TODO: Implement stop
        todo!("stop implementation")
    }

    /// Gets the total capacity of the service
    pub fn get_total_capacity(&self) -> u64 {
        // TODO: Get from vdev
        todo!("get_total_capacity implementation")
    }

    /// Gets the used capacity of the service
    pub fn get_used_capacity(&self) -> u64 {
        // TODO: Get from vdev
        todo!("get_used_capacity implementation")
    }

    /// Gets the device type
    pub fn get_dev_type(&self) -> HSDevType {
        // TODO: Get from vdev
        todo!("get_dev_type implementation")
    }
}

/// Virtual device information (placeholder)
/// TODO: Define proper VDevInfo struct
#[derive(Debug, Clone)]
pub struct VDevInfo {
    pub blk_size: u32,
    // TODO: Add other fields as needed
}

#[cfg(test)]
mod tests {
    // Tests for BlobDev are integrated in blob_mgr.rs tests
    // since BlobDev requires a VirtualDev instance
}


