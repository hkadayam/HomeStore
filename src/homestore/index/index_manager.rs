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

use crate::meta::{MetaBlkWrapper, MetaClient};
use crate::device::virtual_dev::{VirtualDev, VDevParameters};
use crate::device::{HSDevType, BlkAllocatorType, ChunkSelectorType, VDevSizeType, MultiPDevOpts};
use crate::streams::{SimpleLogStreamVdev, FixedBlkStreamVdev, FixedBlkStreamConfig};
use super::btree::underlying::cow_btree::COWBtree;
use super::btree::btree_node::{BtreeNodePtr, BNodeId};
use sisl::SimpleCache;
use iomgr::IOBuffer;

/// Index Manager - manages COW B-tree indexes
pub struct IndexManager {
    /// Meta client for persisting index metadata
    meta_client: Arc<MetaClient>,

    /// Cache for regular nodes (leaf/interior)
    node_cache: Arc<SimpleCache<BNodeId, BtreeNodePtr>>,

    /// Cache for overflow nodes (just buffers)
    overflow_cache: Arc<SimpleCache<BNodeId, Arc<IOBuffer>>>,
}

impl IndexManager {
    /// Create a new IndexManager
    ///
    /// This registers with the MetaBlkManager as a client and creates caches.
    ///
    /// # Arguments
    /// * `node_cache_capacity` - Max capacity for regular node cache (in entries)
    /// * `overflow_cache_capacity` - Max capacity for overflow node cache (in entries)
    pub async fn create(
        node_cache_capacity: u64,
        overflow_cache_capacity: u64,
    ) -> io::Result<Self> {
        let meta_mgr = crate::common::managers::metablk_mgr();
        let meta_client = meta_mgr.register_client("index_manager".to_string()).await?;

        // Create cache for regular nodes (leaf/interior)
        let node_cache = Arc::new(SimpleCache::<BNodeId, BtreeNodePtr>::new(node_cache_capacity));

        // Create cache for overflow nodes (just buffers)
        let overflow_cache = Arc::new(SimpleCache::<BNodeId, Arc<IOBuffer>>::new(overflow_cache_capacity));

        println!("IndexManager: Created with node_cache={} entries, overflow_cache={} entries",
                 node_cache_capacity, overflow_cache_capacity);

        Ok(Self {
            meta_client: Arc::new(meta_client),
            node_cache,
            overflow_cache,
        })
    }
    
    /// Create a new COW B-tree
    ///
    /// This creates 3 VDevs:
    /// 1. SimpleLogStreamVdev for full map (node_id -> blkid)
    /// 2. SimpleLogStreamVdev for incremental map
    /// 3. FixedBlkStreamVdev for btree nodes
    ///
    /// # Arguments
    /// * `name` - Name of the B-tree
    /// * `ordinal` - B-tree ordinal (for multi-tree support)
    pub async fn create_cow_btree(
        &self,
        name: String,
        ordinal: u16,
    ) -> io::Result<Arc<COWBtree>> {
        println!("IndexManager: Creating COW B-tree '{}' (ordinal={})", name, ordinal);

        COWBtree::create(
            name,
            ordinal,
            Arc::clone(&self.meta_client),
            Arc::clone(&self.node_cache),
            Arc::clone(&self.overflow_cache),
        ).await
    }

    /// Load an existing COW B-tree from metadata
    ///
    /// # Arguments
    /// * `metablk` - Metadata block containing the B-tree configuration
    pub async fn load_cow_btree(
        &self,
        metablk: MetaBlkWrapper,
    ) -> io::Result<Arc<COWBtree>> {
        println!("IndexManager: Loading COW B-tree from metadata");

        COWBtree::load(
            metablk,
            Arc::clone(&self.meta_client),
            Arc::clone(&self.node_cache),
            Arc::clone(&self.overflow_cache),
        ).await
    }
    
    /// Get meta client reference
    pub fn meta_client(&self) -> &Arc<MetaClient> {
        &self.meta_client
    }
}

/// COW B-tree metadata (persisted in MetaBlk)
///
/// This stores the VDev IDs for the 3 VDevs used by a COW B-tree:
/// 1. full_map_vdev_id - VDev for full node ID → block ID map
/// 2. incr_map_vdev_id - VDev for incremental map updates (journal)
/// 3. node_vdev_id - VDev for B-tree nodes
#[derive(Debug, Clone, Serialize, Deserialize)]
#[repr(C)]
pub struct COWBtreeMetadata {
    /// Magic number for validation
    pub magic: u64,     // Magic number for validation
    pub version: u32,     // Version number
    pub ordinal: u32,     // B-tree ordinal
    pub full_map_vdev_id: u32,    // VDev ID for full map (SimpleLogStreamVdev)
    pub incr_map_vdev_id: u32,    // VDev ID for incremental map (SimpleLogStreamVdev)
    pub node_vdev_id: u32,        // VDev ID for B-tree nodes (FixedBlkStreamVdev)
    pub root_node_id: u64,        // Root node ID
    pub name: String,     // B-tree name (null-terminated)
}

impl COWBtreeMetadata {
    const MAGIC: u64 = 0x434F5742545245; // "COWBTRE" in ASCII
    const VERSION: u32 = 1;
    
    /// Create a new COWBtreeMetadata
    pub fn new(
        name: String,
        ordinal: u32,
        full_map_vdev_id: u32,
        incr_map_vdev_id: u32,
        node_vdev_id: u32,
    ) -> Self {
        let mut meta = Self {
            magic: Self::MAGIC,
            version: Self::VERSION,
            ordinal,
            full_map_vdev_id,
            incr_map_vdev_id,
            node_vdev_id,
            root_node_id: super::btree_node::EMPTY_BNODEID,
            name: String::from(name),
        };       
        
        meta
    }
    
    /// Validate the metadata
    pub fn validate(&self) -> io::Result<()> {
        if self.magic != Self::MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Invalid COWBtreeMetadata magic: expected 0x{:x}, got 0x{:x}",
                        Self::MAGIC, self.magic),
            ));
        }
        
        if self.version != Self::VERSION {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unsupported COWBtreeMetadata version: {}", self.version),
            ));
        }
        
        Ok(())
    }
    
    /// Get the B-tree name as a string
    pub fn get_name(&self) -> String {
        let end = self.name.iter().position(|&c| c == 0).unwrap_or(self.name.len());
        String::from_utf8_lossy(&self.name[..end]).to_string()
    }
}

impl Default for COWBtreeMetadata {
    fn default() -> Self {
        Self {
            magic: Self::MAGIC,
            version: Self::VERSION,
            ordinal: 0,
            full_map_vdev_id: 0,
            incr_map_vdev_id: 0,
            node_vdev_id: 0,
            root_node_id: super::btree_node::EMPTY_BNODEID,
            name: String::from(""),
        }
    }
}
