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
 ************************************************************************ */

//! Btree Node Manager - Lock upgrade and node management operations
//!
//! This module corresponds to btree_node_mgr.ipp in the C++ implementation.
//! It contains:
//! - Lock upgrade functions (standalone)
//! - Node management methods (additional impl block for Btree)

use super::btree_node::{Node, LockType, InternalLockGuard, BNodeId, NodeOps, NodeCore};
use super::btree_node::{SIMPLE_NODE_OPS, VAR_KEY_NODE_OPS, VAR_VALUE_NODE_OPS, VAR_OBJ_NODE_OPS};
use super::btree_kvs::{BtreeKey, BtreeValue};
use super::btree_types::BtreeError;
use super::btree::Btree;
use triomphe::Arc as TArc;

//================================================================================
// Btree Node Management Methods
//================================================================================

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    //================================================================================
    // Tree-level Locking
    //================================================================================

    /// Acquire shared tree lock (for normal operations)
    ///
    /// **MUST hold returned guard for entire operation!**
    ///
    /// Used for normal operations (GET/PUT/REMOVE) that don't modify root.
    /// In single-threaded mode, returns a no-op guard (no lock acquired).
    pub(super) async fn lock_tree_shared(&self) -> super::btree::TreeLockGuard<'_> {
        if self.config.is_single_threaded {
            super::btree::TreeLockGuard { _guard: None }
        } else {
            super::btree::TreeLockGuard { _guard: Some(self.btree_lock.read().await) }
        }
    }

    /// Acquire exclusive tree lock (for root split/collapse)
    ///
    /// **MUST hold returned guard for entire operation!**
    ///
    /// Used for operations that modify the root node (split/collapse).
    /// In single-threaded mode, returns a no-op guard (no lock acquired).
    pub(super) async fn lock_tree_exclusive(&self) -> super::btree::TreeLockGuardExclusive<'_> {
        if self.config.is_single_threaded {
            super::btree::TreeLockGuardExclusive { _guard: None }
        } else {
            super::btree::TreeLockGuardExclusive { _guard: Some(self.btree_lock.write().await) }
        }
    }

    //================================================================================
    // Single-threaded Locking Helpers
    //================================================================================

    /// Create a Node with InternalLockGuard::None (single-threaded pass-through).
    ///
    /// In single-threaded mode there is no concurrent access, so we skip actual
    /// lock acquisition and return a Node with an empty guard.
    ///
    /// Resolves `ReadInteriorWriteLeaf` to the actual lock type based on the node's
    /// leaf status, matching the behaviour of `NodeCore::lock()`.
    fn make_lockless_node(core: TArc<NodeCore>, lock_type: LockType) -> Node {
        let actual_lock = match lock_type {
            LockType::ReadInteriorWriteLeaf => {
                if core.is_leaf() { LockType::Write } else { LockType::Read }
            }
            other => other,
        };
        Node { _guard: InternalLockGuard::None, core, lock_type: actual_lock }
    }

    /// Lock a node, or skip locking entirely when the btree is single-threaded.
    async fn lock_node(&self, core: TArc<NodeCore>, lock_type: LockType) -> Node {
        if self.config.is_single_threaded {
            Self::make_lockless_node(core, lock_type)
        } else {
            NodeCore::lock(core, lock_type).await
        }
    }

    /// Clone a temporary copy of a node, with or without locking.
    ///
    /// In single-threaded mode the copy is created without acquiring any lock.
    pub(crate) async fn clone_temp_node(&self, node: &Node, lock_type: LockType) -> Node {
        if self.config.is_single_threaded {
            let buffer = node.core.get_phys_buf().as_ref().to_vec();
            let temp_core = TArc::new(NodeCore::from_buffer(buffer));
            Self::make_lockless_node(temp_core, lock_type)
        } else {
            node.clone_temp(lock_type).await
        }
    }

    //================================================================================
    // Node Reading and Locking
    //================================================================================

    /// Get root node ID (must be called under tree lock)
    pub(crate) fn root_node_id(&self) -> BNodeId { self.root_node_id.load(std::sync::atomic::Ordering::Relaxed) }

    /// Read node from storage and lock it (matches C++ read_and_lock_node)
    ///
    /// This is the main entry point for getting locked nodes.
    /// Implementation in btree_node_mgr.rs (like C++ btree_node_mgr.ipp).
    ///
    /// # Process
    /// 1. Get UNLOCKED node from storage (storage handles persistence only)
    /// 2. Lock it based on lock_type (node manager handles locking)
    /// 3. Return locked Node guard
    ///
    /// # Arguments
    /// * `id` - Node ID to read
    /// * `lock_type` - Type of lock to acquire (Read/Write/ReadInteriorWriteLeaf)
    ///
    /// # Returns
    /// * Locked Node guard
    pub async fn read_and_lock_node(&self, id: BNodeId, lock_type: LockType) -> Result<Node, BtreeError> {
        // Get UNLOCKED node from storage (storage layer handles persistence only)
        let node_core = self.storage.read_node(id).await?;

        // Lock it (or pass-through if single-threaded)
        Ok(self.lock_node(node_core, lock_type).await)
    }

    pub(crate) async fn create_new_node(&self, is_leaf: bool, node_variant: u8) -> Result<Node, BtreeError> {
        if is_leaf {
            self.create_leaf_node(node_variant).await
        } else {
            self.create_interior_node(node_variant).await
        }
    }

    /// Create a new leaf node
    pub(crate) async fn create_leaf_node(&self, node_variant: u8) -> Result<Node, BtreeError> {
        let node_core = self.storage.create_node(true, node_variant).await?;
        self.init_new_variant_node(node_core, node_variant).await
    }

    /// Create a new interior node
    pub(crate) async fn create_interior_node(&self, node_variant: u8) -> Result<Node, BtreeError> {
        let node_core = self.storage.create_node(false, node_variant).await?;
        self.init_new_variant_node(node_core, node_variant).await
    }

    /// Get child node and lock it (common pattern in tree traversal)
    ///
    /// Convenience method that reads the child ID from parent and locks the child.
    ///
    /// # Arguments
    /// * `parent` - Parent node (already locked)
    /// * `idx` - Child index in parent
    /// * `lock_type` - Type of lock for child
    ///
    /// # Returns
    /// * Locked child Node guard
    pub async fn get_child_and_lock(&self, parent: &Node, idx: u32, lock_type: LockType) -> Result<Node, BtreeError> {
        let child_id = if idx == parent.total_entries() {
            debug_assert!(parent.has_valid_edge(), "Child index {} does not have valid bnode_id", idx);
            parent.get_edge_value()
        } else {
            debug_assert!(idx < parent.total_entries(), "Index {} >= total_entries {}", idx, parent.total_entries());
            parent
                .get_nth_value::<K, BNodeId>(idx, /* copy= */ false)
                .expect_inline("Interior nodeid cannot be overflow references")
        };

        self.read_and_lock_node(child_id, lock_type).await
    }

    /// Upgrade single node lock from READ to WRITE (matches C++ upgrade_node_lock)
    ///
    /// In single-threaded mode, skips the drop-and-reacquire entirely.
    ///
    /// # Process (multi-threaded)
    /// 1. Drop current lock
    /// 2. Reacquire WRITE lock
    /// 3. Validate node wasn't modified (check node_gen and deleted flag)
    /// 4. Return Retry error if validation fails
    pub async fn upgrade_node_lock(&self, guard: Node) -> Result<Node, BtreeError> {
        // Single-threaded: no concurrent modification possible, skip drop/re-acquire/validate.
        if self.config.is_single_threaded {
            return Ok(Self::make_lockless_node(guard.core, LockType::Write));
        }

        let core = guard.core().clone();
        let prev_gen = core.node_gen();

        // Drop current lock
        drop(guard);

        // Acquire WRITE lock
        let write_guard = core.lock.write().await;

        // Validate node wasn't modified
        if core.is_node_deleted() || core.node_gen() != prev_gen {
            return Err(BtreeError::Retry);
        }

        // Create new guard with WRITE lock
        // SAFETY: Arc<NodeCore> in Node keeps the lock alive, so 'static transmute is safe
        let write_guard = unsafe { std::mem::transmute(write_guard) };
        Ok(Node {
            core,
            lock_type: LockType::Write,
            _guard: InternalLockGuard::Write(write_guard),
        })
    }

    /// Upgrade parent and child node locks to WRITE (matches C++ upgrade_node_locks)
    ///
    /// In single-threaded mode, skips the drop-and-reacquire entirely.
    ///
    /// # Process (multi-threaded)
    /// 1. Drop both current locks
    /// 2. Reacquire WRITE locks for both (parent first, then child)
    /// 3. Validate both nodes weren't modified
    /// 4. Return Retry error if either validation fails
    pub(crate) async fn upgrade_node_locks(
        &self,
        parent_guard: Node,
        child_guard: Node,
    ) -> Result<(Node, Node), BtreeError> {
        // Single-threaded: no concurrent modification possible, skip drop/re-acquire/validate.
        if self.config.is_single_threaded {
            return Ok((
                Self::make_lockless_node(parent_guard.core, LockType::Write),
                Self::make_lockless_node(child_guard.core, LockType::Write),
            ));
        }

        let parent_core = parent_guard.core().clone();
        let child_core = child_guard.core().clone();
        let parent_prev_gen = parent_core.node_gen();
        let child_prev_gen = child_core.node_gen();

        // Drop both locks
        drop(child_guard);
        drop(parent_guard);

        // Acquire WRITE locks (parent first, then child - matches C++ ordering)
        let parent_write = parent_core.lock.write().await;
        let child_write = child_core.lock.write().await;

        // Validate both nodes
        if parent_core.is_node_deleted()
            || parent_core.node_gen() != parent_prev_gen
            || child_core.is_node_deleted()
            || child_core.node_gen() != child_prev_gen
        {
            return Err(BtreeError::Retry);
        }

        // Create new guards with WRITE locks
        // SAFETY: Arc<NodeCore> in Node keeps the lock alive, so 'static transmute is safe
        let parent_write = unsafe { std::mem::transmute(parent_write) };
        let child_write = unsafe { std::mem::transmute(child_write) };

        Ok((
            Node {
                core: parent_core,
                lock_type: LockType::Write,
                _guard: InternalLockGuard::Write(parent_write),
            },
            Node {
                core: child_core,
                lock_type: LockType::Write,
                _guard: InternalLockGuard::Write(child_write),
            },
        ))
    }

    /// Initialize a newly created node based on its variant type
    /// Sets node_variant in header, calls NodeOps::init_new_node(), acquires write lock, returns Node
    pub(crate) async fn init_new_variant_node(
        &self,
        node_core: TArc<NodeCore>,
        node_variant: u8,
    ) -> Result<Node, BtreeError> {
        // Set the node variant in persistent header
        {
            let header = node_core.get_persistent_header_mut();
            header.node_variant = node_variant;
        }

        // Dispatch to appropriate NodeOps based on variant for initialization
        match node_variant {
            0 => {
                (&SIMPLE_NODE_OPS as &dyn NodeOps<u64, u64>).init_new_node(&node_core);
            }
            1 => {
                (&VAR_KEY_NODE_OPS as &dyn NodeOps<u64, u64>).init_new_node(&node_core);
            }
            2 => {
                (&VAR_VALUE_NODE_OPS as &dyn NodeOps<u64, u64>).init_new_node(&node_core);
            }
            3 => {
                (&VAR_OBJ_NODE_OPS as &dyn NodeOps<u64, u64>).init_new_node(&node_core);
            }
            4 => {
                // Create PrefixCompressNodeOps with configured expected_prefix_size
                let prefix_ops = super::variant::PrefixCompressNodeOps::new(self.config.expected_prefix_size);
                (&prefix_ops as &dyn NodeOps<u64, u64>).init_new_node(&node_core);
            }
            _ => {
                return Err(BtreeError::Io(std::io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    format!("Unknown node variant: {}", node_variant),
                )));
            }
        }

        // Lock the initialized node (or pass-through if single-threaded)
        Ok(self.lock_node(node_core, LockType::Write).await)
    }
}

//================================================================================
// Helper Functions
//================================================================================
// (None currently)
