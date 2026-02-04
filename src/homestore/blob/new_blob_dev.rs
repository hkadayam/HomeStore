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
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};

use crate::device::virtual_dev::VirtualDev;
use crate::meta::meta_client::MetaClient;

use super::stream_base::{StreamBase, StreamId};
use super::blk_stream::BlkStream;
use super::append_only_stream::AppendOnlyStream;
use super::fixed_blk_stream::FixedBlkStream;
use super::blob_device_mgr::StreamInfo;

/// Stream type identifier
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum StreamType {
    /// Block-level stream (random access, no WriteUnits)
    BlkStream,
    
    /// Fixed block stream (append with WriteUnits, checkpoint-based flush)
    FixedBlkStream,
    
    /// Append-only stream (sequential writes, no reads)
    AppendOnlyStream,
    
    /// Indexed log stream (future)
    IndexedLogStream,
}

/// Stream metadata stored in stream base metablk
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StreamMetadata {
    /// Stream ID
    pub stream_id: u64,
    
    /// Stream type
    pub stream_type: StreamType,
    
    /// List of chunk IDs in order
    pub chunk_list: Vec<u32>,
    
    /// Chunk size
    pub chunk_size: u64,
    
    /// Max chunks in pool
    pub max_chunks_in_pool: usize,
}

/// Blob Device - manages multiple stream types
/// 
/// Each stream has its own ChunkPool (not shared) since streams are independent
pub struct BlobDevice {
    /// Device type (e.g., "Index", "Raw", "Journal")
    device_type: String,
    
    /// Virtual device (1:1 relationship)
    vdev: Arc<VirtualDev>,
    
    /// Meta client
    meta_client: Arc<MetaClient>,
    
    /// BlkStreams (stream_id -> BlkStream)
    blk_streams: RwLock<HashMap<StreamId, Arc<BlkStream>>>,
    
    /// FixedBlkStreams (stream_id -> FixedBlkStream)
    fixed_blk_streams: RwLock<HashMap<StreamId, Arc<FixedBlkStream>>>,
    
    /// AppendOnlyStreams (stream_id -> AppendOnlyStream)
    append_streams: RwLock<HashMap<StreamId, Arc<AppendOnlyStream>>>,
    
    /// Next stream ID
    next_stream_id: std::sync::atomic::AtomicU64,
}

impl BlobDevice {
    /// Create a new BlobDevice
    pub async fn create(
        device_type: String,
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
    ) -> io::Result<Self> {
        Ok(Self {
            device_type,
            vdev,
            meta_client,
            blk_streams: RwLock::new(HashMap::new()),
            fixed_blk_streams: RwLock::new(HashMap::new()),
            append_streams: RwLock::new(HashMap::new()),
            next_stream_id: std::sync::atomic::AtomicU64::new(1),
        })
    }
    
    /// Load existing BlobDevice
    /// 
    /// # Arguments
    /// * `device_type` - Device type identifier
    /// * `vdev` - Virtual device for this blob device
    /// * `meta_client` - Meta client for metadata access
    /// * `streams_info` - Map of stream_id -> StreamInfo (from BlobDeviceManager::load())
    pub async fn load(
        device_type: String,
        vdev: Arc<VirtualDev>,
        meta_client: Arc<MetaClient>,
        streams_info: HashMap<u64, StreamInfo>,
    ) -> io::Result<Self> {
        let mut blk_streams = HashMap::new();
        let mut fixed_blk_streams = HashMap::new();
        let mut append_streams = HashMap::new();
        let mut max_stream_id = 0u64;
        
        // Load each stream based on its type (determined from metadata)
        for (stream_id, stream_info) in streams_info {
            max_stream_id = max_stream_id.max(stream_id);
            
            // Read StreamMetadata from stream base metablk
            let stream_base_mblk = stream_info.stream_base_mblk
                .ok_or_else(|| io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("Stream {} missing stream base metablk", stream_id)
                ))?;
            
            let metadata_bytes = stream_base_mblk.read_data(&meta_client.meta_vdev).await?;
            let stream_metadata: StreamMetadata = bincode::deserialize(&metadata_bytes)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
            
            println!("Loading {} stream {} with {} chunks", 
                match stream_metadata.stream_type {
                    StreamType::BlkStream => "BlkStream",
                    StreamType::FixedBlkStream => "FixedBlkStream",
                    StreamType::AppendOnlyStream => "AppendOnlyStream",
                    StreamType::IndexedLogStream => "IndexedLogStream",
                },
                stream_id, 
                stream_metadata.chunk_list.len()
            );
            
            // Load stream based on type
            match stream_metadata.stream_type {
                StreamType::BlkStream => {
                    // Build ordered chunk_metablks Vec based on chunk_list order
                    let chunk_metablks: Vec<MetaBlk> = stream_metadata.chunk_list
                        .iter()
                        .filter_map(|chunk_id| stream_info.chunk_metablks.get(chunk_id).cloned())
                        .collect();
                    
                    let stream = BlkStream::load(
                        vdev.clone(),
                        stream_metadata,
                        stream_base_mblk,
                        chunk_metablks,
                        meta_client.clone(),
                    ).await?;
                    blk_streams.insert(stream_id, Arc::new(stream));
                }
                
                StreamType::FixedBlkStream => {
                    // Build ordered chunk_metablks Vec based on chunk_list order
                    let chunk_metablks: Vec<MetaBlk> = stream_metadata.chunk_list
                        .iter()
                        .filter_map(|chunk_id| stream_info.chunk_metablks.get(chunk_id).cloned())
                        .collect();
                    
                    let stream = FixedBlkStream::load(
                        vdev.clone(),
                        stream_metadata,
                        stream_base_mblk,
                        chunk_metablks,
                        meta_client.clone(),
                    ).await?;
                    fixed_blk_streams.insert(stream_id, Arc::new(stream));
                }
                
                StreamType::AppendOnlyStream => {
                    let stream = AppendOnlyStream::load(
                        vdev.clone(),
                        meta_client.clone(),
                        stream_metadata,
                        stream_base_mblk,
                    ).await?;
                    
                    append_streams.insert(stream_id, Arc::new(stream));
                }
                
                StreamType::IndexedLogStream => {
                    return Err(io::Error::new(io::ErrorKind::Unsupported, "IndexedLogStream not yet implemented"));
                }
            }
        }
        
