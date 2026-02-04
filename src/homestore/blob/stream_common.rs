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

pub type StreamId = u64;
pub type FlushSessionId = u64;
pub const MAX_FLUSH_SESSIONS: usize = 2;

/// Base trait for all log streams
pub trait LogStream: Send + Sync {
    /// Append data to stream
    async fn append(&self, session_id: FlushSessionId, data: Vec<u8>) -> io::Result<()>;
    
    /// Flush a session (cleanup happens automatically at end)
    async fn flush(&self, session_id: FlushSessionId) -> io::Result<()>;
    
    /// Get current tail offset
    fn tail_offset(&self) -> u64;
    
    /// Get stream ID
    fn stream_id(&self) -> StreamId;
}

/// Chunk pool for reusing freed chunks across streams
pub struct ChunkPool {
    /// Available chunks by size (chunk_size -> Vec<chunk_id>)
    pools: Mutex<HashMap<u64, Vec<u32>>>,
}

impl ChunkPool {
    pub fn new() -> Self {
        Self {
            pools: Mutex::new(HashMap::new()),
        }
    }
    
    /// Return chunks to pool after truncation
    pub fn return_chunks(&self, chunk_size: u64, chunk_ids: Vec<u32>) {
        let mut pools = self.pools.lock();
        pools.entry(chunk_size)
            .or_insert_with(Vec::new)
            .extend(chunk_ids);
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

/// Common chunk management for log streams
pub struct StreamChunkManager {
    stream_id: StreamId,
    vdev: Arc<VirtualDev>,
    device_mgr: Arc<DeviceManager>,
    chunk_pool: Arc<ChunkPool>,
    
    /// Ordered list of VDev chunks (RCU)
    chunks: ArcSwap<Vec<u32>>,
    
    /// Chunk size
    chunk_size: u64,
    
    /// Block size
    block_size: u32,
}

impl StreamChunkManager {
    pub fn new(
        stream_id: StreamId,
        vdev: Arc<VirtualDev>,
        device_mgr: Arc<DeviceManager>,
        chunk_pool: Arc<ChunkPool>,
        chunk_size: u64,
        initial_chunks: Vec<u32>,
    ) -> Self {
        let block_size = vdev.block_size();
        
        Self {
            stream_id,
            vdev,
            device_mgr,
            chunk_pool,
            chunks: ArcSwap::from_pointee(initial_chunks),
            chunk_size,
            block_size,
        }
    }
    
    /// Get stream ID
    pub fn stream_id(&self) -> StreamId {
        self.stream_id
    }
    
    /// Get current chunks
    pub fn chunks(&self) -> arc_swap::Guard<Arc<Vec<u32>>> {
        self.chunks.load()
    }
    
    /// Ensure chunk exists (try pool first, then allocate)
    pub async fn ensure_chunk_exists(&self, chunk_idx: usize) -> io::Result<()> {
        loop {
            let current_chunks = self.chunks.load();
            if chunk_idx < current_chunks.len() {
                return Ok(());
            }
            
            let mut new_chunks = (*current_chunks).clone();
            while new_chunks.len() <= chunk_idx {
                // Try to get from pool first
                let chunk_id = if let Some(pooled_chunk) = self.chunk_pool.try_get_chunk(self.chunk_size) {
                    pooled_chunk
                } else {
                    // Allocate new chunk
                    let chunk = self.device_mgr
                        .expand_vdev(&self.vdev, self.stream_id, self.chunk_size)
                        .await?;
                    chunk.chunk_id()
                };
                
                new_chunks.push(chunk_id);
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
            self.chunk_pool.return_chunks(self.chunk_size, chunks.to_vec());
        }
    }
    
    /// Return chunks before index to pool (for truncate)
    pub fn return_chunks_before(&self, chunk_idx: usize) {
        let current_chunks = self.chunks.load();
        
        if chunk_idx == 0 || chunk_idx >= current_chunks.len() {
            return;
        }
        
        // Return chunks [0..chunk_idx) to pool
        let chunks_to_return = current_chunks[..chunk_idx].to_vec();
        self.chunk_pool.return_chunks(self.chunk_size, chunks_to_return);
        
        // Update chunks list (remove freed chunks)
        let new_chunks = current_chunks[chunk_idx..].to_vec();
        self.chunks.store(Arc::new(new_chunks));
    }
    
    pub fn vdev(&self) -> &Arc<VirtualDev> {
        &self.vdev
    }
    
    pub fn block_size(&self) -> u32 {
        self.block_size
    }
    
    pub fn chunk_size(&self) -> u64 {
        self.chunk_size
    }
}
