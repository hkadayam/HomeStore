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
 ****************** */

//! MemBtree - Simple In-Memory Storage for Btree Validation
//!
//! This is a simple, in-memory storage backend for btree nodes. It's implemented
//! FIRST (before COWBtree) to validate the entire locking architecture with a
//! minimal storage layer.
//!
//! ## Purpose
//! - Validate locking logic without persistence complexity
//! - Fast iteration for fixing locking bugs
//! - Prove locking correct before adding persistence
//!
//! ## Architecture
//! ```
//! Btree (locking layer)
//!   ↓
//! MemBtree (this - simple storage, NO locking)
//!   ↓
//! NodeMap<V> (DashMap for concurrent, lock-free HashMap for single-threaded)
//! ```
//!
//! ## Implementation Strategy
//! Once MemBtree validates all locking scenarios, COWBtree can be implemented
//! with the same UnderlyingBtree interface but with actual persistence.

use dashmap::DashMap;
use triomphe::Arc as TArc;
use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};

#[cfg(feature = "async_code")]
use async_trait::async_trait;

use crate::index::btree::btree_node::{BNodeId, NodeCore, Node};
use crate::index::btree::btree_types::{BtreeError, BtreeConfig, BtreeBuffer};
use crate::index::btree::btree::{Btree, UnderlyingBtree};
use crate::index::btree::btree_kvs::{BtreeKey, BtreeValue};

//================================================================================
// NodeMap - Dual-mode storage: concurrent DashMap or lock-free HashMap
//================================================================================

/// Dual-mode storage map.
/// - `Concurrent`: DashMap (lock-free sharded, multi-threaded safe)
/// - `LockFree`: plain HashMap behind UnsafeCell (zero overhead, single-threaded only)
enum NodeMap<V: Clone + Send + 'static> {
    Concurrent(DashMap<BNodeId, V>),
    LockFree(UnsafeCell<HashMap<BNodeId, V>>),
}

// SAFETY: The LockFree variant is only constructed when BtreeConfig::is_single_threaded=true,
// which guarantees no concurrent access to the UnsafeCell. The Concurrent variant uses
// DashMap which is already Sync.
unsafe impl<V: Clone + Send + 'static> Sync for NodeMap<V> {}
unsafe impl<V: Clone + Send + 'static> Send for NodeMap<V> {}

impl<V: Clone + Send + 'static> NodeMap<V> {
    fn concurrent() -> Self { Self::Concurrent(DashMap::new()) }
    fn lockfree() -> Self { Self::LockFree(UnsafeCell::new(HashMap::new())) }

    fn get(&self, id: BNodeId) -> Option<V> {
        match self {
            Self::Concurrent(m) => m.get(&id).map(|r| r.value().clone()),
            Self::LockFree(m)   => unsafe { &*m.get() }.get(&id).map(|v| v.clone()),
        }
    }

    fn insert(&self, id: BNodeId, val: V) {
        match self {
            Self::Concurrent(m) => { m.insert(id, val); }
            Self::LockFree(m)   => { unsafe { &mut *m.get() }.insert(id, val); }
        }
    }

    fn remove(&self, id: BNodeId) {
        match self {
            Self::Concurrent(m) => { m.remove(&id); }
            Self::LockFree(m)   => { unsafe { &mut *m.get() }.remove(&id); }
        }
    }

    fn len(&self) -> usize {
        match self {
            Self::Concurrent(m) => m.len(),
            Self::LockFree(m)   => unsafe { &*m.get() }.len(),
        }
    }

    fn clear(&self) {
        match self {
            Self::Concurrent(m) => m.clear(),
            Self::LockFree(m)   => unsafe { &mut *m.get() }.clear(),
        }
    }
}

//================================================================================
// MemBtree - In-Memory Storage Implementation
//================================================================================

/// MemBtree - Simple in-memory storage for btree nodes
///
/// **NO LOCKING** - All locking is handled by the Btree layer above.
/// This layer handles ONLY storage/persistence (in-memory map).
///
/// When `is_single_threaded=true`, uses a lock-free `HashMap` (zero overhead).
/// When `is_single_threaded=false`, uses `DashMap` for concurrent access.
pub struct MemBtree {
    /// In-memory node storage
    nodes: NodeMap<TArc<NodeCore>>,

    /// Overflow storage - separate from regular nodes
    /// Stores pure user data (no btree headers)
    overflow: NodeMap<TArc<BtreeBuffer>>,

    /// Node size for this btree (fixed size for all nodes)
    node_size: u32,

    /// Btree name (from config) for logging and debugging
    btree_name: String,

    /// Next node ID to allocate (atomic for thread-safety)
    next_node_id: AtomicU64,
}

impl MemBtree {
    /// Create a new MemBtree from btree config
    ///
    /// Uses config for node size, btree name, and concurrency mode.
    ///
    /// # Arguments
    /// * `config` - Btree configuration (node_size, btree_name, is_single_threaded, etc.)
    pub fn new(config: &BtreeConfig) -> Self {
        tracing::info!(
            "MemBtree: Creating in-memory btree name={} node_size={} single_threaded={}",
            config.btree_name, config.node_size, config.is_single_threaded
        );

        let st = config.is_single_threaded;
        Self {
            nodes:    if st { NodeMap::lockfree() } else { NodeMap::concurrent() },
            overflow: if st { NodeMap::lockfree() } else { NodeMap::concurrent() },
            node_size: config.node_size,
            btree_name: config.btree_name.clone(),
            next_node_id: AtomicU64::new(1), // Start from 1
        }
    }

