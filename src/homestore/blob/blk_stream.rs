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
use serde::{Deserialize, Serialize};

use crate::device::virtual_dev::VirtualDev;
use crate::meta::meta_blk::MetaBlk;
use crate::meta::meta_client::MetaClient;
use crate::blob::types::BlkId;
use iomgr::IOBuffer;

use super::stream_base::StreamBase;
use super::new_blob_dev::StreamMetadata;

/// BlkStream - supports read/write/invalidate of individual blocks
pub struct BlkStream {
    /// Base stream infrastructure
    stream_base: StreamBase,
    
    /// Meta client for metadata operations
    meta_client: Arc<MetaClient>,
    
    // TODO: Add buffering state for cached mode
    // TODO: Add per-segment write units
}

impl BlkStream {
    /// Create a new BlkStream
    pub async fn create(
        vdev: Arc<VirtualDev>,
        metadata: StreamMetadata,
        mut stream_mblk: MetaBlk,
        meta_client: Arc<MetaClient>,
    ) -> io::Result<Self> {
        // Write metadata ONCE after initialization
        let metadata_bytes = bincode::serialize(&metadata)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        stream_mblk.write_data(&metadata_bytes, &meta_client.meta_vdev).await?;
        
        // Create StreamBase (takes ownership of stream_mblk)
        let stream_base = StreamBase::new(vdev, metadata, stream_mblk);
        
        Ok(Self {
            stream_base,
            meta_client,
        })
    }
    
    /// Load an existing BlkStream
    pub async fn load(
        vdev: Arc<VirtualDev>,
        metadata: StreamMetadata,
        stream_mblk: MetaBlk,
        chunk_metablks: Vec<MetaBlk>,
        meta_client: Arc<MetaClient>,
    ) -> io::Result<Self> {
        // Step 1: Load StreamBase with existing chunks and their metadata
        let stream_base = StreamBase::load(vdev.clone(), metadata, stream_mblk, chunk_metablks.clone())?;
        
        // Step 2: Read allocator state from chunk metablks and initialize block allocators
        // Build HashMap<chunk_id, IOBuffer> with raw allocator data and pass it on to vdev.
        let mut chunk_buffers = std::collections::HashMap::new();
        let stream_chunks = stream_base.chunks();
        
        for (idx, stream_chunk) in stream_chunks.iter().enumerate() {
            if let Some(ref chunk_meta_blk) = stream_chunk.chunk_meta_blk {
                // Read the raw allocator data as IOBuffer (no deserialization, no copy)
                let allocator_data = chunk_meta_blk.read_data(&meta_client.meta_vdev).await?;
                chunk_buffers.insert(stream_chunk.chunk_id(), allocator_data);
            }
        }
        
        // Step 3: Let VirtualDev initialize block allocators for all chunks
        vdev.load_blk_allocator(Some(chunk_buffers))?;
        
        Ok(Self {
            stream_base,
            meta_client,
        })
    }
    
    /// Get current chunk list
    pub fn get_chunk_list(&self) -> Vec<u32> {
        self.stream_base.get_chunk_ids()
    }
    
    /// Read from a block
    pub async fn read(&self, blk_id: &BlkId, buf: &mut [u8]) -> io::Result<usize> {
        self.stream_base.vdev().read(blk_id, buf).await
    }
    
    /// Write to a block (direct or buffered depending on mode)
    pub async fn write(&self, blk_id: &BlkId, data: &IOBuffer) -> io::Result<()> {
        // TODO: Implement buffered vs direct write logic
        self.stream_base.vdev().write(data, blk_id).await
    }
    
    /// Invalidate a block
    pub fn invalidate(&self, blk_id: &BlkId) -> io::Result<()> {
        // TODO: Implement invalidation (remove from cache if cached, free block)
        self.stream_base.vdev().free_blk(&[blk_id.clone()])
    }
    
    /// Append data (creates write unit in buffered mode)
    pub async fn append(&self, _segment_id: u16, _data: IOBuffer) -> io::Result<BlkId> {
        // TODO: Implement append logic
        todo!("BlkStream::append implementation")
    }
    
    /// Destroy stream - delegates to StreamBase for all cleanup
    pub async fn destroy(self) -> io::Result<()> {
        // Delegate everything to StreamBase
        // StreamBase already owns stream_mblk and chunk_metablks
        self.stream_base.destroy(&self.meta_client).await
    }
}
