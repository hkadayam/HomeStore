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

use std::collections::HashMap;
use std::sync::{RwLock, Arc};
use std::sync::atomic::{AtomicU64, Ordering};
use async_trait::async_trait;

use crate::index::btree::btree_node::{BNodeId, NodeCore};
use crate::index::btree::btree::{UnderlyingBtree, BtreeError};

//================================================================================
// Phase 8: MemBtree - In-Memory Storage Implementation
//================================================================================

/// MemBtree - Simple in-memory storage for btree nodes
///
/// **NO LOCKING** - All locking is handled by the Btree layer above.
/// This layer handles ONLY storage/persistence (in-memory HashMap).
///
/// Uses `std::sync::RwLock` (not AsyncRwLock) since HashMap operations are fast.
pub struct MemBtree {
    /// In-memory node storage (HashMap wrapped in RwLock for interior mutability)
    /// Using std::sync::RwLock (NOT AsyncRwLock) since HashMap ops are fast
    nodes: RwLock<HashMap<BNodeId, Arc<NodeCore>>>,

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
            nodes: RwLock::new(HashMap::new()),
            node_size,
            next_node_id: AtomicU64::new(1), // Start from 1
        }
    }

    /// Allocate a new node ID
    fn allocate_node_id(&self) -> BNodeId {
        self.next_node_id.fetch_add(1, Ordering::SeqCst)
    }

    /// Get node count (for debugging/testing)
    pub fn node_count(&self) -> usize {
        self.nodes.read().unwrap().len()
    }

    /// Clear all nodes (for testing)
    pub fn clear(&self) {
        self.nodes.write().unwrap().clear();
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
    async fn read_node(&self, id: BNodeId) -> Result<Arc<NodeCore>, BtreeError> {
        let nodes = self.nodes.read().unwrap();
        nodes.get(&id)
            .cloned()
            .ok_or(BtreeError::NodeNotFound)
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
    async fn write_node(&self, _node: &crate::index::btree::btree_node::Node) -> Result<(), BtreeError> {
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
    async fn create_node(&self, is_leaf: bool, node_variant: u8) -> Result<Arc<NodeCore>, BtreeError> {
        // Storage layer: allocate ID, create buffer, store in HashMap
        // btree_node_mgr::init_new_variant_node() sets node_variant and initializes
        
        // Allocate new node ID
        let node_id = self.allocate_node_id();

        // Create new node buffer
        let core = Arc::new(NodeCore::new(node_id, is_leaf, self.node_size));

        // Add to in-memory storage
        let mut nodes = self.nodes.write().unwrap();
        nodes.insert(node_id, Arc::clone(&core));

        tracing::info!("MemBtree: Created node {} (is_leaf={}, node_variant={})",
                 node_id, is_leaf, node_variant);

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
        let mut nodes = self.nodes.write().unwrap();
        nodes.remove(&id);

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
    pub async fn create_btree<K, V>(node_size: u32) -> Result<crate::index::btree::btree::Btree<K, V>, crate::index::btree::btree::BtreeError>
    where
        K: crate::index::btree::btree_kvs::BtreeKey + 'static,
        V: crate::index::btree::btree_kvs::BtreeValue + 'static,
    {
        let storage = Box::new(Self::new(node_size));
        let config = crate::index::btree::btree::BtreeConfig::new(node_size, "mem_btree".to_string());
        crate::index::btree::btree::Btree::new(config, storage, None).await
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
/// // Use btree normally
/// let _tree_lock = btree.lock_tree_shared().await;
/// let node = btree.read_and_lock_node(node_id, LockType::Read).await?;
/// // ... operations ...
/// ```
///
/// Example: Testing locking scenarios with MemBtree
///
/// ```ignore
/// // Multi-threaded GET operations (all READ locks)
/// let btree = Arc::new(MemBtree::create_btree::<u64, u64>(4096, root_id));
///
/// let handles: Vec<_> = (0..10).map(|i| {
///     let btree = Arc::clone(&btree);
///     tokio::spawn(async move {
///         let _tree_guard = btree.lock_tree_shared().await;
///         let node = btree.read_and_lock_node(node_id, LockType::Read).await?;
///         // Concurrent readers - should all succeed
///         let value = node.get_nth_value::<u64>(i);
///         Ok::<_, BtreeError>(value)
///     })
/// }).collect();
///
/// // Wait for all to complete
/// for handle in handles {
///     handle.await??;
/// }
/// ```
pub(crate) mod examples {}