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

// Import StreamMetadata from new_blob_dev
use super::new_blob_dev::StreamMetadata;

pub type StreamId = u64;
pub type FlushSessionId = u64;
pub const MAX_FLUSH_SESSIONS: usize = 2;

/// StreamChunk - encapsulates a chunk with optional per-chunk metadata
pub struct StreamChunk {
    /// The underlying chunk (from VDev)
    pub base_chunk: Arc<crate::homestore::device::Chunk>,
    
    /// Optional per-chunk metadata (can be None for append-only chunks)
    pub chunk_meta_blk: Option<MetaBlk>,
}

impl StreamChunk {
    pub fn new(base_chunk: Arc<crate::homestore::device::Chunk>, chunk_meta_blk: Option<MetaBlk>) -> Self {
        Self {
            base_chunk,
            chunk_meta_blk,
        }
    }
    
    pub fn chunk_id(&self) -> u32 {
        self.base_chunk.chunk_id()
    }
}

/// Chunk pool for reusing freed chunks across streams with capacity limits
pub struct ChunkPool {
    /// Available chunks by size (chunk_size -> Vec<chunk_id>)
    pools: Mutex<HashMap<u64, Vec<u32>>>,
    
    /// Maximum chunks to keep per size
    max_chunks_per_size: usize,
    
    /// VirtualDev for returning excess chunks
    vdev: Arc<VirtualDev>,
}

impl ChunkPool {
    pub fn new(vdev: Arc<VirtualDev>, max_chunks_per_size: usize) -> Self {
        Self {
            pools: Mutex::new(HashMap::new()),
            max_chunks_per_size,
            vdev,
        }
    }
    
    /// Return chunks to pool after truncation
    /// If pool exceeds max_chunks_per_size, excess chunks are freed back to VDev
    pub fn return_chunks(&self, chunk_size: u64, chunk_ids: Vec<u32>) -> io::Result<()> {
        let excess_chunks = {
            let mut pools = self.pools.lock();
            let pool = pools.entry(chunk_size).or_insert_with(Vec::new);
            
            let total_count = pool.len() + chunk_ids.len();
            
            if total_count <= self.max_chunks_per_size {
                // All fit in pool
                pool.extend(chunk_ids);
                Vec::new()
            } else {
                // Some chunks exceed limit
                let space_left = self.max_chunks_per_size.saturating_sub(pool.len());
                pool.extend(chunk_ids.iter().take(space_left).copied());
                chunk_ids.into_iter().skip(space_left).collect()
            }
        };
        
        // Free excess chunks back to VDev
        for chunk_id in excess_chunks {
            self.vdev.remove_chunk(chunk_id)?;
        }
        
        Ok(())
    }
    
    /// Try to get a chunk from pool (or None if empty)
    pub fn try_get_chunk(&self, chunk_size: u64) -> Option<u32> {
        let mut pools = self.pools.lock();
        pools.get_mut(&chunk_size)?.pop()
    }
    
    /// Get count of available chunks for a size
    pub fn available_count(&self, chunk_size: u64) -> usize {
        self.pools.lock()
            .get(&chunk_size)
            .map(|v| v.len())
            .unwrap_or(0)
    }
}

/// StreamBase - Common chunk management for all stream types
///
/// Responsibilities:
/// - Manages ordered list of StreamChunks (chunk + metadata as one unit)
/// - Owns its own ChunkPool for chunk reuse (not shared with other streams)
/// - Expands stream by allocating/reusing chunks
/// - Provides RCU-based chunk list updates
/// - Owns all metadata blocks (stream + chunks)
pub struct StreamBase {
    stream_id: StreamId,
    vdev: Arc<VirtualDev>,
    
    /// ChunkPool owned by this stream (not shared)
    chunk_pool: ChunkPool,
    
    /// Stream metadata
    pub metadata: StreamMetadata,
    
    /// Ordered list of StreamChunks (chunk + metablk as one unit) (RCU)
    chunks: ArcSwap<Vec<StreamChunk>>,
    
    /// Block size
    block_size: u32,
    
    /// Stream-level metadata block
    stream_mblk: MetaBlk,
}

impl StreamBase {
    /// Create new StreamBase with empty chunk list
    pub fn new(
        vdev: Arc<VirtualDev>,
        metadata: StreamMetadata,
        stream_mblk: MetaBlk,
    ) -> Self {
        let block_size = vdev.block_size();
        
        // Create ChunkPool for this stream
        let chunk_pool = ChunkPool::new(vdev.clone(), metadata.max_chunks_in_pool);
        
        Self {
            stream_id: metadata.stream_id,
            vdev,
            chunk_pool,
            chunks: ArcSwap::from_pointee(Vec::new()),
            block_size,
            metadata,
            stream_mblk,
        }
    }
    
    /// Load StreamBase with existing chunks and their metadata
    pub fn load(
        vdev: Arc<VirtualDev>,
        metadata: StreamMetadata,
        stream_mblk: MetaBlk,
        chunk_metablks: Vec<MetaBlk>,
    ) -> io::Result<Self> {
        let block_size = vdev.block_size();
        let chunk_pool = ChunkPool::new(vdev.clone(), metadata.max_chunks_in_pool);
        
        // Build StreamChunks by pairing chunks with their metadata
        let mut stream_chunks = Vec::new();
        for (idx, &chunk_id) in metadata.chunk_list.iter().enumerate() {
            let base_chunk = vdev.get_chunk(chunk_id)
                .ok_or_else(|| io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("Chunk {} not found in VDev", chunk_id)
                ))?;
            
            // For AppendOnlyStream, chunk_metablks is empty (None for all)
            // For BlkStream, chunk_metablks[idx] corresponds to chunk_list[idx]
            let chunk_meta_blk = chunk_metablks.get(idx).cloned();
            
            stream_chunks.push(StreamChunk::new(base_chunk, chunk_meta_blk));
        }
        
