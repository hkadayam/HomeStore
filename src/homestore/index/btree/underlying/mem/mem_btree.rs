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
 ****************************** */

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
//! HashMap<BNodeId, Arc<NodeCore>> (in-memory)
//! ```
//!
//! ## Implementation Strategy
//! Once MemBtree validates all locking scenarios, COWBtree can be implemented
//! with the same UnderlyingBtree interface but with actual persistence.

use dashmap::DashMap;
use triomphe::Arc as TArc;
use std::sync::atomic::{AtomicU64, Ordering};
use async_trait::async_trait;

use crate::index::btree::btree_node::{BNodeId, NodeCore, Node};
use crate::index::btree::btree_types::{BtreeError, BtreeConfig};
use crate::index::btree::btree::{Btree, UnderlyingBtree};
use crate::index::btree::btree_kvs::{BtreeKey, BtreeValue};
use iomgr::IOBuffer;

//================================================================================
// Phase 8: MemBtree - In-Memory Storage Implementation
//================================================================================

/// MemBtree - Simple in-memory storage for btree nodes
///
/// **NO LOCKING** - All locking is handled by the Btree layer above.
/// This layer handles ONLY storage/persistence (in-memory DashMap).
///
/// Uses DashMap for lock-free concurrent access without global lock contention.
pub struct MemBtree {
    /// In-memory node storage - concurrent HashMap without global lock
    nodes: DashMap<BNodeId, TArc<NodeCore>>,

    /// Overflow storage - separate from regular nodes
    /// Stores pure user data (no btree headers)
    overflow: DashMap<BNodeId, TArc<IOBuffer>>,

    /// Node size for this btree (fixed size for all nodes)
    node_size: u32,

    /// Next node ID to allocate (atomic for thread-safety)
    next_node_id: AtomicU64,
}