        let blob_device = Self {
            device_type,
            vdev,
            meta_client,
            blk_streams: RwLock::new(blk_streams),
            fixed_blk_streams: RwLock::new(fixed_blk_streams),
            append_streams: RwLock::new(append_streams),
            next_stream_id: std::sync::atomic::AtomicU64::new(max_stream_id + 1),
        };
        
        // Reconcile chunks: capture any chunks that exist in VDev but missing from metadata
        // This handles cases where chunk expansion completed but metadata write didn't
        blob_device.reconcile_stream_chunks().await?;
        
        Ok(blob_device)
    }
    
    /// Reconcile stream chunks after load
    /// 
    /// Compares chunks in VDev with chunks in stream metadata.
    /// Any chunks present in VDev but missing from metadata are orphaned
    /// (flush didn't complete before crash) and should be deleted.
    async fn reconcile_stream_chunks(&self) -> io::Result<()> {
        use std::collections::HashSet;
        
        // Get all stream->chunks mapping from VDev
        let vdev_stream_chunks = self.vdev.get_all_stream_chunks();
        
        // Reconcile all streams in a single pass
        for (stream_id, vdev_chunks) in &vdev_stream_chunks {
            // Try BlkStream first
            if let Some(stream) = self.blk_streams.read().get(stream_id) {
                let metadata_chunks = stream.get_chunk_list();
                let metadata_set: HashSet<u32> = metadata_chunks.into_iter().collect();
                
                // Find chunks in VDev but NOT in metadata (set difference)
                let orphaned_chunks: Vec<u32> = vdev_chunks.iter()
                    .filter(|chunk_id| !metadata_set.contains(chunk_id))
                    .copied()
                    .collect();
                
                if !orphaned_chunks.is_empty() {
                    println!("BlkStream {}: Deleting {} orphaned chunks (VDev has {}, metadata has {})",
                        stream_id, orphaned_chunks.len(), vdev_chunks.len(), metadata_set.len());
                    
                    for chunk_id in orphaned_chunks {
                        self.vdev.remove_chunk(chunk_id).await?;
                    }
                }
                continue;
            }
            
            // Try FixedBlkStream
            if let Some(stream) = self.fixed_blk_streams.read().get(stream_id) {
                let metadata_chunks = stream.get_chunk_list();
                let metadata_set: HashSet<u32> = metadata_chunks.into_iter().collect();
                
                // Find chunks in VDev but NOT in metadata (set difference)
                let orphaned_chunks: Vec<u32> = vdev_chunks.iter()
                    .filter(|chunk_id| !metadata_set.contains(chunk_id))
                    .copied()
                    .collect();
                
                if !orphaned_chunks.is_empty() {
                    println!("FixedBlkStream {}: Deleting {} orphaned chunks (VDev has {}, metadata has {})",
                        stream_id, orphaned_chunks.len(), vdev_chunks.len(), metadata_set.len());
                    
                    for chunk_id in orphaned_chunks {
                        self.vdev.remove_chunk(chunk_id)?;
                    }
                }
                continue;
            }
            
            // Try AppendOnlyStream
            if let Some(stream) = self.append_streams.read().get(stream_id) {
                let metadata_chunks = stream.get_chunk_list();
                let metadata_set: HashSet<u32> = metadata_chunks.into_iter().collect();
                
                // Find chunks in VDev but NOT in metadata (set difference)
                let orphaned_chunks: Vec<u32> = vdev_chunks.iter()
                    .filter(|chunk_id| !metadata_set.contains(chunk_id))
                    .copied()
                    .collect();
                
                if !orphaned_chunks.is_empty() {
                    println!("AppendOnlyStream {}: Deleting {} orphaned chunks (VDev has {}, metadata has {})",
                        stream_id, orphaned_chunks.len(), vdev_chunks.len(), metadata_set.len());
                    
                    for chunk_id in orphaned_chunks {
                        self.vdev.remove_chunk(chunk_id)?;
                    }
                }
            }
        }
        
        Ok(())
    }
    
    /// Create a new BlkStream
    pub async fn create_blk_stream(&self, chunk_size: u64, max_chunks_in_pool: usize) -> io::Result<StreamId> {
        let stream_id = self.next_stream_id.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        
        // Create stream metadata (but don't write yet!)
        let stream_metadata = StreamMetadata {
            stream_id,
            stream_type: StreamType::BlkStream,
            chunk_list: Vec::new(),  // Stream might populate this
            chunk_size,
            max_chunks_in_pool,
        };
        
        // Create empty metablk
        let meta_name = format!("{}_bdev_stream_base_{}", self.device_type, stream_id);
        let stream_mblk = self.meta_client.create_meta_blk(&meta_name, None).await?;
        
        // Pass metadata + metablk to stream - it will write after initialization
        let stream = BlkStream::create(
            self.vdev.clone(),
            stream_metadata,
            stream_mblk,
            self.meta_client.clone(),
        ).await?;
        
        self.blk_streams.write().insert(stream_id, Arc::new(stream));
        
        Ok(stream_id)
    }
    
    /// Create a new FixedBlkStream
    pub async fn create_fixed_blk_stream(
        &self, 
        chunk_size: u64, 
        max_chunks_in_pool: usize,
        config: Option<super::fixed_blk_stream::FixedBlkStreamConfig>
    ) -> io::Result<StreamId> {
        let stream_id = self.next_stream_id.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        
        // Create stream metadata (but don't write yet!)
        let stream_metadata = StreamMetadata {
            stream_id,
            stream_type: StreamType::FixedBlkStream,
            chunk_list: Vec::new(),
            chunk_size,
            max_chunks_in_pool,
        };
        
        // Create empty metablk
        let meta_name = format!("{}_bdev_stream_base_{}", self.device_type, stream_id);
        let stream_mblk = self.meta_client.create_meta_blk(&meta_name, None).await?;
        
        // Pass metadata + metablk to stream - it will write after initialization
        let stream = FixedBlkStream::create(
            self.vdev.clone(),
            stream_metadata,
            stream_mblk,
            self.meta_client.clone(),
            config,
        ).await?;
        
        self.fixed_blk_streams.write().insert(stream_id, Arc::new(stream));
        
        Ok(stream_id)
    }
    
    /// Create a new AppendOnlyStream
    pub async fn create_append_only_stream(&self, chunk_size: u64, max_chunks_in_pool: usize) -> io::Result<StreamId> {
        let stream_id = self.next_stream_id.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        
        // Create stream metadata (but don't write yet!)
        let stream_metadata = StreamMetadata {
            stream_id,
            stream_type: StreamType::AppendOnlyStream,
            chunk_list: Vec::new(),
            chunk_size,
            max_chunks_in_pool,
        };
        
        // Create empty metablk
        let meta_name = format!("{}_bdev_stream_base_{}", self.device_type, stream_id);
        let stream_mblk = self.meta_client.create_meta_blk(&meta_name, None).await?;
        
        // Pass metadata + metablk to stream - it will write after initialization
        let stream = AppendOnlyStream::new(
            self.vdev.clone(),
            self.meta_client.clone(),
            stream_metadata,
            stream_mblk,
        ).await?;
        
        self.append_streams.write().insert(stream_id, Arc::new(stream));
        
        Ok(stream_id)
    }

    pub async fn destroy_stream(&self, stream_id: StreamId) -> io::Result<()> {
        if let Some(stream) = self.get_blk_stream(stream_id) {
            stream.destroy().await
        } else if let Some(stream) = self.get_append_stream(stream_id) {
            stream.destroy().await
        } else {
            return Err(io::Error::new(io::ErrorKind::NotFound, format!("Stream {} not found", stream_id)));
        }
        Ok(())
    }
    
    /// Get a BlkStream by ID
    pub fn get_blk_stream(&self, stream_id: StreamId) -> Option<Arc<BlkStream>> {
        self.blk_streams.read().get(&stream_id).cloned()
    }
    
    /// Get an AppendOnlyStream by ID
    pub fn get_append_stream(&self, stream_id: StreamId) -> Option<Arc<AppendOnlyStream>> {
        self.append_streams.read().get(&stream_id).cloned()
    }
}
