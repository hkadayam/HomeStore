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

//! Copy-On-Write B-tree Implementation
//!
//! This is a Rust port of the C++ COWBtree class, focusing on the basic
//! operations: create, read, write, flush, and delete nodes.
//!
//! # Architecture
//!
//! The COW B-tree maintains:
//! - A cache of in-memory nodes (DashMap for concurrent access)
//! - A mapping from compact node IDs to block IDs (for disk locations)
//! - Checkpoint sessions that track dirty/deleted nodes
//! - Integration with VirtualDev for block I/O
//!
//! # Copy-On-Write Semantics
//!
//! When a node is modified in a new checkpoint:
//! 1. If the node's modified_cp_id matches the current CP, reuse the buffer
//! 2. Otherwise, create a new buffer copy (COW) and update modified_cp_id
//! 3. Add the node to the current checkpoint's dirty list
//!
//! During flush, all dirty nodes are written to new blocks, and the
//! node ID -> block ID mapping is updated.

use std::sync::{Arc, Mutex};
use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, AtomicU64, Ordering};
use std::io;
use arc_swap::ArcSwap;
use dashmap::DashMap;
use parking_lot::RwLock;
use sisl::LargeIDReserver;

use crate::device::virtual_dev::{VDevParameters, VDevSizeType, MultiPDevOpts};
use crate::device::{BlkAllocatorType, ChunkSelectorType, HSDevType};
use crate::common::BlkId;
use iomgr::IOBuffer;
use crate::meta::{MetaBlkWrapper, MetaClient};
use crate::streams::{SimpleLogStreamVdev, FixedBlkStreamVdev, FixedBlkStreamConfig};
use crate::index::btree::btree_node::*;
use crate::index::index_manager::COWBtreeMetadata;

/// Maximum number of concurrent checkpoint sessions
const MAX_CP_SESSIONS: usize = 2;

/// COWBtree Tunables
/// 
/// These are compile-time constants that control the behavior of the COW B-tree.
/// In the future, these may be moved to a global settings structure.
pub struct Tunables;

impl Tunables {
    /// Chunk size for full map VDev (in bytes)
    pub const FULL_MAP_CHUNK_SIZE: u64 = 64 * 1024 * 1024; // 64MB
    
    /// Chunk size for incremental map VDev (in bytes)
    pub const INCR_MAP_CHUNK_SIZE: u64 = 16 * 1024 * 1024; // 16MB
    
    /// Chunk size for node VDev (in bytes)
    pub const NODE_VDEV_CHUNK_SIZE: u64 = 128 * 1024 * 1024; // 128MB
    
    /// Interior (internal) node size (in bytes)
    pub const INTERIOR_NODE_SIZE: u32 = 4096; // 4KB
    
    /// Leaf node size (in bytes)
    pub const LEAF_NODE_SIZE: u32 = 4096; // 4KB
    
    /// Minimum overflow node block size (in bytes)
    pub const MIN_OVERFLOW_NODE_BLK_SIZE: u32 = 512; // 512 bytes
    
    /// Node VDev block size - minimum of all node sizes to support all types
    pub const NODE_VDEV_BLK_SIZE: u32 = {
        let mut min = Self::INTERIOR_NODE_SIZE;
        if Self::LEAF_NODE_SIZE < min {
            min = Self::LEAF_NODE_SIZE;
        }
        if Self::MIN_OVERFLOW_NODE_BLK_SIZE < min {
            min = Self::MIN_OVERFLOW_NODE_BLK_SIZE;
        }
        min
    };
    
    /// Interior node block multiplier (how many blocks per interior node)
    pub const INTERIOR_BLK_MULTIPLIER: u32 = Self::INTERIOR_NODE_SIZE / Self::NODE_VDEV_BLK_SIZE;
    
    /// Leaf node block multiplier (how many blocks per leaf node)
    pub const LEAF_BLK_MULTIPLIER: u32 = Self::LEAF_NODE_SIZE / Self::NODE_VDEV_BLK_SIZE;
    
    /// Overflow node block multiplier (minimum blocks per overflow node)
    /// Overflow nodes can be variable size, this is the minimum
    pub const OVERFLOW_BLK_MULTIPLIER: u32 = Self::MIN_OVERFLOW_NODE_BLK_SIZE / Self::NODE_VDEV_BLK_SIZE;
}

/// Result type for btree operations
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BtreeStatus {
    Success,
    NotFound,
    SpaceNotAvail,
    CpMismatch,
}

/// Compact block ID (matches C++ struct)
/// 
/// This is a space-efficient representation of a BlkId that packs:
/// - Valid flag (1 bit)
/// - Block number (31 bits)
/// - Chunk number (16 bits)
/// Total: 6 bytes instead of BlkId's larger size
#[derive(Debug, Clone, Copy)]
#[repr(C, packed)]
pub struct CompactBlkId {
    /// Valid flag (1 bit) + block number (31 bits)
    blk_num_with_valid: u32,
    /// Chunk number
    chunk_num: u16,
}

impl CompactBlkId {
    /// Create an invalid (empty) CompactBlkId
    pub fn new() -> Self {
        Self {
            blk_num_with_valid: 0, // is_valid = 0
            chunk_num: 0,
        }
    }
    
