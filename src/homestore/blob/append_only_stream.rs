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

use crate::device::virtual_dev::VirtualDev;
use crate::meta::meta_client::MetaClient;
use crate::meta::meta_blk::MetaBlk;
use crate::blob::types::BlkId;
use iomgr::IOBuffer;

// Import common stream infrastructure
use super::stream_base::{
    StreamBase,
    StreamId, FlushSessionId, MAX_FLUSH_SESSIONS,
};
use super::new_blob_dev::StreamMetadata;

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

/// Append-Only Stream
pub struct AppendOnlyStream {
    stream_base: StreamBase,
    
    /// Logical stream offset (tail position)
    stream_tail_offset: AtomicU64,
    
    /// Per-session buffers
    session_buffers: [SessionBuffer; MAX_FLUSH_SESSIONS],
    
    /// Meta client
    meta_client: Arc<MetaClient>,
}

impl AppendOnlyStream {
    /// Create new append-only stream
    pub async fn new(
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
        metadata: StreamMetadata,
        mut stream_mblk: MetaBlk,
    ) -> io::Result<Self> {
        let block_size = vdev.block_size();
        
        // Write metadata ONCE after initialization
        let append_metadata = AppendOnlyMetadata {
            base: metadata.clone(),
            stream_tail_offset: 0,
        };
        
        let metadata_bytes = bincode::serialize(&append_metadata)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        stream_mblk.write_data(&metadata_bytes, &meta_client.meta_vdev).await?;
        
        // Create StreamBase (takes ownership of stream_mblk)
        let stream_base = StreamBase::new(vdev, metadata.clone(), stream_mblk);
        
        Ok(Self {
            stream_base,
            stream_tail_offset: AtomicU64::new(0),
            session_buffers: [
                SessionBuffer::new(block_size),
                SessionBuffer::new(block_size),
            ],
            meta_client,
        })
    }
    
    /// Load existing append-only stream
    pub async fn load(
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
        stream_metadata: StreamMetadata,
        stream_mblk: MetaBlk,
    ) -> io::Result<Self> {
        let block_size = vdev.block_size();
        
        // Load metadata
        let metadata: AppendOnlyMetadata = bincode::deserialize(
            &stream_mblk.read_data(&meta_client.meta_vdev).await?
        ).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        // Load StreamBase with existing chunks (AppendOnlyStream has no per-chunk metadata)
        let stream_base = StreamBase::load(vdev, stream_metadata, stream_mblk, Vec::new())?;
        
        let stream = Self {
            stream_base,
            stream_tail_offset: AtomicU64::new(metadata.stream_tail_offset),
            session_buffers: [
                SessionBuffer::new(block_size),
                SessionBuffer::new(block_size),
            ],
            meta_client,
        };
        
        // Restore residual buffer by reading from disk
        let residual_buf = stream.read_residual_buffer(metadata.stream_tail_offset, block_size).await?;
        if !residual_buf.is_empty() {
            *stream.session_buffers[0].residual_buf.lock() = residual_buf.clone();
            *stream.session_buffers[1].residual_buf.lock() = residual_buf;
        }
        
        Ok(stream)
    }

    /// Append data - takes ownership of data.
    pub async fn append(&self, session_id: FlushSessionId, data: Vec<u8>) -> io::Result<()> {
        let session_idx = (session_id % MAX_FLUSH_SESSIONS as u64) as usize;
        let session_buf = &self.session_buffers[session_idx];
        let data_len = data.len();
        
        // Track buffered size (lock-free)
        session_buf.buffered_size.fetch_add(data_len, Ordering::Relaxed);
        
        // Move Vec into queue (lock-free)
        session_buf.queue.push(data);
        
        Ok(())
    }
    
    /// Flush a session (cleanup happens automatically at end)
    pub async fn flush(&self, session_id: FlushSessionId) -> io::Result<()> {
        let session_idx = (session_id % MAX_FLUSH_SESSIONS as u64) as usize;
        let session_buf = &self.session_buffers[session_idx];
        let block_size = session_buf.block_size;
        
        let mut residual_buf = session_buf.residual_buf.lock();
        let buffered_size = session_buf.buffered_size.load(Ordering::Relaxed);
        
        if buffered_size == 0 && residual_buf.is_empty() {
            return Ok(());
        }
        
        let current_tail = self.stream_tail_offset.load(Ordering::Acquire);
        let mut write_offset = Self::round_down(current_tail, block_size as u64);
        let chunk_size = self.stream_base.chunk_size();
        
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
                current_residual.drain(..to_copy); // Remove consumed bytes
            }
            