impl MemBtree {
    /// Create a new MemBtree with the given node size
    ///
    /// # Arguments
    /// * `node_size` - Size of each node in bytes (e.g., 4096)
    pub fn new(node_size: u32) -> Self {
        tracing::info!("MemBtree: Creating in-memory btree with node_size={}", node_size);

        Self {
            nodes: DashMap::new(),
            overflow: DashMap::new(),
            node_size,
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

#[async_trait]
impl UnderlyingBtree for MemBtree {
    /// Read node from memory - returns UNLOCKED node
    ///
    /// **Locking is Btree layer's responsibility** - this just fetches from HashMap.
    ///
    /// # Arguments
    /// * `id` - Node ID to read
    ///
    /// # Returns
    /// * `Ok(Arc<NodeCore>)` - Unlocked node (Btree layer will lock it)
    /// * `Err(BtreeError::NodeNotFound)` - Node doesn't exist
    async fn read_node(&self, id: BNodeId) -> Result<TArc<NodeCore>, BtreeError> {
        self.nodes.get(&id).map(|entry| TArc::clone(entry.value())).ok_or(BtreeError::NodeNotFound)
    }

    /// Write node to storage
    ///
    /// For MemBtree, this is a no-op since nodes are already in memory.
    /// Node modifications happen in-place on the buffer - nothing to persist.
    ///
    /// **Note**: The node is already locked by Btree layer before this call.
    ///
    /// # Arguments
    /// * `node` - Locked node to write (ignored for MemBtree)
    ///
    /// # Returns
    /// * `Ok(())` - Always succeeds (no-op)
    async fn write_node(&self, _node: &Node) -> Result<(), BtreeError> {
        // For MemBtree, nodes are already in memory, no-op
        // Node modifications happen in-place on the buffer
        Ok(())
    }

    /// Create new node in memory
    ///
    /// Creates a new node, allocating node_id internally, and adds it to the HashMap.
    /// Returns UNLOCKED node that Btree layer will lock as needed.
    ///
    /// # Arguments
    /// * `is_leaf` - Whether this is a leaf node
    /// * `node_variant` - Node type (0=SimpleNode, 1=VarKeyNode)
    ///
    /// # Returns
    /// * `Ok(Arc<NodeCore>)` - Unlocked new node
    async fn create_node(&self, is_leaf: bool, node_variant: u8) -> Result<TArc<NodeCore>, BtreeError> {
        // Allocate new node ID
        let node_id = self.allocate_node_id();
        let core = TArc::new(NodeCore::new(node_id, is_leaf, self.node_size));
        self.nodes.insert(node_id, TArc::clone(&core));

        tracing::info!("MemBtree: Created node {} (is_leaf={}, node_variant={})", node_id, is_leaf, node_variant);

        Ok(core)
    }

    /// Delete node from memory (for cleanup)
    ///
    /// Removes node from HashMap. Used for cleanup operations.
    ///
    /// # Arguments
    /// * `id` - Node ID to delete
    ///
    /// # Returns
    /// * `Ok(())` - Node deleted (or didn't exist)
    async fn delete_node(&self, id: BNodeId) -> Result<(), BtreeError> {
        self.nodes.remove(&id);
        tracing::info!("MemBtree: Deleted node {}", id);
        Ok(())
    }

    /// Notify storage that root node has changed
    ///
    /// For MemBtree, this is a no-op since there's no persistence.
    /// In a persistent storage implementation (e.g., COWBtree), this would
    /// persist the root node ID to metadata.
    ///
    /// # Arguments
    /// * `root_node_id` - New root node ID
    ///
    /// # Returns
    /// * `Ok(())` - Always succeeds (no-op for MemBtree)
    async fn on_root_changed(&self, root_node_id: BNodeId) -> Result<(), BtreeError> {
        tracing::info!("MemBtree: Root changed to node {}", root_node_id);
        Ok(())
    }

    /// Write overflow data to memory and return allocated node_id
    ///
    /// # Arguments
    /// * `data` - IOBuffer containing overflow data
    ///
    /// # Returns
    /// * `Ok(BNodeId)` - Allocated node ID for this overflow data
    async fn write_overflow(&self, data: IOBuffer) -> Result<BNodeId, BtreeError> {
        let node_id = self.allocate_node_id();
        let len = data.len();
        self.overflow.insert(node_id, TArc::new(data));

        tracing::debug!("MemBtree: Wrote overflow node {} ({} bytes)", node_id, len);
        Ok(node_id)
    }

    /// Read overflow data by node_id
    ///
    /// # Arguments
    /// * `node_id` - Overflow node ID to read
    ///
    /// # Returns
    /// * `Ok(Arc<IOBuffer>)` - Shared overflow data (zero-copy)
    /// * `Err(BtreeError::NodeNotFound)` - Overflow node doesn't exist
    async fn read_overflow(&self, node_id: BNodeId) -> Result<TArc<IOBuffer>, BtreeError> {
        self.overflow.get(&node_id).map(|entry| TArc::clone(entry.value())).ok_or(BtreeError::NodeNotFound)
    }

    /// Delete overflow node from memory
    ///
    /// # Arguments
    /// * `node_id` - Overflow node ID to delete
    ///
    /// # Returns
    /// * `Ok(())` - Overflow node deleted (or didn't exist)
    async fn delete_overflow(&self, node_id: BNodeId) -> Result<(), BtreeError> {
        self.overflow.remove(&node_id);
        tracing::debug!("MemBtree: Deleted overflow node {}", node_id);
        Ok(())
    }
}

//================================================================================
// Helper for Btree Construction
//================================================================================

impl MemBtree {
    /// Create a new Btree with MemBtree storage (for testing)
    ///
    /// This is a convenience method that creates both the storage and the Btree.
    ///
    /// # Arguments
    /// * `node_size` - Size of each node in bytes
    ///
    /// # Returns
    /// * Result with Btree and MemBtree storage, or BtreeError
    pub async fn create_btree<K, V>(node_size: u32) -> Result<Btree<K, V>, BtreeError>
    where
        K: BtreeKey + 'static,
        V: BtreeValue + 'static,
    {
        let storage = Box::new(Self::new(node_size));
        let config = BtreeConfig::new(node_size, "mem_btree".to_string());
        Btree::new(config, storage, None).await
    }
}

//================================================================================
// Usage Examples (for documentation)
//================================================================================

/// Example: Create and use MemBtree
///
/// ```ignore
/// // Create MemBtree storage
/// let mem_btree = MemBtree::new(4096);
///
/// // Create Btree with MemBtree storage
/// let btree = Btree::<u64, u64>::new(Box::new(mem_btree), root_node_id);
///
/// // Or use convenience method
/// let btree = MemBtree::create_btree::<u64, u64>(4096, root_node_id);
///
/// // ... operations ...
pub(crate) mod examples {}