    /// Create from a BlkId
    pub fn from_blkid(blk_id: &BlkId) -> Self {
        Self {
            blk_num_with_valid: (1u32 << 31) | (blk_id.blk_num() & 0x7FFF_FFFF),
            chunk_num: blk_id.chunk_num(),
        }
    }
    
    /// Check if this is a valid block ID
    pub fn is_valid(&self) -> bool {
        (self.blk_num_with_valid & (1u32 << 31)) != 0
    }
    
    /// Convert to BlkId (returns None if invalid)
    pub fn to_blkid(&self) -> Option<BlkId> {
        if !self.is_valid() {
            None
        } else {
            let blk_num = self.blk_num_with_valid & 0x7FFF_FFFF;
            Some(BlkId::new(blk_num, 1, self.chunk_num))
        }
    }
}

impl Default for CompactBlkId {
    fn default() -> Self {
        Self::new()
    }
}

/// Flush node info - wrapper around node with buffer management
/// 
/// This holds a reference to the node and a shared reference to its buffer
/// at the time of flushing. Even if the node is later modified (COW),
/// the flush still has access to the original buffer.
pub struct FlushNodeInfo {
    pub node: BtreeNodePtr,
    pub buf: Arc<Vec<u8>>,
}

impl FlushNodeInfo {
    /// Create a new FlushNodeInfo by sharing the node's current buffer
    pub fn new(node: BtreeNodePtr) -> Self {
        let buf = node.share_phys_node_buf();
        Self { node, buf }
    }
    
    /// Get the buffer bytes as a slice
    pub fn bytes(&self) -> &[u8] {
        &self.buf
    }
}

/// Context for checkpoint operations (simplified)
/// 
/// In the full implementation, this would be part of the checkpoint manager
/// and contain additional context about the checkpoint being executed.
pub struct CPContext {
    cp_id: i64,
}

impl CPContext {
    pub fn new(cp_id: i64) -> Self {
        Self { cp_id }
    }
    
    pub fn id(&self) -> i64 {
        self.cp_id
    }
}

/// Checkpoint session - tracks dirty and deleted nodes for a single checkpoint
/// 
/// Each checkpoint has its own session that accumulates:
/// - Modified nodes (dirty list) - to be flushed to disk
/// - Deleted nodes - to be removed from the node ID map
/// - New root node ID (if root changed)
/// 
/// The session is used during the checkpoint flush phase to write all
/// dirty nodes and update the persistent state.
pub struct CPSession {
    cp_id: AtomicI64,
    modified_nodes: Mutex<Vec<FlushNodeInfo>>,
    deleted_nodes: Mutex<Vec<CompactNodeId>>,
    new_root_id: AtomicU64,
}

impl CPSession {
    pub fn new() -> Self {
        Self {
            cp_id: AtomicI64::new(-1),
            modified_nodes: Mutex::new(Vec::new()),
            deleted_nodes: Mutex::new(Vec::new()),
            new_root_id: AtomicU64::new(EMPTY_BNODEID),
        }
    }
    
    pub fn set_cp_id(&self, cp_id: i64) {
        self.cp_id.store(cp_id, Ordering::Release);
    }
    
    pub fn cp_id(&self) -> i64 {
        self.cp_id.load(Ordering::Acquire)
    }
    
    pub fn add_modified_node(&self, finfo: FlushNodeInfo) {
        self.modified_nodes.lock().unwrap().push(finfo);
    }
    
    pub fn add_deleted_node(&self, node_id: CompactNodeId) {
        self.deleted_nodes.lock().unwrap().push(node_id);
    }
    
    pub fn get_modified_nodes(&self) -> Vec<FlushNodeInfo> {
        std::mem::take(&mut *self.modified_nodes.lock().unwrap())
    }
    
    pub fn get_deleted_nodes(&self) -> Vec<CompactNodeId> {
        std::mem::take(&mut *self.deleted_nodes.lock().unwrap())
    }
    
    pub fn clear(&self) {
        self.modified_nodes.lock().unwrap().clear();
        self.deleted_nodes.lock().unwrap().clear();
        self.new_root_id.store(EMPTY_BNODEID, Ordering::Release);
    }
    
    pub fn set_new_root_id(&self, node_id: BNodeId) {
        self.new_root_id.store(node_id, Ordering::Release);
    }
    
    pub fn new_root_id(&self) -> BNodeId {
        self.new_root_id.load(Ordering::Acquire)
    }
}

/// Copy-On-Write B-tree implementation
/// 
/// This implements the lower-layer COW B-tree that manages node persistence,
/// caching, and checkpoint integration. It works in conjunction with the
/// upper-layer B-tree logic (not included in this port) that handles the
/// actual B-tree algorithms (search, insert, split, etc.).
/// 
/// # Node ID Format
/// 
/// Node IDs are 64-bit values split into:
/// - Upper 32 bits: B-tree ordinal (for multi-tree support)
/// - Lower 32 bits: Compact node ID (unique within this tree)
/// 
/// This allows multiple B-trees to coexist with non-overlapping node ID spaces.
/// 
/// # VDev Architecture
/// 
/// COWBtree uses 3 separate VDevs:
/// 1. **full_map_vdev** (SimpleLogStreamVdev) - Stores full node ID → block ID map
/// 2. **incr_map_vdev** (SimpleLogStreamVdev) - Stores incremental map updates (journal)
/// 3. **node_vdev** (FixedBlkStreamVdev) - Stores actual B-tree nodes
pub struct COWBtree {
    /// VDev for full node map (SimpleLogStreamVdev)
    full_map_vdev: Arc<SimpleLogStreamVdev>,

