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

use crate::meta::MetaClient;
use crate::device::VDevParameters;

use super::fixed_blk_stream_vdev::{FixedBlkStreamVdev, FixedBlkStreamConfig};
use super::simple_log_stream_vdev::SimpleLogStreamVdev;

/// Streams Manager - manages all stream-based virtual devices
/// 
/// Provides type-specific APIs for creating and loading stream vdevs,
/// with separate meta_clients for each stream type.
pub struct StreamsManager {
    /// FixedBlkStream vdevs by vdev_id
    fixed_blk_streams: RwLock<HashMap<u32, Arc<FixedBlkStreamVdev>>>,
    
    /// SimpleLogStream vdevs by vdev_id
    simple_log_streams: RwLock<HashMap<u32, Arc<SimpleLogStreamVdev>>>,
    
    /// Meta client for FixedBlkStream metadata
    fixed_blk_meta_client: Arc<MetaClient>,
    
    /// Meta client for SimpleLogStream metadata
    simple_log_meta_client: Arc<MetaClient>,
}

impl StreamsManager {
    /// Create a new StreamsManager
    /// 
    /// Registers separate meta_clients for each stream type:
    /// - "FixedBlkStreamManager"
    /// - "SimpleLogStreamManager"
    pub async fn create() -> io::Result<Self> {
        let meta_mgr = crate::common::managers::metablk_mgr();
        
        // Register separate meta_clients for each stream type
        let fixed_blk_meta_client = meta_mgr.register_client("FixedBlkStreamManager".to_string()).await?;
        let simple_log_meta_client = meta_mgr.register_client("SimpleLogStreamManager".to_string()).await?;
        
        println!("StreamsManager: Created with separate meta_clients for each stream type");
        
        Ok(Self {
            fixed_blk_streams: RwLock::new(HashMap::new()),
            simple_log_streams: RwLock::new(HashMap::new()),
            fixed_blk_meta_client: Arc::new(fixed_blk_meta_client),
            simple_log_meta_client: Arc::new(simple_log_meta_client),
        })
    }
    
    /// Load existing StreamsManager during recovery
    /// 
    /// Registers meta_clients for each stream type. Actual stream vdev loading
    /// is deferred until `open_*()` is called, which will get the VirtualDev
    /// from DeviceManager and load the stream vdev with its chunks.
    pub async fn load() -> io::Result<Self> {
        let meta_mgr = crate::common::managers::metablk_mgr();
        
        // Register separate meta_clients for each stream type
        let fixed_blk_meta_client = meta_mgr.register_client("FixedBlkStreamManager".to_string()).await?;
        let simple_log_meta_client = meta_mgr.register_client("SimpleLogStreamManager".to_string()).await?;
        
        println!("StreamsManager: Registered meta_clients (vdevs will be loaded on-demand)");
        
        Ok(Self {
            fixed_blk_streams: RwLock::new(HashMap::new()),
            simple_log_streams: RwLock::new(HashMap::new()),
            fixed_blk_meta_client: Arc::new(fixed_blk_meta_client),
            simple_log_meta_client: Arc::new(simple_log_meta_client),
        })
    }
    
    // ===== FixedBlkStream APIs =====
    
    /// Create a new FixedBlkStream vdev
    /// 
    /// # Arguments
    /// * `vdev_params` - Parameters for creating the underlying VirtualDev
    /// * `num_sessions` - Number of concurrent sessions to support
    /// * `config` - Optional configuration for write units
    /// 
    /// # Returns
    /// Arc<FixedBlkStreamVdev> - The created stream vdev
    pub async fn create_fixed_blk_stream_vdev(
        &self,
        vdev_params: VDevParameters,
        num_sessions: usize,
        config: Option<FixedBlkStreamConfig>,
    ) -> io::Result<Arc<FixedBlkStreamVdev>> {
        // Create the FixedBlkStreamVdev (internally creates VirtualDev via DeviceManager)
        let stream_vdev = Arc::new(FixedBlkStreamVdev::create(
            vdev_params,
            self.fixed_blk_meta_client.clone(),
            num_sessions,
            config,
        ).await?);
        
        let vdev_id = stream_vdev.vdev_id();
        
        // Register in map
        self.fixed_blk_streams.write().insert(vdev_id, Arc::clone(&stream_vdev));
        
        println!("StreamsManager: Created FixedBlkStreamVdev {}", vdev_id);
        Ok(stream_vdev)  // Move, not clone - most efficient
    }
    