    /// Allocate a new node ID
    fn allocate_node_id(&self) -> BNodeId { self.next_node_id.fetch_add(1, Ordering::SeqCst) }

    /// Get node count (for debugging/testing)
    pub fn node_count(&self) -> usize { self.nodes.len() }

    /// Get overflow node count (for debugging/testing)
    pub fn overflow_node_count(&self) -> usize { self.overflow.len() }

    /// Clear all nodes and overflow (for testing)
    pub fn clear(&self) {
        self.nodes.clear();
        self.overflow.clear();
    }
}

#[cfg_attr(feature = "async_code", async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
impl UnderlyingBtree for MemBtree {
    /// Read node from memory - returns UNLOCKED node
    ///
    /// **Locking is Btree layer's responsibility** - this just fetches from the map.
    ///
    /// # Arguments
    /// * `id` - Node ID to read
    ///
    /// # Returns
    /// * `Ok(Arc<NodeCore>)` - Unlocked node (Btree layer will lock it)
    /// * `Err(BtreeError::NodeNotFound)` - Node doesn't exist
    async fn read_node(&self, id: BNodeId) -> Result<TArc<NodeCore>, BtreeError> {
        self.nodes.get(id).ok_or(BtreeError::NodeNotFound)
    }

    /// Write node to storage
    ///
    /// For MemBtree, this is a no-op since nodes are already in memory.
    /// Node modifications happen in-place on the buffer - nothing to persist.
    async fn write_node(&self, _node: &Node) -> Result<(), BtreeError> {
        Ok(())
    }

    /// Create new node in memory
    ///
    /// Creates a new node, allocating node_id internally, and adds it to the map.
    /// Returns UNLOCKED node that Btree layer will lock as needed.
    async fn create_node(&self, is_leaf: bool, _node_variant: u8) -> Result<TArc<NodeCore>, BtreeError> {
        let node_id = self.allocate_node_id();
        let core = TArc::new(NodeCore::new(node_id, is_leaf, self.node_size));
        self.nodes.insert(node_id, TArc::clone(&core));
        Ok(core)
    }

    /// Delete node from memory (for cleanup)
    async fn delete_node(&self, id: BNodeId) -> Result<(), BtreeError> {
        self.nodes.remove(id);
        Ok(())
    }

    /// Notify storage that root node has changed
    ///
    /// For MemBtree, this is a no-op since there's no persistence.
    async fn on_root_changed(&self, root_node_id: BNodeId) -> Result<(), BtreeError> {
        tracing::debug!("MemBtree: Root changed to node {}", root_node_id);
        Ok(())
    }

    /// Write overflow data to memory and return allocated node_id
    async fn write_overflow(&self, data: BtreeBuffer) -> Result<BNodeId, BtreeError> {
        let node_id = self.allocate_node_id();
        let len = data.len();
        self.overflow.insert(node_id, TArc::new(data));
        tracing::debug!("MemBtree: Wrote overflow node {} ({} bytes)", node_id, len);
        Ok(node_id)
    }

    /// Read overflow data by node_id
    async fn read_overflow(&self, node_id: BNodeId) -> Result<TArc<BtreeBuffer>, BtreeError> {
        self.overflow.get(node_id).ok_or(BtreeError::NodeNotFound)
    }

    /// Delete overflow node from memory
    async fn delete_overflow(&self, node_id: BNodeId) -> Result<(), BtreeError> {
        self.overflow.remove(node_id);
        tracing::debug!("MemBtree: Deleted overflow node {}", node_id);
        Ok(())
    }
}

//================================================================================
// Helper for Btree Construction
//================================================================================

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
impl MemBtree {
    /// Create a new Btree with MemBtree storage (for testing)
    ///
    /// # Arguments
    /// * `node_size` - Size of each node in bytes
    ///
    /// # Returns
    /// * Result with Btree, or BtreeError
    pub async fn create_btree<K, V>(node_size: u32) -> Result<Btree<K, V>, BtreeError>
    where
        K: BtreeKey + 'static,
        V: BtreeValue + 'static,
    {
        let config = BtreeConfig::new(node_size, "mem_btree".to_string());
        let storage = Box::new(Self::new(&config));
        Btree::new(config, storage, None).await
    }
}

//================================================================================
// Usage Examples (for documentation)
//================================================================================

/// Example: Create and use MemBtree
///
/// ```ignore
/// // Create MemBtree storage (config supplies node_size and btree name for logs)
/// let config = BtreeConfig::new(4096, "my_btree".to_string());
/// let mem_btree = MemBtree::new(&config);
///
/// // Create Btree with MemBtree storage
/// let btree = Btree::<u64, u64>::new(Box::new(mem_btree), root_node_id);
///
/// // Or use convenience method
/// let btree = MemBtree::create_btree::<u64, u64>(4096, root_node_id);
///
/// // ... operations ...
pub(crate) mod examples {}
