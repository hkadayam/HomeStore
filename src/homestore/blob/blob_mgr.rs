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

use std::collections::HashMap;
use std::io;
use std::sync::Arc;
use parking_lot::RwLock;

use crate::common::managers;
use crate::meta::MetaClient;
use super::blob_dev::BlobDevice;

// Re-export device types for convenience
pub use crate::device::{VDevParameters, HSDevType, BlkAllocatorType, ChunkSelectorType, MultiPDevOpts};

// Re-export blob_dev types for convenience
pub use super::blob_dev::WriteMode;

/// Blob Manager
/// 
/// Central manager for all blob devices. Handles:
/// - Creating and managing multiple blob devices
/// - Device lifecycle management
/// - Device map maintenance
/// - Metadata management for chunks across all blob devices
/// 
/// The BlobManager maintains a single MetaClient that is used to create
/// metadata blocks for each chunk. Each chunk's metadata will be stored
/// with its chunk name in the MetaBlkHeader.
pub struct BlobManager {
    /// Map of blob devices by name
    blob_devices: RwLock<HashMap<String, Arc<BlobDevice>>>,
    
    /// Metadata client for managing chunk metadata
    meta_client: MetaClient,
}

impl BlobManager {
    /// Create a new BlobManager and register it to the global Managers singleton
    /// 
    /// This is the first-time boot path. It:
    /// 1. Registers a MetaClient with MetaBlkManager for chunk metadata
    /// 2. Creates an empty manager that will track blob devices
    /// 
    /// # Returns
    /// Static reference to the registered BlobManager
    pub async fn create() -> io::Result<&'static Self> {
        let meta_mgr = managers::metablk_mgr();
        
        // Register a metadata client for blob manager (for chunk metadata)
        let meta_client = meta_mgr.register_client("blob_mgr".to_string()).await?;
        
        // Create the manager instance
        let mgr = Self {
            blob_devices: RwLock::new(HashMap::new()),
            meta_client,
        };
        
        // Register to global managers singleton
        managers::Managers::instance().init_blob_mgr(mgr);
        