    /// Open an existing FixedBlkStream vdev by vdev_id
    /// 
    /// # Arguments
    /// * `vdev_id` - VirtualDev ID
    /// * `num_sessions` - Number of concurrent sessions to support
    /// * `config` - Optional configuration for write units
    /// 
    /// # Returns
    /// Arc<FixedBlkStreamVdev> - The loaded stream vdev
    pub async fn open_fixed_blk_stream_vdev(
        &self,
        vdev_id: u32,
        num_sessions: usize,
        config: Option<FixedBlkStreamConfig>,
    ) -> io::Result<Arc<FixedBlkStreamVdev>> {
        // Check if already loaded
        {
            let streams = self.fixed_blk_streams.read();
            if let Some(stream_vdev) = streams.get(&vdev_id) {
                println!("StreamsManager: FixedBlkStreamVdev {} already loaded", vdev_id);
                return Ok(stream_vdev.clone());
            }
        }
        
        // Get VirtualDev from DeviceManager
        let device_mgr = crate::common::managers::device_mgr();
        let vdev = device_mgr.get_vdev(vdev_id)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound,
                format!("VirtualDev {} not found", vdev_id)
            ))?;
        
        // Load the FixedBlkStreamVdev
        let stream_vdev = Arc::new(FixedBlkStreamVdev::load(
            vdev,
            Arc::clone(&self.fixed_blk_meta_client),
            num_sessions,
            config,
        ).await?);
        
        // Register in map
        self.fixed_blk_streams.write().insert(vdev_id, Arc::clone(&stream_vdev));
        
        println!("StreamsManager: Opened FixedBlkStreamVdev {}", vdev_id);
        Ok(stream_vdev)  // Move, not clone - most efficient
    }
    
    /// Get a FixedBlkStream vdev by vdev_id
    pub fn get_fixed_blk_stream_vdev(&self, vdev_id: u32) -> Option<Arc<FixedBlkStreamVdev>> {
        self.fixed_blk_streams.read().get(&vdev_id).cloned()
    }
    
    // ===== SimpleLogStream APIs =====
    
    /// Create a new SimpleLogStream vdev
    /// 
    /// # Arguments
    /// * `vdev_params` - Parameters for creating the underlying VirtualDev
    /// * `num_sessions` - Number of concurrent sessions to support
    /// 
    /// # Returns
    /// Arc<SimpleLogStreamVdev> - The created stream vdev
    pub async fn create_simple_log_stream_vdev(
        &self,
        vdev_params: VDevParameters,
        num_sessions: usize,
    ) -> io::Result<Arc<SimpleLogStreamVdev>> {
        // Create the SimpleLogStreamVdev (internally creates VirtualDev via DeviceManager)
        let stream_vdev = Arc::new(SimpleLogStreamVdev::create(
            vdev_params,
            self.simple_log_meta_client.clone(),
            num_sessions,
        ).await?);
        
        let vdev_id = stream_vdev.vdev_id();
        
        // Register in map
        self.simple_log_streams.write().insert(vdev_id, Arc::clone(&stream_vdev));
        
        println!("StreamsManager: Created SimpleLogStreamVdev {}", vdev_id);
        Ok(stream_vdev)  // Move, not clone - most efficient
    }
    
    /// Open an existing SimpleLogStream vdev by vdev_id
    /// 
    /// # Arguments
    /// * `vdev_id` - VirtualDev ID
    /// * `num_sessions` - Number of concurrent sessions to support
    /// 
    /// # Returns
    /// Arc<SimpleLogStreamVdev> - The loaded stream vdev
    pub async fn open_simple_log_stream_vdev(
        &self,
        vdev_id: u32,
        num_sessions: usize,
    ) -> io::Result<Arc<SimpleLogStreamVdev>> {
        // Check if already loaded
        {
            let streams = self.simple_log_streams.read();
            if let Some(stream_vdev) = streams.get(&vdev_id) {
                println!("StreamsManager: SimpleLogStreamVdev {} already loaded", vdev_id);
                return Ok(stream_vdev.clone());
            }
        }
        
        // Get VirtualDev from DeviceManager
        let device_mgr = crate::common::managers::device_mgr();
        let vdev = device_mgr.get_vdev(vdev_id)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound,
                format!("VirtualDev {} not found", vdev_id)
            ))?;
        
        // Load the SimpleLogStreamVdev
        let stream_vdev = Arc::new(SimpleLogStreamVdev::load(
            vdev,
            Arc::clone(&self.simple_log_meta_client),
            num_sessions,
        ).await?);
        
        // Register in map
        self.simple_log_streams.write().insert(vdev_id, Arc::clone(&stream_vdev));
        
        println!("StreamsManager: Opened SimpleLogStreamVdev {}", vdev_id);
        Ok(stream_vdev)  // Move, not clone - most efficient
    }
    
    /// Get a SimpleLogStream vdev by vdev_id
    pub fn get_simple_log_stream_vdev(&self, vdev_id: u32) -> Option<Arc<SimpleLogStreamVdev>> {
        self.simple_log_streams.read().get(&vdev_id).cloned()
    }
}