    /// VDev for incremental map updates (SimpleLogStreamVdev)
    incr_map_vdev: Arc<SimpleLogStreamVdev>,

    /// VDev for B-tree nodes (FixedBlkStreamVdev)
    node_vdev: Arc<FixedBlkStreamVdev>,

    /// Cache for regular nodes (leaf/interior) - shared across all btrees
    node_cache: Arc<sisl::SimpleCache<BNodeId, BtreeNodePtr>>,

    /// Cache for overflow nodes - shared across all btrees
    overflow_cache: Arc<sisl::SimpleCache<BNodeId, Arc<IOBuffer>>>,
    
    /// NodeID to BlkId mapping (compact format)
    /// Protected by RwLock for concurrent reads, exclusive writes
    /// Sorted by node ID for efficient range operations and persistence
    bnodeid_map: Arc<RwLock<HashMap<CompactNodeId, CompactBlkId>>>,
    
    /// Large ID reserver for allocating node numbers
    /// Uses interval set to efficiently track and reuse freed IDs
    node_num_reserver: Arc<LargeIDReserver>,
    
    /// B-tree ordinal (for multi-tree support) - 16 bits
    btree_ordinal: u16,
    
    /// Precomputed prefix for regular node IDs (ordinal + overflow_bit=0)
    regular_id_prefix: u64,
    
    /// Precomputed prefix for overflow node IDs (ordinal + overflow_bit=1)
    overflow_id_prefix: u64,
    
    /// Root node ID (using ArcSwap for lock-free updates)
    root_node_id: ArcSwap<BNodeId>,
    
    /// B-tree name
    name: String,
    
    /// Metadata block wrapper (stores vdev IDs)
    meta_blk: MetaBlkWrapper,
    
    /// Checkpoint sessions (simplified - only 2 concurrent CPs)
    /// Index is cp_id % 2
    cp_sessions: [Arc<CPSession>; 2],
}

impl COWBtree {
    // Node ID bit layout:
    // [63:48] - 16 bits: btree_ordinal
    // [47]    - 1 bit:   overflow flag (0=regular, 1=overflow)
    // [46:0]  - 47 bits: node_number
    
    const BTREE_ORDINAL_BITS: u64 = 16;
    const BTREE_OVERFLOW_BIT: u64 = 1;
    const BTREE_NODE_NUMBER_BITS: u64 = 47;
    
    const BTREE_ORDINAL_SHIFT: u64 = 48;
    const BTREE_OVERFLOW_SHIFT: u64 = 47;
    
    const BTREE_NODE_NUMBER_MASK: u64 = (1u64 << Self::BTREE_NODE_NUMBER_BITS) - 1;
    
    /// Maximum number of nodes per B-tree (2^47)
    const MAX_NODE_COUNT: u64 = 1u64 << Self::BTREE_NODE_NUMBER_BITS;
    const BTREE_OVERFLOW_MASK: u64 = 1u64 << Self::BTREE_OVERFLOW_SHIFT;
    const BTREE_ORDINAL_MASK: u64 = ((1u64 << Self::BTREE_ORDINAL_BITS) - 1) << Self::BTREE_ORDINAL_SHIFT;
    