        Ok(Self {
            stream_id: metadata.stream_id,
            vdev,
            chunk_pool,
            chunks: ArcSwap::from_pointee(stream_chunks),
            block_size,
            metadata,
            stream_mblk,
        })
    }
    
    /// Get reference to stream metablk
    pub fn stream_mblk(&self) -> &MetaBlk {
        &self.stream_mblk
    }
    
    /// Get stream ID
    pub fn stream_id(&self) -> StreamId {
        self.stream_id
    }
    
    /// Get current chunks
    pub fn chunks(&self) -> arc_swap::Guard<Arc<Vec<StreamChunk>>> {
        self.chunks.load()
    }
    
    /// Get chunk IDs (for metadata serialization)
    pub fn get_chunk_ids(&self) -> Vec<u32> {
        self.chunks.load().iter().map(|sc| sc.chunk_id()).collect()
    }
    
    /// Ensure chunk exists (try pool first, then allocate)
    /// 
    /// Note: Metadata persistence happens during flush. If crash occurs between
    /// chunk allocation and metadata write, orphaned chunks are deleted on next boot.
    /// 
    /// chunk_meta_blk_fn: Optional function to create per-chunk metadata (for BlkStream)
    pub async fn ensure_chunk_exists<F>(
        &self,
        chunk_idx: usize,
        chunk_meta_blk_fn: Option<F>,
    ) -> io::Result<()>
    where
        F: Fn(u32) -> Option<MetaBlk>,
    {
        loop {
            let current_chunks = self.chunks.load();
            if chunk_idx < current_chunks.len() {
                return Ok(());
            }
            
            let mut new_chunks = (*current_chunks).clone();
            
            while new_chunks.len() <= chunk_idx {
                // Try to get from pool first
                let chunk = if let Some(pooled_chunk_id) = self.chunk_pool.try_get_chunk(self.metadata.chunk_size) {
                    self.vdev.get_chunk(pooled_chunk_id)
                        .ok_or_else(|| io::Error::new(
                            io::ErrorKind::NotFound,
                            format!("Pooled chunk {} not found", pooled_chunk_id)
                        ))?
                } else {
                    // Allocate new chunk using singleton device_mgr
                    let device_mgr = crate::homestore::common::managers::device_mgr();
                    device_mgr
                        .expand_vdev(&self.vdev, self.stream_id, self.metadata.chunk_size)
                        .await?
                };
                
                let chunk_id = chunk.chunk_id();
                
                // Create per-chunk metadata if provided (for BlkStream)
                let chunk_meta_blk = chunk_meta_blk_fn.as_ref().and_then(|f| f(chunk_id));
                
                new_chunks.push(StreamChunk::new(chunk, chunk_meta_blk));
            }
            
            match self.chunks.compare_and_swap(&current_chunks, Arc::new(new_chunks)) {
                Ok(_) => return Ok(()),
                Err(_) => continue,
            }
        }
    }
    
    /// Return all chunks to pool (for delete)
    pub fn return_all_chunks_to_pool(&self) {
        let chunks = self.chunks.load();
        if !chunks.is_empty() {
            let chunk_ids: Vec<u32> = chunks.iter().map(|sc| sc.chunk_id()).collect();
            let _ = self.chunk_pool.return_chunks(self.metadata.chunk_size, chunk_ids);
        }
    }
    
    /// Return chunks before index to pool (for truncate)
    pub fn return_chunks_before(&self, chunk_idx: usize) {
        let current_chunks = self.chunks.load();
        
        if chunk_idx == 0 || chunk_idx >= current_chunks.len() {
            return;
        }
        
        // Return chunks [0..chunk_idx) to pool
        let chunk_ids: Vec<u32> = current_chunks[..chunk_idx]
            .iter()
            .map(|sc| sc.chunk_id())
            .collect();
        let _ = self.chunk_pool.return_chunks(self.metadata.chunk_size, chunk_ids);
        
        // Update chunks list (remove freed chunks)
        let new_chunks = current_chunks[chunk_idx..].to_vec();
        self.chunks.store(Arc::new(new_chunks));
    }
    
    /// Destroy stream base - delete all metadata and remove chunks from VDev
    /// 
    /// Handles complete cleanup:
    /// 1. Deletes stream metadata
    /// 2. Deletes chunk metadata (from StreamChunks)
    /// 3. Removes all chunks from VDev
    pub async fn destroy(
        &self,
        meta_client: &Arc<crate::homestore::meta::meta_client::MetaClient>,
    ) -> io::Result<()> {
        // 1. Delete stream metadata (remove_meta_blk already calls free)
        meta_client.remove_meta_blk(&self.stream_mblk).await?;
        
        // 2. Delete per-chunk metadata blocks and remove chunks from VDev
        let stream_chunks = self.chunks.load();
        for stream_chunk in stream_chunks.iter() {
            // Delete chunk metadata if it exists
            if let Some(ref chunk_metablk) = stream_chunk.chunk_meta_blk {
                meta_client.remove_meta_blk(chunk_metablk).await?;
            }
            
            // Remove chunk from VDev
            self.vdev.remove_chunk(stream_chunk.chunk_id())?;
        }
        
        Ok(())
    }
    
    pub fn vdev(&self) -> &Arc<VirtualDev> {
        &self.vdev
    }
    
    pub fn block_size(&self) -> u32 {
        self.block_size
    }
    
    pub fn chunk_size(&self) -> u64 {
        self.metadata.chunk_size
    }
}
