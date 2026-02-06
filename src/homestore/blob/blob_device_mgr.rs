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

use crate::meta::meta_client::MetaClient;
use crate::meta::meta_blk::MetaBlk;
use crate::device::VDevParameters;

use super::new_blob_dev::BlobDevice;

/// Runtime information for a single stream (used during recovery)
pub struct StreamInfo {
    /// Stream ID
    pub stream_id: u64,
    
    /// Stream base metablk (if exists)
    pub stream_base_mblk: Option<MetaBlk>,
    
    /// Per-chunk metablks (chunk_id -> MetaBlk)
    pub chunk_metablks: HashMap<u32, MetaBlk>,
}

/// BlobDevice Manager - singleton managing multiple BlobDevice instances
/// 
/// Each BlobDevice has its own VirtualDev (1:1 relationship).
/// Metadata naming convention:
/// - Stream base metablks: `<type>_bdev_stream_base_<stream_id>`
/// - Chunk metablks: `<type>_bdev_stream_<stream_id>_chunk_<chunk_id>`
pub struct BlobDeviceManager {
    /// All blob devices by type (device_type -> BlobDevice)
    devices: RwLock<HashMap<String, Arc<BlobDevice>>>,
    
    /// Meta client for managing metadata
    meta_client: Arc<MetaClient>,
}

impl BlobDeviceManager {
    /// Create a new BlobDeviceManager
    pub async fn create() -> io::Result<Self> {
        // Register as "BlobDeviceManager" with MetaBlkManager
        let meta_mgr = crate::common::managers::metablk_mgr();
        let meta_client = meta_mgr.register_client("BlobDeviceManager".to_string()).await?;
        
        Ok(Self {
            devices: RwLock::new(HashMap::new()),
            meta_client: Arc::new(meta_client),
        })
    }
    
    /// Load existing BlobDeviceManager and all devices during recovery
    /// 
    /// Scans all metablks and groups them by device_type:
    /// - Pattern: `<type>_bdev_stream_base_<stream_id>` for stream base metablks
    /// - Pattern: `<type>_bdev_stream_<stream_id>_chunk_<chunk_id>` for chunk metablks
    /// 
    /// For each device_type found, loads a BlobDevice with its metablks
    pub async fn load() -> io::Result<Self> {
        /// Runtime information for a single BlobDevice during recovery
        struct BDevInfo {
            device_type: String,
            streams: HashMap<u64, StreamInfo>,
        }
        
        // Register as "BlobDeviceManager" with MetaBlkManager
        let meta_mgr = crate::common::managers::metablk_mgr();
        let meta_client = meta_mgr.register_client("BlobDeviceManager".to_string()).await?;
        
        // Scan all metablks and group by device_type
        let mut bdev_info_map: HashMap<String, BDevInfo> = HashMap::new();
        
        use futures::StreamExt;
        let mut recovered_mblks = meta_client.recovered_blocks();
        
        while let Some(result) = recovered_mblks.next().await {
            let (meta_blk, _data) = result?;
            let header = meta_blk.header();
            let name = header.get_name();
            
            // Parse: <type>_bdev_stream_base_<stream_id>
            if let Some(rest) = name.strip_suffix("_bdev_stream_base_") {
                if let Some((device_type, stream_id_str)) = rest.rsplit_once('_') {
                    if let Ok(stream_id) = stream_id_str.parse::<u64>() {
                        let bdev_info = bdev_info_map
                            .entry(device_type.to_string())
                            .or_insert_with(|| BDevInfo {
                                device_type: device_type.to_string(),
                                streams: HashMap::new(),
                            });
                        
                        let stream_info = bdev_info.streams
                            .entry(stream_id)
                            .or_insert_with(|| StreamInfo {
                                stream_id,
                                stream_base_mblk: None,
                                chunk_metablks: HashMap::new(),
                            });
                        
                        stream_info.stream_base_mblk = Some(meta_blk);
                        continue;
                    }
                }
            }
            
            // Parse: <type>_bdev_stream_<stream_id>_chunk_<chunk_id>
            if name.contains("_bdev_stream_") && name.contains("_chunk_") {
                // Split by "_chunk_" first to get chunk_id
                if let Some((prefix, chunk_id_str)) = name.rsplit_once("_chunk_") {
                    if let Ok(chunk_id) = chunk_id_str.parse::<u32>() {
                        // Now parse prefix: <type>_bdev_stream_<stream_id>
                        if let Some(rest) = prefix.strip_suffix("_bdev_stream_") {
                            if let Some((device_type, stream_id_str)) = rest.rsplit_once('_') {
                                if let Ok(stream_id) = stream_id_str.parse::<u64>() {
                                    let bdev_info = bdev_info_map
                                        .entry(device_type.to_string())
                                        .or_insert_with(|| BDevInfo {
                                            device_type: device_type.to_string(),
                                            streams: HashMap::new(),
                                        });
                                    
                                    let stream_info = bdev_info.streams
                                        .entry(stream_id)
                                        .or_insert_with(|| StreamInfo {
                                            stream_id,
                                            stream_base_mblk: None,
                                            chunk_metablks: HashMap::new(),
                                        });
                                    
                                    stream_info.chunk_metablks.insert(chunk_id, meta_blk);
                                    continue;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Load each BlobDevice with its metablks
        let mut devices = HashMap::new();
        let device_mgr = crate::common::managers::device_mgr();
        
        for (device_type, bdev_info) in bdev_info_map {
            println!("Loading BlobDevice '{}' with {} streams", device_type, bdev_info.streams.len());
            
            // Get the VirtualDev for this device_type
            let vdev = device_mgr.get_vdev(&device_type)
                .ok_or_else(|| io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("VirtualDev '{}' not found", device_type)
                ))?;
            
            // Load BlobDevice with its streams
            let device = BlobDevice::load(
                device_type.clone(),
                vdev,
                Arc::clone(&meta_client),
                bdev_info.streams,
            ).await?;
            
            devices.insert(device_type, Arc::new(device));
        }
        
        Ok(Self {
            devices: RwLock::new(devices),
            meta_client: Arc::new(meta_client),
        })
    }
    
    /// Create a new BlobDevice
    /// 
    /// Creates a VirtualDev for this device (1:1 relationship) using the provided parameters
    /// 
    /// # Arguments
    /// * `device_type` - Unique device type identifier (e.g., "Index", "Raw", "Journal")
    /// * `vdev_params` - Parameters for creating the VirtualDev
    pub async fn create_blob_device(
        &self,
        device_type: String,
        vdev_params: VDevParameters,
    ) -> io::Result<Arc<BlobDevice>> {
        // Get singleton DeviceManager
        let device_mgr = crate::common::managers::device_mgr();
        
        // Create VirtualDev for this BlobDevice (1:1 relationship)
        let vdev = device_mgr.create_vdev(vdev_params).await?;
        
        // Create the BlobDevice (doesn't need device_mgr - uses singleton)
        let device = BlobDevice::create(
            device_type.clone(),
            vdev,
            self.meta_client.clone(),
        ).await?;
        
        // Add to devices map
        self.devices.write().insert(device_type, device.clone());
        
        Ok(device)
    }
    
    /// Get a BlobDevice by type
    pub fn get_blob_device(&self, device_type: &str) -> Option<Arc<BlobDevice>> {
        self.devices.read().get(device_type).cloned()
    }
}