    /// Create a new COW B-tree with 3 VDevs
    ///
    /// Creates:
    /// 1. SimpleLogStreamVdev for full map
    /// 2. SimpleLogStreamVdev for incremental map
    /// 3. FixedBlkStreamVdev for nodes
    ///
    /// # Arguments
    /// * `name` - B-tree name
    /// * `ordinal` - Ordinal for this tree (for multi-tree support, 16-bit)
    /// * `meta_client` - MetaClient for metadata persistence
    /// * `node_cache` - Shared cache for regular nodes
    /// * `overflow_cache` - Shared cache for overflow nodes
    pub async fn create(
        name: String,
        ordinal: u16,
        meta_client: Arc<MetaClient>,
        node_cache: Arc<sisl::SimpleCache<BNodeId, BtreeNodePtr>>,
        overflow_cache: Arc<sisl::SimpleCache<BNodeId, Arc<IOBuffer>>>,
    ) -> io::Result<Arc<Self>> {
        println!("COWBtree::create: Creating '{}' (ordinal={}, interior_node_size={}, leaf_node_size={})", 
                 name, ordinal, Tunables::INTERIOR_NODE_SIZE, Tunables::LEAF_NODE_SIZE);
        
        // Precompute node ID prefixes
        // Regular nodes: [ordinal(16 bits)][0(1 bit)][node_number(47 bits)]
        let regular_id_prefix = (ordinal as u64) << Self::BTREE_ORDINAL_SHIFT;
        
        // Overflow nodes: [ordinal(16 bits)][1(1 bit)][node_number(47 bits)]
        let overflow_id_prefix = ((ordinal as u64) << Self::BTREE_ORDINAL_SHIFT) | Self::BTREE_OVERFLOW_MASK;
        
        println!("COWBtree: Node ID prefixes - regular: 0x{:016x}, overflow: 0x{:016x}", 
                 regular_id_prefix, overflow_id_prefix);
        
        // Create VDev parameters for full map (SimpleLogStreamVdev)
        let full_map_params = VDevParameters {
            vdev_name: format!("{}_full_map", name),
            vdev_size: Tunables::FULL_MAP_CHUNK_SIZE,  // start with 1 chunk size
            num_chunks: 1, // Start with 1 chunk
            chunk_size: Tunables::FULL_MAP_CHUNK_SIZE,
            incremental_chunk_size: Tunables::FULL_MAP_CHUNK_SIZE,
            size_type: VDevSizeType::Dynamic,
            blk_size: 4096, // Really doesn't matter because append happens at byte level
            dev_type: HSDevType::Fast,
            multi_pdev_opts: MultiPDevOpts::SingleRandomPDev,
            num_mirrors: 0,
            alloc_type: BlkAllocatorType::None,
            chunk_sel_type: ChunkSelectorType::MostAvailableSpace,
            chunk_pool_limit: Some(1), // Pool 1 chunk
        };
        
        // Create VDev parameters for incremental map (SimpleLogStreamVdev)
        let incr_map_params = VDevParameters {
            vdev_name: format!("{}_incr_map", name),
            vdev_size: 0,
            num_chunks: 0,
            chunk_size: 0,
            incremental_chunk_size: Tunables::INCR_MAP_CHUNK_SIZE,
            size_type: VDevSizeType::Dynamic,
            blk_size: 4096,
            dev_type: HSDevType::Data,
            multi_pdev_opts: MultiPDevOpts::SingleFirstPDev,
            num_mirrors: 0,
            alloc_type: BlkAllocatorType::None, // Use EmptyBlkAllocator for logs
            chunk_sel_type: ChunkSelectorType::RoundRobin,
            chunk_pool_limit: Some(1), // Pool 1 chunk
        };
        
        // Create VDev parameters for nodes (FixedBlkStreamVdev)
        // Use the minimum node size as block size to support all node types
        let node_vdev_params = VDevParameters {
            vdev_name: format!("{}_nodes", name),
            vdev_size: 0,
            num_chunks: 0,
            chunk_size: 0,
            incremental_chunk_size: Tunables::NODE_VDEV_CHUNK_SIZE,
            size_type: VDevSizeType::Dynamic,
            blk_size: Tunables::NODE_VDEV_BLK_SIZE,
            dev_type: HSDevType::Data,
            multi_pdev_opts: MultiPDevOpts::SingleFirstPDev,
            num_mirrors: 0,
            alloc_type: BlkAllocatorType::Varsize,
            chunk_sel_type: ChunkSelectorType::RoundRobin,
            chunk_pool_limit: Some(4), // Pool up to 4 chunks per size
        };
        
        // Create the 3 VDevs
        println!("COWBtree: Creating full_map_vdev");
        let full_map_vdev = Arc::new(
            SimpleLogStreamVdev::create(full_map_params, Arc::clone(&meta_client), MAX_CP_SESSIONS).await?
        );
        
        println!("COWBtree: Creating incr_map_vdev");
        let incr_map_vdev = Arc::new(
            SimpleLogStreamVdev::create(incr_map_params, Arc::clone(&meta_client), MAX_CP_SESSIONS).await?
        );
        
        println!("COWBtree: Creating node_vdev");
        let node_vdev = Arc::new(
            FixedBlkStreamVdev::create(
                node_vdev_params,
                Arc::clone(&meta_client),
                MAX_CP_SESSIONS,
                Some(FixedBlkStreamConfig::default())
            ).await?
        );
        
        // Get VDev IDs
        let full_map_vdev_id = full_map_vdev.vdev_id();
        let incr_map_vdev_id = incr_map_vdev.vdev_id();
        let node_vdev_id = node_vdev.vdev_id();
        
        println!("COWBtree: Created VDevs: full_map={}, incr_map={}, nodes={}", 
                 full_map_vdev_id, incr_map_vdev_id, node_vdev_id);
        
        // Create metadata block, initialize meta_data, serilialize and write to meta_blk
        let meta_blk = MetaBlkWrapper::create(
            meta_client,
            &format!("cow_btree_{}", name.clone()),
            Some(std::mem::size_of::<COWBtreeMetadata>()),
        ).await?;

        let meta_data = COWBtreeMetadata::new(name.clone(), ordinal as u32, full_map_vdev_id, 
                                              incr_map_vdev_id, node_vdev_id);
        let meta_bytes = bincode::serialize(&meta_data)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        meta_blk.write(&meta_bytes).await?;
        
        Ok(Arc::new(Self {
            full_map_vdev,
            incr_map_vdev,
            node_vdev,
            node_cache,
            overflow_cache,
            bnodeid_map: Arc::new(RwLock::new(HashMap::new())),
            node_num_reserver: Arc::new(LargeIDReserver::new(Self::MAX_NODE_COUNT)),
            btree_ordinal: ordinal,
            regular_id_prefix,
            overflow_id_prefix,
            root_node_id: ArcSwap::from_pointee(EMPTY_BNODEID),
            name,
            meta_blk,
            cp_sessions: [Arc::new(CPSession::new()), Arc::new(CPSession::new())],
        }))
    }
    