        Ok(managers::blob_mgr())
    }
    
    /// Create a new blob device
    /// 
    /// # Arguments
    /// * `dev_name` - Name for this blob device
    /// * `params` - Virtual device parameters
    /// 
    /// # Returns
    /// Reference to the created blob device
    /// 
    /// # Examples
    /// 
    /// ```ignore
    /// let params = VDevParameters {
    ///     vdev_name: "my_blob".to_string(),
    ///     vdev_size: 1024 * 1024 * 1024, // 1GB
    ///     num_chunks: 10,
    ///     blk_size: 4096,
    ///     chunk_size: 100 * 1024 * 1024, // 100MB
    ///     dev_type: HSDevType::SSD,
    ///     alloc_type: BlkAllocatorType::Varsize,
    ///     chunk_sel_type: ChunkSelectorType::RoundRobin,
    ///     multi_pdev_opts: MultiPDevOpts::AllPdevStriped,
    ///     context_data: vec![],
    /// };
    /// 
    /// let blob_dev = blob_mgr.create_blob_dev("myblob", params).await?;
    /// ```
    pub async fn create_blob_dev(
        &self,
        dev_name: &str,
        params: VDevParameters,
    ) -> io::Result<Arc<BlobDevice>> {
        // Check if device already exists
        {
            let devices = self.blob_devices.read();
            if devices.contains_key(dev_name) {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    format!("Blob device '{}' already exists", dev_name),
                ));
            }
        }
        
        // Create the virtual device first via DeviceManager
        let dev_mgr = managers::device_mgr();
        let vdev = dev_mgr.create_vdev(params).await?;
        
        // Create the blob device with vdev and metadata initialization
        let blob_dev = Arc::new(
            BlobDevice::create(dev_name, vdev, &self.meta_client).await?
        );
        
        // Add to our map
        {
            let mut devices = self.blob_devices.write();
            devices.insert(dev_name.to_string(), Arc::clone(&blob_dev));
        }
        
        Ok(blob_dev)
    }
    
    /// Get a blob device by name
    /// 
    /// # Arguments
    /// * `dev_name` - Name of the blob device to retrieve
    /// 
    /// # Returns
    /// Option containing the blob device if found
    pub fn get_blob_dev(&self, dev_name: &str) -> Option<Arc<BlobDevice>> {
        let devices = self.blob_devices.read();
        devices.get(dev_name).cloned()
    }
    
    /// List all blob device names
    pub fn list_devices(&self) -> Vec<String> {
        let devices = self.blob_devices.read();
        devices.keys().cloned().collect()
    }
    
    /// Get the number of blob devices
    pub fn num_devices(&self) -> usize {
        let devices = self.blob_devices.read();
        devices.len()
    }
    
    /// Remove a blob device
    /// 
    /// # Arguments
    /// * `dev_name` - Name of the device to remove
    /// 
    /// # Returns
    /// The removed blob device, or None if not found
    pub fn remove_blob_dev(&self, dev_name: &str) -> Option<Arc<BlobDevice>> {
        let mut devices = self.blob_devices.write();
        devices.remove(dev_name)
    }
      
    /// Start all blob devices
    pub async fn start(&self) -> io::Result<()> {
        // Collect devices to avoid holding lock across await
        let devices_list: Vec<(String, Arc<BlobDevice>)> = {
            let devices = self.blob_devices.read();
            devices.iter().map(|(k, v)| (k.clone(), Arc::clone(v))).collect()
        };
        
        for (name, dev) in devices_list {
            dev.start().await.map_err(|e| {
                io::Error::new(
                    e.kind(),
                    format!("Failed to start blob device '{}': {}", name, e),
                )
            })?;
        }
        Ok(())
    }
    
    /// Stop all blob devices
    pub async fn stop(&self) -> io::Result<()> {
        // Collect devices to avoid holding lock across await
        let devices_list: Vec<(String, Arc<BlobDevice>)> = {
            let devices = self.blob_devices.read();
            devices.iter().map(|(k, v)| (k.clone(), Arc::clone(v))).collect()
        };
        
        for (name, dev) in devices_list {
            dev.stop().await.map_err(|e| {
                io::Error::new(
                    e.kind(),
                    format!("Failed to stop blob device '{}': {}", name, e),
                )
            })?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::{BlkAllocatorType, ChunkSelectorType, MultiPDevOpts};
    use crate::common::managers::Managers;

    #[iomgr::iomanager_test]
    async fn test_blob_manager_creation() {
        // Setup device manager first
        let dev_infos = vec![
            crate::device::DevInfo {
                dev_name: "test_blob_dev1".to_string(),
                dev_size: 1024 * 1024 * 100, // 100MB
                dev_type: crate::device::HSDevType::Data,
            },
        ];
        
        crate::device::DeviceManager::new(dev_infos).expect("DeviceManager creation failed");
        
        // Create MetaBlkManager
        crate::meta::MetaBlkManager::create(1024 * 1024 * 10).await.expect("MetaBlkManager creation failed");
        
        // Create BlobManager
        let blob_mgr = BlobManager::create().await.expect("BlobManager creation failed");
        
        assert_eq!(blob_mgr.num_devices(), 0);
        assert!(blob_mgr.list_devices().is_empty());
        
        // Cleanup
        Managers::restart().await;
    }

    #[iomgr::iomanager_test]
    async fn test_create_blob_dev() {
        // Setup
        let dev_infos = vec![
            crate::device::DevInfo {
                dev_name: "test_blob_dev2".to_string(),
                dev_size: 1024 * 1024 * 100, // 100MB
                dev_type: crate::device::HSDevType::Data,
            },
        ];
        
        crate::device::DeviceManager::new(dev_infos).expect("DeviceManager creation failed");
        crate::meta::MetaBlkManager::create(1024 * 1024 * 10).await.expect("MetaBlkManager creation failed");
        let blob_mgr = BlobManager::create().await.expect("BlobManager creation failed");
        
        // Create a blob device
        let params = VDevParameters {
            vdev_name: "test_vdev".to_string(),
            vdev_size: 1024 * 1024 * 50, // 50MB
            num_chunks: 5,
            chunk_size: 10 * 1024 * 1024, // 10MB
            blk_size: 4096,
            dev_type: crate::device::HSDevType::Data,
            num_mirrors: 1,
            alloc_type: BlkAllocatorType::Varsize,
            chunk_sel_type: ChunkSelectorType::RoundRobin,
            multi_pdev_opts: MultiPDevOpts::AllPDevStriped,
        };
        
        let blob_dev = blob_mgr.create_blob_dev("myblob", params).await;
        assert!(blob_dev.is_ok(), "Failed to create blob device");
        
        assert_eq!(blob_mgr.num_devices(), 1);
        assert!(blob_mgr.get_blob_dev("myblob").is_some());
        
        // Cleanup
        Managers::restart().await;
    }
}