            // Pop and append from queue
            while chunk_offset < chunk_space {
                if let Some(buf) = session_buf.queue.pop() {
                    let remaining_space = chunk_space - chunk_offset;
                    
                    if buf.len() <= remaining_space {
                        // Fits entirely
                        chunk_buf.as_mut_slice()[chunk_offset..chunk_offset + buf.len()].copy_from_slice(&buf);
                        chunk_offset += buf.len();
                    } else {
                        // Partial fit, rest becomes residual
                        chunk_buf.as_mut_slice()[chunk_offset..chunk_space].copy_from_slice(&buf[..remaining_space]);
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
            
            // Write this chunk
            let chunk_idx = (write_offset / chunk_size) as usize;
            self.stream_base.ensure_chunk_exists(chunk_idx, None::<fn(u32) -> Option<MetaBlk>>).await?;
            self.write_to_chunk(chunk_idx, offset_in_chunk, chunk_buf).await?;
            
            write_offset += chunk_offset as u64;
        }
        
        // Save final residual
        *residual_buf = current_residual;
        
        let new_tail = current_tail + buffered_size as u64;
        self.stream_tail_offset.store(new_tail, Ordering::Release);
        self.persist_metadata(new_tail).await?;
        
        drop(residual_buf);
        session_buf.reset();
        
        Ok(())
    }
    
    /// Truncate stream - reset logical offset to 0, reuse existing chunks
    /// 
    /// This resets the tail offset to 0 so next append will overwrite from the start.
    /// All chunks remain allocated and can be reused.
    pub async fn truncate(&self) -> io::Result<()> {
        // Reset logical tail offset
        self.stream_tail_offset.store(0, Ordering::Release);
        
        // Clear residual buffers
        for session_buf in &self.session_buffers {
            session_buf.residual_buf.lock().clear();
            session_buf.buffered_size.store(0, Ordering::Release);
        }
        
        // Persist metadata with new tail offset
        self.persist_metadata(0).await?;
        
        Ok(())
    }
    
    /// Destroy stream - completely removes the chunk and its metadata
    pub async fn destroy(self) -> io::Result<()> {
        // Delegate everything to StreamBase
        self.stream_base.destroy(&self.meta_client).await
    }
    
    /// Get current chunk list
    pub fn get_chunk_list(&self) -> Vec<u32> {
        self.stream_base.get_chunk_ids()
    }
    
    // Helper: Round up to block size
    #[inline]
    fn round_up(size: usize, block_size: usize) -> usize {
        (size + block_size - 1) / block_size * block_size
    }
    
    // Helper: Round down to block size
    #[inline]
    fn round_down(offset: u64, block_size: u64) -> u64 {
        (offset / block_size) * block_size
    }
    
    /// Write IOBuffer to specific VDev chunk
    async fn write_to_chunk(
        &self,
        chunk_idx: usize,
        offset_in_chunk: u64,
        io_buffer: IOBuffer,
    ) -> io::Result<()> {
        let chunks = self.stream_base.chunks();
        let chunk_id = chunks[chunk_idx].chunk_id();
        let block_size = self.stream_base.block_size();
        
        let blk_num = (offset_in_chunk / block_size as u64) as u32;
        let nblks = (Self::round_up(io_buffer.len(), block_size as usize) / block_size as usize) as u16;
        let blk_id = BlkId::new(blk_num, nblks, chunk_id);
        
        self.stream_base.vdev().write(&io_buffer, &blk_id).await?;
        Ok(())
    }
    
    /// Persist metadata
    async fn persist_metadata(&self, tail_offset: u64) -> io::Result<()> {
        let base_metadata = super::new_blob_dev::StreamMetadata {
            stream_id: self.stream_base.stream_id(),
            stream_type: super::new_blob_dev::StreamType::AppendOnlyStream,
            chunk_list: self.stream_base.get_chunk_ids(),
            chunk_size: self.stream_base.chunk_size(),
            max_chunks_in_pool: self.stream_base.metadata.max_chunks_in_pool,
        };
        
        let metadata = AppendOnlyMetadata {
            base: base_metadata,
            stream_tail_offset: tail_offset,
        };
        
        let serialized = bincode::serialize(&metadata)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        self.stream_base.stream_mblk().write_data(&serialized, &self.meta_client.meta_vdev).await?;
        Ok(())
    }
    
    /// Read residual buffer from disk (on load)
    async fn read_residual_buffer(&self, tail_offset: u64, block_size: u32) -> io::Result<Vec<u8>> {
        let block_aligned_offset = (tail_offset / block_size as u64) * block_size as u64;
        let residual_size = (tail_offset - block_aligned_offset) as usize;
        
        if residual_size == 0 {
            return Ok(Vec::new());
        }
        
        // Read the last block containing the residual
        let chunk_size = self.stream_base.chunk_size();
        let chunk_idx = (block_aligned_offset / chunk_size) as usize;
        
        if chunk_idx >= self.stream_base.chunks().len() {
            return Ok(Vec::new());
        }
        
        let chunks = self.stream_base.chunks();
        let chunk_id = chunks[chunk_idx];
        let chunk_start_offset = chunk_idx as u64 * chunk_size;
        let offset_in_chunk = block_aligned_offset - chunk_start_offset;
        
        let blk_num = (offset_in_chunk / block_size as u64) as u32;
        let blk_id = BlkId::new(blk_num, 1, chunk_id);
        
        // Read the block
        let block_data = self.stream_base.vdev().read(&blk_id).await?;
        
        // Extract residual portion
        Ok(block_data[..residual_size].to_vec())
    }
 
    pub fn tail_offset(&self) -> u64 {
        self.stream_tail_offset.load(Ordering::Acquire)
    }
    
    pub fn stream_id(&self) -> StreamId {
        self.stream_base.stream_id()
    }
}

#[derive(Serialize, Deserialize)]
struct AppendOnlyMetadata {
    /// Base stream metadata (MUST be first field for bincode compatibility)
    base: super::new_blob_dev::StreamMetadata,
    
    /// Stream-specific: tail offset
    stream_tail_offset: u64,
}
