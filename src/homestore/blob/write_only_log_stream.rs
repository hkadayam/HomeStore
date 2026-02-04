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
use crate::common::managers::DeviceManager;
use crate::blob::types::BlkId;
use iomgr::IOBuffer;

// Import common stream infrastructure
use super::stream_common::{
    LogStream, ChunkPool, StreamChunkManager,
    StreamId, FlushSessionId, MAX_FLUSH_SESSIONS,
};

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

/// Write-Only Log Stream
pub struct WriteOnlyLogStream {
    chunk_mgr: StreamChunkManager,
    
    /// Logical stream offset (tail position)
    stream_tail_offset: AtomicU64,
    
    /// Per-session buffers
    session_buffers: [SessionBuffer; MAX_FLUSH_SESSIONS],
    
    /// Metadata block
    stream_mblk: MetaBlk,
    
    /// Meta client
    meta_client: Arc<MetaClient>,
}

impl WriteOnlyLogStream {
    /// Create new write-only log stream
    pub async fn new(
        stream_id: StreamId,
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
        device_mgr: Arc<DeviceManager>,
        chunk_pool: Arc<ChunkPool>,
        chunk_size: u64,
        stream_mblk: MetaBlk,
    ) -> io::Result<Self> {
        let block_size = vdev.block_size();
        let chunk_mgr = StreamChunkManager::new(
            stream_id,
            vdev,
            device_mgr,
            chunk_pool,
            chunk_size,
            Vec::new(),
        );
        
        Ok(Self {
            chunk_mgr,
            stream_tail_offset: AtomicU64::new(0),
            session_buffers: [
                SessionBuffer::new(block_size),
                SessionBuffer::new(block_size),
            ],
            stream_mblk,
            meta_client,
        })
    }
    
    /// Load existing write-only log stream
    pub async fn load(
        stream_id: StreamId,
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
        device_mgr: Arc<DeviceManager>,
        chunk_pool: Arc<ChunkPool>,
        chunk_ids: Vec<u32>,
        chunk_size: u64,
        stream_mblk: MetaBlk,
    ) -> io::Result<Self> {
        let block_size = vdev.block_size();
        
        // Load metadata
        let metadata: WriteOnlyLogMetadata = bincode::deserialize(
            &stream_mblk.read_data(&meta_client.meta_vdev).await?
        ).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        let chunk_mgr = StreamChunkManager::new(
            stream_id,
            vdev,
            device_mgr,
            chunk_pool,
            chunk_size,
            chunk_ids,
        );
        
        let stream = Self {
            chunk_mgr,
            stream_tail_offset: AtomicU64::new(metadata.stream_tail_offset),
            session_buffers: [
                SessionBuffer::new(block_size),
                SessionBuffer::new(block_size),
            ],
            stream_mblk,
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
    
    /// Delete entire stream (returns all chunks to pool)
    pub async fn delete(self) -> io::Result<()> {
        // Return all chunks to pool for reuse
        self.chunk_mgr.return_all_chunks_to_pool();
        
        // Delete metadata (optional - could mark as deleted)
        // self.stream_mblk.delete(...)?;
        
        Ok(())
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
        let chunks = self.chunk_mgr.chunks();
        let chunk_id = chunks[chunk_idx];
        let block_size = self.chunk_mgr.block_size();
        
        let blk_num = (offset_in_chunk / block_size as u64) as u32;
        let nblks = (Self::round_up(io_buffer.len(), block_size as usize) / block_size as usize) as u16;
        let blk_id = BlkId::new(blk_num, nblks, chunk_id);
        
        self.chunk_mgr.vdev().write(&io_buffer, &blk_id).await?;
        Ok(())
    }
    
    /// Persist metadata
    async fn persist_metadata(&self, tail_offset: u64) -> io::Result<()> {
        let metadata = WriteOnlyLogMetadata {
            stream_tail_offset: tail_offset,
            chunk_size: self.chunk_mgr.chunk_size(),
            num_chunks: self.chunk_mgr.chunks().len(),
        };
        
        let serialized = bincode::serialize(&metadata)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        
        self.stream_mblk.write_data(&serialized, &self.meta_client.meta_vdev).await?;
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
        let chunk_size = self.chunk_mgr.chunk_size();
        let chunk_idx = (block_aligned_offset / chunk_size) as usize;
        
        if chunk_idx >= self.chunk_mgr.chunks().len() {
            return Ok(Vec::new());
        }
        
        let chunks = self.chunk_mgr.chunks();
        let chunk_id = chunks[chunk_idx];
        let chunk_start_offset = chunk_idx as u64 * chunk_size;
        let offset_in_chunk = block_aligned_offset - chunk_start_offset;
        
        let blk_num = (offset_in_chunk / block_size as u64) as u32;
        let blk_id = BlkId::new(blk_num, 1, chunk_id);
        
        // Read the block
        let block_data = self.chunk_mgr.vdev().read(&blk_id).await?;
        
        // Extract residual portion
        Ok(block_data[..residual_size].to_vec())
    }
}

impl LogStream for WriteOnlyLogStream {
    /// Append data - takes ownership, caller provides session_id
    async fn append(&self, session_id: FlushSessionId, data: Vec<u8>) -> io::Result<()> {
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
    async fn flush(&self, session_id: FlushSessionId) -> io::Result<()> {
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
        let chunk_size = self.chunk_mgr.chunk_size();
        
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
            self.chunk_mgr.ensure_chunk_exists(chunk_idx).await?;
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
    
    fn tail_offset(&self) -> u64 {
        self.stream_tail_offset.load(Ordering::Acquire)
    }
    
    fn stream_id(&self) -> StreamId {
        self.chunk_mgr.stream_id()
    }
}

#[derive(Serialize, Deserialize)]
struct WriteOnlyLogMetadata {
    stream_tail_offset: u64,
    chunk_size: u64,
    num_chunks: usize,
}