    /// Load an existing COW B-tree from metadata
    ///
    /// # Arguments
    /// * `meta_blk` - Metadata block containing the B-tree configuration
    /// * `meta_client` - MetaClient for accessing metadata
    /// * `node_cache` - Shared cache for regular nodes
    /// * `overflow_cache` - Shared cache for overflow nodes
    pub async fn load(
        meta_blk: MetaBlkWrapper,
        meta_client: Arc<MetaClient>,
        node_cache: Arc<sisl::SimpleCache<BNodeId, BtreeNodePtr>>,
        overflow_cache: Arc<sisl::SimpleCache<BNodeId, Arc<IOBuffer>>>,
    ) -> io::Result<Arc<Self>> {
        // Read and validate metadata
        let meta_bytes = meta_blk.read().await?;
        let meta_data: COWBtreeMetadata = bincode::deserialize(&meta_bytes)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        meta_data.validate()?;
        
        println!("COWBtree::load: Loading '{}' (ordinal={})", meta_data.get_name(), meta_data.ordinal);
        
        // Get DeviceManager to load VDevs
        let device_mgr = crate::common::managers::device_mgr();
        
        // Load the 3 VDevs by their IDs
        let full_map_vdev_arc = device_mgr.get_vdev(meta_data.full_map_vdev_id)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound,
                format!("Full map VDev {} not found", meta_data.full_map_vdev_id)))?;
        
        let incr_map_vdev_arc = device_mgr.get_vdev(meta_data.incr_map_vdev_id)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound,
                format!("Incr map VDev {} not found", meta_data.incr_map_vdev_id)))?;
        
        let node_vdev_arc = device_mgr.get_vdev(meta_data.node_vdev_id)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound,
                format!("Node VDev {} not found", meta_data.node_vdev_id)))?;
        
        // Reconstruct the stream vdevs
        // Note: SimpleLogStreamVdev and FixedBlkStreamVdev need load() methods
        // For now, we'll create wrappers (TODO: implement proper load methods)
        let full_map_vdev = Arc::new(
            SimpleLogStreamVdev::load(full_map_vdev_arc, meta_client.clone(), MAX_CP_SESSIONS).await?);
        
        let incr_map_vdev = Arc::new(
            SimpleLogStreamVdev::load(incr_map_vdev_arc, meta_client.clone(), MAX_CP_SESSIONS).await?);
        
        let node_vdev = Arc::new(
            FixedBlkStreamVdev::load(
                node_vdev_arc,
                meta_client.clone(),
                MAX_CP_SESSIONS,
                Some(FixedBlkStreamConfig::default())).await?);
        
        // Precompute node ID prefixes (same as in create)
        let regular_id_prefix = (meta_data.ordinal as u64) << Self::BTREE_ORDINAL_SHIFT;
        let overflow_id_prefix = ((meta_data.ordinal as u64) << Self::BTREE_ORDINAL_SHIFT) | Self::BTREE_OVERFLOW_MASK;
        
        println!("COWBtree: Loaded VDevs: full_map={}, incr_map={}, nodes={}", 
                 meta_data.full_map_vdev_id, meta_data.incr_map_vdev_id, meta_data.node_vdev_id);
        
        // TODO: Load node ID map from full_map_vdev and incr_map_vdev
        
        Ok(Arc::new(Self {
            full_map_vdev,
            incr_map_vdev,
            node_vdev,
            node_cache,
            overflow_cache,
            bnodeid_map: Arc::new(RwLock::new(HashMap::new())),
            node_num_reserver: Arc::new(LargeIDReserver::new(Self::MAX_NODE_COUNT)),
            btree_ordinal: meta_data.ordinal as u16,
            regular_id_prefix,
            overflow_id_prefix,
            root_node_id: ArcSwap::from_pointee(meta_data.root_node_id),
            name: meta_data.get_name().clone(),
            meta_blk,
            cp_sessions: [Arc::new(CPSession::new()), Arc::new(CPSession::new())],
        }))
    }
    
    /// Generate a new node ID
    /// 
    /// Uses the LargeIDReserver to allocate a unique node number, which can
    /// efficiently reuse freed IDs. The node number is combined with the
    /// precomputed prefix (ordinal + overflow bit) to form the full 64-bit ID.
    /// 
    /// # Arguments
    /// * `is_overflow` - If true, generates an overflow node ID, else regular node ID
    /// 
    /// # Returns
    /// A unique 64-bit node ID with format:
    /// [ordinal(16 bits)][overflow_bit(1 bit)][node_number(47 bits)]
    /// 
    /// # Panics
    /// Panics if no more node IDs are available (2^47 limit reached)
    async fn generate_node_id(&self, is_overflow: bool) -> BNodeId {
        let node_number = self.node_num_reserver.reserve().await;
        
        // Check for out of bounds (should never happen with 2^47 limit)
        assert!(node_number != LargeIDReserver::OUT_OF_BOUNDS,
            "Exhausted node ID space (2^47 nodes allocated)");
        
        // Ensure node_number fits in 47 bits (should always be true due to MAX_NODE_COUNT)
        debug_assert!(node_number <= Self::BTREE_NODE_NUMBER_MASK, "Node num {} exceeds 47-bit limit",  node_number);
        
        if is_overflow {
            self.overflow_id_prefix | node_number
        } else {
            self.regular_id_prefix | node_number
        }
    }
    
    /// Convert full node ID to compact node ID (47-bit node number)
    /// 
    /// Extracts the lower 47 bits (node number) from the full 64-bit node ID.
    #[inline]
    fn to_compact_nodeid(node_id: BNodeId) -> CompactNodeId {
        (node_id & Self::BTREE_NODE_NUMBER_MASK) as CompactNodeId
    }
    
    /// Check if a node ID is an overflow node
    #[inline]
    fn is_overflow_node(node_id: BNodeId) -> bool {
        (node_id & Self::BTREE_OVERFLOW_MASK) != 0
    }
    
    /// Extract ordinal from node ID
    #[inline]
    fn extract_ordinal(node_id: BNodeId) -> u16 {
        ((node_id & Self::BTREE_ORDINAL_MASK) >> Self::BTREE_ORDINAL_SHIFT) as u16
    }
    
    /// Create a new node
    /// 
    /// This allocates a new node ID, creates the node in memory,
    /// adds it to the cache, and adds it to the checkpoint's dirty list.
    /// 
    /// # Arguments
    /// * `is_leaf` - True if this should be a leaf node
    /// * `is_overflow` - True if this should be an overflow node
    /// * `cp_ctx` - Checkpoint context for tracking the modification
    /// 
    /// # Returns
    /// Reference-counted pointer to the new node
    pub async fn create_node(&self, is_leaf: bool, is_overflow: bool, cp_ctx: &CPContext) -> BtreeNodePtr {
        let node_id = self.generate_node_id(is_overflow).await;
        
        // Determine node size based on type (using Tunables)
        let node_size = if is_leaf {
            Tunables::LEAF_NODE_SIZE
        } else {
            Tunables::INTERIOR_NODE_SIZE
        };
        
        let node = Arc::new(BtreeNode::new(node_id, is_leaf, node_size));
        
        // Add to cache
        self.node_cache.insert(node_id, node.clone());
        
        // Add to dirty list for this checkpoint
        self.add_to_dirty_list(FlushNodeInfo::new(node.clone()), cp_ctx);
        
        // Set modified CP ID
        node.set_modified_cp_id(cp_ctx.id());
        
        println!("COWBtree: Created node {} (leaf={}, overflow={}, size={}) for CP {}", 
                 node_id, is_leaf, is_overflow, node_size, cp_ctx.id());
        
        node
    }
    
    /// Read node from cache or disk
    /// 
    /// First attempts to read from the in-memory cache. If not found,
    /// looks up the block ID in the node map and reads from disk via vdev.
    /// 
    /// # Arguments
    /// * `node_id` - Node ID to read
    /// 
    /// # Returns
    /// Result containing the node or an error status
    pub async fn read_node(&self, node_id: BNodeId) -> Result<BtreeNodePtr, BtreeStatus> {
        // Try cache first
        if let Some(entry) = self.node_cache.get(&node_id) {
            println!("COWBtree: Read node {} from cache", node_id);
            return Ok(entry.clone());
        }
        
        // Get block ID from map
        let compact_id = Self::to_compact_nodeid(node_id);
        let blkid = {
            let map = self.bnodeid_map.read();
            map.get(&compact_id)
                .and_then(|cblk| cblk.to_blkid())
                .ok_or(BtreeStatus::NotFound)?
        };
        
        println!("COWBtree: Reading node {} from disk at blk {:?}", node_id, blkid);
        
        // Determine node size (check if overflow node)
        // Determine node size based on type (from node_id overflow bit and persistent header later)
        // For now, allocate based on the node type we expect
        // TODO: Read persistent header first to determine exact size
        let is_overflow = Self::is_overflow_node(node_id);
        let node_size = if is_overflow {
            // Overflow nodes can be variable size, use minimum block size
            Tunables::NODE_VDEV_BLK_SIZE
        } else {
            // Regular nodes: use maximum to handle both interior and leaf
            Tunables::INTERIOR_NODE_SIZE.max(Tunables::LEAF_NODE_SIZE)
        };
        
        // Read from disk via node_vdev
        let buf = IOBuffer::new(node_size as usize);
        let (result, buf) = self.node_vdev.vdev().read(buf, &blkid).await;
        result.map_err(|e| {
            eprintln!("COWBtree: Read failed for node {}: {}", node_id, e);
            BtreeStatus::NotFound
        })?;
        
        // Create node from buffer (PersistentHeader already contains node_id, is_leaf, etc.)
        let buffer = buf.as_slice().to_vec();
        let node = Arc::new(BtreeNode::from_buffer(buffer));
        
        // Add to cache
        self.node_cache.insert(node_id, node.clone());
        println!("COWBtree: Node {} loaded from disk and cached", node_id);
        Ok(node)
    }
    
    /// Write node (no-op for COW, actual write happens in flush)
    /// 
    /// In COW B-tree, we don't write nodes immediately. Instead, they're
    /// accumulated in the dirty list and written during checkpoint flush.
    pub fn write_node(&self, _node: &BtreeNodePtr, _cp_ctx: &CPContext) -> BtreeStatus {
        BtreeStatus::Success
    }
    
    /// Refresh node for read-modify-write (implements COW)
    /// 
    /// This is called before modifying a node. It implements the COW logic:
    /// - If the node was already modified in this checkpoint, reuse the buffer
    /// - If the node was modified in an older checkpoint, create a new buffer copy
    /// - Add the node to the dirty list if it wasn't already there
    /// 
    /// # Arguments
    /// * `node` - Node to refresh
    /// * `for_read_modify_write` - True if the caller intends to modify the node
    /// * `cp_ctx` - Current checkpoint context
    /// 
    /// # Returns
    /// Status indicating success or CP mismatch
    pub fn refresh_node(
        &self,
        node: &BtreeNodePtr,
        for_read_modify_write: bool,
        cp_ctx: &CPContext,
    ) -> BtreeStatus {
        if !for_read_modify_write {
            return BtreeStatus::Success;
        }
        
        let mod_cp_id = node.get_modified_cp_id();
        let cur_cp_id = cp_ctx.id();
        
        if mod_cp_id == cur_cp_id {
            // Same CP, can reuse buffer
            println!("COWBtree: Node {} already modified in CP {}, reusing buffer", 
                     node.node_id(), cur_cp_id);
            return BtreeStatus::Success;
        } else if mod_cp_id > cur_cp_id {
            // Asking for older CP - not possible
            eprintln!("COWBtree: CP mismatch for node {} (mod_cp={}, cur_cp={})", 
                      node.node_id(), mod_cp_id, cur_cp_id);
            return BtreeStatus::CpMismatch;
        }
        
        // Need to COW: create new buffer copy
        println!("COWBtree: COW node {} from CP {} to CP {}", 
                 node.node_id(), mod_cp_id, cur_cp_id);
        
        let old_buf = node.share_phys_node_buf();
        let new_buf = (*old_buf).clone();
        node.set_phys_node_buf(new_buf);
        
        // Add to dirty list
        self.add_to_dirty_list(FlushNodeInfo::new(node.clone()), cp_ctx);
        node.set_modified_cp_id(cur_cp_id);
        
        BtreeStatus::Success
    }
    
    /// Remove node (add to deleted list)
    /// 
    /// Marks a node for deletion by adding it to the checkpoint's deleted list.
    /// The node is removed from cache immediately, but its blocks are only
    /// freed during checkpoint flush.
    /// 
    /// # Arguments
    /// * `node` - Node to remove
    /// * `cp_ctx` - Checkpoint context
    pub fn remove_node(&self, node: &BtreeNodePtr, cp_ctx: &CPContext) {
        let node_id = node.node_id();
        let compact_id = Self::to_compact_nodeid(node_id);
        
        println!("COWBtree: Removing node {} in CP {}", node_id, cp_ctx.id());
        
        // Add to deleted list
        self.add_to_remove_list(compact_id, cp_ctx);
        
        // Remove from cache
        self.node_cache.remove(&node_id);
    }
    
    /// Flush dirty nodes during checkpoint
    /// 
    /// Writes all modified nodes in the current checkpoint session to disk:
    /// 1. For each dirty node, allocate a new block
    /// 2. Write the node buffer to the allocated block
    /// 3. Update the node ID -> block ID mapping
    /// 
    /// # Arguments
    /// * `cp_ctx` - Checkpoint context
    /// 
    /// # Returns
    /// Result indicating success or I/O error
    pub async fn flush_nodes(&self, cp_ctx: &CPContext) -> Result<(), std::io::Error> {
        let session = self.cp_session(cp_ctx.id());
        let modified_nodes = session.get_modified_nodes();
        
        if modified_nodes.is_empty() {
            println!("COWBtree: No dirty nodes to flush for CP {}", cp_ctx.id());
            return Ok(());
        }
        
        println!("COWBtree: Flushing {} dirty nodes for CP {}", 
                 modified_nodes.len(), cp_ctx.id());
        
        // For each dirty node, allocate blocks and write
        for finfo in modified_nodes.into_iter() {
            let node = &finfo.node;
            let node_id = node.node_id();
            let compact_id = Self::to_compact_nodeid(node_id);
            
            // Allocate a block from node_vdev
            // Note: FixedBlkStreamVdev uses append() rather than alloc + write
            
            // Create IOBuffer and copy node data (use actual node size)
            let node_size = finfo.node.node_size();
            let mut buf = IOBuffer::new(node_size as usize);
            buf.as_mut_slice().copy_from_slice(finfo.bytes());
            
            // Use segment 0 for simplicity (can be parameterized later)
            let segment_id = 0;
            let session_id = (cp_ctx.id() % MAX_CP_SESSIONS as i64) as u64;
            let blkid = self.node_vdev.append(session_id, segment_id, buf)
                .await
                .map_err(|e| {
                    eprintln!("COWBtree: Node write failed for node {}: {}", node_id, e);
                    e
                })?;
            
            println!("COWBtree: Wrote node {} to blk {:?}", node_id, blkid);
            
            // Update map (outside of loop for efficiency, but done here for simplicity)
            let mut map = self.bnodeid_map.write();
            
            // Free old block if exists
            if let Some(old_cblk) = map.get(&compact_id) {
                if let Some(old_blkid) = old_cblk.to_blkid() {
                    println!("COWBtree: Freeing old block {:?} for node {}", old_blkid, node_id);
                    // TODO: FixedBlkStreamVdev needs a free_blk method
                    // self.node_vdev.free_blk(&old_blkid);
                }
            }
            
            map.insert(compact_id, CompactBlkId::from_blkid(&blkid));
        }
        
        println!("COWBtree: Flush complete for CP {}", cp_ctx.id());
        Ok(())
    }
    
    /// Delete nodes from map (called after flush)
    /// 
    /// Processes the deleted nodes list from the checkpoint session:
    /// 1. Remove each node from the node ID -> block ID map
    /// 2. Free the corresponding blocks
    /// 3. Unreserve the node ID for reuse
    /// 
    /// This is called after flush_nodes to finalize deletions.
    pub async fn delete_nodes(&self, cp_ctx: &CPContext) {
        let session = self.cp_session(cp_ctx.id());
        let deleted_nodes = session.get_deleted_nodes();
        
        if deleted_nodes.is_empty() {
            return;
        }
        
        println!("COWBtree: Deleting {} nodes for CP {}", deleted_nodes.len(), cp_ctx.id());
        
        let mut map = self.bnodeid_map.write();
        for compact_id in deleted_nodes {
            if let Some(cblk) = map.remove(&compact_id) {
                if let Some(blkid) = cblk.to_blkid() {
                    println!("COWBtree: Freeing block {:?} for deleted node {}", blkid, compact_id);
                    // TODO: FixedBlkStreamVdev needs a free_blk method
                    // self.node_vdev.free_blk(&blkid);
                }
            }
            
            // Unreserve the node ID for reuse
            self.node_num_reserver.unreserve(compact_id as u64).await;
            println!("COWBtree: Unreserved node ID {} for reuse", compact_id);
        }
    }
    
    /// Complete checkpoint - clear session
    /// 
    /// Cleans up the checkpoint session after all operations are complete.
    pub fn finish_checkpoint(&self, cp_ctx: &CPContext) {
        let session = self.cp_session(cp_ctx.id());
        session.clear();
        println!("COWBtree: Finished checkpoint {}", cp_ctx.id());
    }
    
    // Helper methods
    
    /// Get checkpoint session for a given CP ID
    fn cp_session(&self, cp_id: i64) -> &Arc<CPSession> {
        let idx = (cp_id % 2) as usize;
        &self.cp_sessions[idx]
    }
    
    /// Add node to dirty list for current checkpoint
    fn add_to_dirty_list(&self, finfo: FlushNodeInfo, cp_ctx: &CPContext) {
        let session = self.cp_session(cp_ctx.id());
        session.add_modified_node(finfo);
    }
    
    /// Add node to remove list for current checkpoint
    fn add_to_remove_list(&self, node_id: CompactNodeId, cp_ctx: &CPContext) {
        let session = self.cp_session(cp_ctx.id());
        session.add_deleted_node(node_id);
    }
    
    /// Get root node ID
    pub fn root_node_id(&self) -> BNodeId {
        **self.root_node_id.load()
    }
    
    /// Set root node ID
    pub fn set_root_node_id(&self, node_id: BNodeId, cp_ctx: Option<&CPContext>) {
        self.root_node_id.store(Arc::new(node_id));
        
        if let Some(ctx) = cp_ctx {
            let session = self.cp_session(ctx.id());
            session.set_new_root_id(node_id);
        }
        
        println!("COWBtree: Set root node ID to {}", node_id);
    }
    
    /// Get interior node size
    pub fn interior_node_size(&self) -> u32 {
        Tunables::INTERIOR_NODE_SIZE
    }
    
    /// Get leaf node size
    pub fn leaf_node_size(&self) -> u32 {
        Tunables::LEAF_NODE_SIZE
    }
    
    /// Get btree ordinal
    pub fn ordinal(&self) -> u16 {
        self.btree_ordinal
    }
    
    /// Get cache statistics
    pub fn cache_stats(&self) -> (usize, usize) {
        let cached = self.cache.len();
        let mapped = self.bnodeid_map.read().len();
        (cached, mapped)
    }
}

impl std::fmt::Debug for COWBtree {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let (cached, mapped) = self.cache_stats();
        f.debug_struct("COWBtree")
            .field("name", &self.name)
            .field("ordinal", &self.btree_ordinal)
            .field("interior_node_size", &Tunables::INTERIOR_NODE_SIZE)
            .field("leaf_node_size", &Tunables::LEAF_NODE_SIZE)
            .field("regular_id_prefix", &format_args!("0x{:016x}", self.regular_id_prefix))
            .field("overflow_id_prefix", &format_args!("0x{:016x}", self.overflow_id_prefix))
            .field("root_node_id", &self.root_node_id())
            .field("cached_nodes", &cached)
            .field("mapped_nodes", &mapped)
            .field("full_map_vdev_id", &self.full_map_vdev.vdev_id())
            .field("incr_map_vdev_id", &self.incr_map_vdev.vdev_id())
            .field("node_vdev_id", &self.node_vdev.vdev_id())
            .finish()
    }
}
