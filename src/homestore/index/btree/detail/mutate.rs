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
 */

//! Btree Mutation Operations
//!
//! This module contains PUT, REMOVE, and other mutation operations.
//! Corresponds to btree mutate operations in C++ btree implementation.

use super::super::btree_node::{Node, BNodeId, LockType};
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree::Btree;
use super::super::btree_types::BtreeError;
use super::btree_req::{
    BtreeSinglePutRequest, BtreeRangePutRequest, BtreeRequest, BtreePutType, PutFilter, PutFilterDecision,
};

//================================================================================
// PUT Result Type
//================================================================================

/// Result of PUT operation
#[derive(Debug, PartialEq, Eq)]
pub enum PutResult {
    Success, // New key inserted
    Updated, // Existing key updated
}

//================================================================================
// Internal Implementation (called from btree.rs public API)
//================================================================================

#[maybe_async_cfg::maybe(
    keep_self,
    sync(feature = "sync_code"),
    async(feature = "async_code")
)]
impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    //================================================================================
    // Single-key PUT Implementation
    //================================================================================

    /// Single-key PUT request with retry loop
    pub(in super::super) async fn put_one_internal<'a>(
        &self,
        req: &'a BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        loop {
            let tree_lock = self.lock_tree_shared().await;
            let root_id = self.root_node_id();
            let root = self.read_and_lock_node(root_id, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&root, req) {
                drop(root);
                drop(tree_lock);
                self.check_split_root(req).await?;
                continue; // Retry from root
            }

            match self.put_one_walk(root, req).await {
                Ok(()) => return Ok(()),
                Err(BtreeError::Retry) => continue, // Retriable errors
                Err(e) => return Err(e),            // Non-retriable errors
            }
        }
    }

    /// Recursive PUT for single-key requests
    async fn put_one_walk<'a>(
        &self,
        mut my_node: Node,
        req: &'a BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        if my_node.is_leaf() {
            return self.put_one_in_leaf(&my_node, req).await;
        }

        // Interior node: find child and traverse
        loop {
            let (_, idx) = my_node.find::<K, V>(req.key());
            let mut child = self.get_child_and_lock(&my_node, idx, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&child, req) {
                tracing::debug!(parent = my_node.node_id(), child = child.node_id(), idx = idx, "Child needs split");
                // Upgrade both the locks - if it fails, retry from root
                (my_node, child) = self.upgrade_node_locks(my_node, child).await?;

                // Perform split - if it fails, propagate error to retry from root
                let _split_key = self.split_child_node(&my_node, &child, idx).await?;
                tracing::debug!(split_key=?_split_key, "Split completed, retrying");
                continue; // After successful split, retry search from this node
            }

            drop(my_node); // Can unlock the parent node now
            #[cfg(feature = "async_code")]
            return Box::pin(self.put_one_walk(child, req)).await;
            #[cfg(feature = "sync_code")]
            return self.put_one_walk(child, req);
        }
    }

    /// Write to leaf node for single-key request (matches C++ mutate_write_leaf_node lines 259-269)
    ///
    /// Now at Btree layer to support:
    /// - Async overflow value resolution for filters
    /// - Overflow value writing/cleanup
    /// - Two-phase filter execution
    async fn put_one_in_leaf<'a>(
        &self,
        node: &Node,
        req: &'a BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        use super::btree_req::{BtreePutType, PutFilterDecision};

        debug_assert!(node.is_leaf(), "Put operation on node is supported only for leaf nodes");
        tracing::trace!(node=node.node_id(), key=?req.key(), "Leaf mutation");

        let key = req.key();
        let value = req.value();
        let put_type = req.put_type();
        let filter = req.filter();

        // Find the key
        let (found, idx) = node.find::<K, V>(key);

        // If found and filter provided, apply filter with async overflow resolution
        if found {
            let decision = self.apply_put_filter(node, idx, filter).await?;
            match decision {
                PutFilterDecision::Keep => {
                    // No change needed
                    return Ok(());
                }
                PutFilterDecision::Remove => {
                    // Remove the entry
                    node.remove::<K, V>(idx)?;
                    self.storage.write_node(node).await?;
                    return Ok(());
                }
                PutFilterDecision::Replace => {
                    // Fall through to update logic below
                }
                PutFilterDecision::NeedOldValue => unreachable!(),
            }
        }

        // Dispatch based on put_type
        match put_type {
            BtreePutType::Insert => {
                if found {
                    return Err(BtreeError::KeyAlreadyExists);
                }
                // Build ValueOrOverflow, writing to overflow storage if needed
                let v = ValueOrOverflow::build(self.storage.as_ref(), value, self.config.overflow_threshold).await?;
                node.insert::<K, V>(idx, key, &v)?;
            }
            BtreePutType::Update => {
                if !found {
                    return Err(BtreeError::KeyNotFound);
                }
                self.replace_value(node, idx, value).await?;
            }
            BtreePutType::Upsert => {
                if found {
                    self.replace_value(node, idx, value).await?;
                } else {
                    let v =
                        ValueOrOverflow::build(self.storage.as_ref(), value, self.config.overflow_threshold).await?;
                    node.insert::<K, V>(idx, key, &v)?;
                }
            }
        }

        tracing::debug!(node = node.node_id(), entries = node.total_entries(), "Mutation done");
        self.storage.write_node(node).await?;
        Ok(())
    }

    //================================================================================
    // Range PUT Implementation
    //================================================================================

    /// Range PUT request with retry loop (matches C++ lines 71-78 for has_more handling)
    pub(in super::super) async fn put_range_internal<'a>(
        &self,
        mut req: BtreeRangePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        loop {
            let tree_lock = self.lock_tree_shared().await;
            let root_id = self.root_node_id();
            let root = self.read_and_lock_node(root_id, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&root, &req) {
                drop(root);
                drop(tree_lock);
                self.check_split_root(&req).await?;
                continue; // Retry from root after split
            }

            match self.put_range_walk(root, &mut req).await {
                Ok(result) => return Ok(result),
                Err(BtreeError::Retry) => continue, // Retriable errors
                Err(e) => return Err(e),            // Non-retriable errors
            }
        }
    }

    /// Main PUT recursive traversal for range operations (matches C++ Btree::do_put lines 108-243)
    async fn put_range_walk<'a>(
        &self,
        mut my_node: Node,
        req: &mut BtreeRangePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        if my_node.is_leaf() {
            return self.put_range_in_leaf(&my_node, req).await;
        }

        // Interior node: traverse to child(ren)
        'outer: loop {
            // Match the key range to find child indices (matches C++ line 130)
            // Clone the working_range to avoid holding immutable borrow of req
            let working_range = req.working_range().clone();
            let (matched, start_idx, end_idx) = my_node.match_range::<K, V>(&working_range);
            if !matched {
                return Err(BtreeError::KeyNotFound); // match_range returns 0 entries is not valid
            }

            // Iterate all matched children
            let mut curr_idx = start_idx;
            while curr_idx <= end_idx {
                let mut child = self.get_child_and_lock(&my_node, curr_idx, LockType::ReadInteriorWriteLeaf).await?;

                // Check if split needed
                if self.is_split_needed(&child, req) {
                    (my_node, child) = self.upgrade_node_locks(my_node, child).await?;
                    let _split_key = self.split_child_node(&my_node, &child, curr_idx).await?;
                    continue 'outer; // Restart from this node again after split
                }

                // Trim working range for leaf child to how much ever the node link's highest key is.
                if child.is_leaf() && curr_idx < my_node.total_entries() {
                    let child_end_key: K = my_node.get_nth_key::<K, V>(curr_idx, /* copy= */ true);
                    if child_end_key < req.working_range().end_key {
                        req.trim_working_range(child_end_key, /* end_incl= */ true);
                    }
                }

                if curr_idx == end_idx {
                    drop(my_node);
                    return self.put_range_walk(child, req).await;
                } else {
                    self.put_range_walk(child, req).await?;
                    curr_idx += 1;
                }
            }

            return Ok(());
        }
    }

    /// Write to leaf node for range operations (matches C++ lines 250-257)
    ///
    /// Combined implementation with:
    /// - Async overflow value resolution
    /// - Two-phase filtering (check_key, then check_kv)
    /// - Direct range shifting and node write
    async fn put_range_in_leaf<'a>(
        &self,
        node: &Node,
        req: &mut BtreeRangePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        let put_type = BtreePutType::Update; // Range operations always update
        debug_assert!(node.is_leaf(), "Multi put only for leaf nodes");

        // Find range of entries matching the key range
        let (matched, start_idx, end_idx) = node.match_range::<K, V>(req.working_range());
        if !matched {
            return Err(BtreeError::KeyNotFound);
        }

        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap_or(0);
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap_or(0);
        let new_value = req.value();
        let filter = req.filter();

        // Update all entries in range
        let mut idx = start_idx;
        let mut last_failed_key: Option<K> = None;

        while idx <= end_idx {
            // Check if we have room for potential update
            if !node.has_room_for_put::<K, V>(put_type, key_size, val_size) {
                last_failed_key = Some(node.get_nth_key::<K, V>(idx, /* copy= */ true));
                break;
            }

            // Apply filter if provided
            let decision = self.apply_put_filter(node, idx, filter).await?;
            match decision {
                PutFilterDecision::Keep => {
                    idx += 1;
                }
                PutFilterDecision::Replace => {
                    self.replace_value(node, idx, new_value).await?;
                    idx += 1;
                }
                PutFilterDecision::Remove => {
                    node.remove::<K, V>(idx)?;
                    // Don't increment idx - next entry shifts down
                }
                PutFilterDecision::NeedOldValue => unreachable!(),
            }
        }

        // Shift working range based on result
        req.shift_working_range(last_failed_key);

        // Write the modified node to storage (matches C++ line 268)
        self.storage.write_node(node).await?;
        Ok(())
    }

    //================================================================================
    // Split related method
    //================================================================================

    /// Check if node needs a split in case any insertion has to be done in this node.
    /// Applicable for both interior and leaf nodes. (matches C++ lines 383-393)
    fn is_split_needed<ReqT>(&self, node: &Node, req: &ReqT) -> bool
    where
        ReqT: BtreeRequest + ?Sized,
    {
        if !node.is_leaf() {
            // Interior node: size is at most one additional entry with max key and nodeid
            // Use config.max_key_size instead of K::get_max_size() for accurate sizing
            return !node.has_room_for_put::<K, V>(
                BtreePutType::Upsert,
                self.config.max_key_size(),
                std::mem::size_of::<BNodeId>() as u32,
            );
        }

        // For leaf nodes, check if there's room for the key and value from the request
        !node.has_room_for_put::<K, V>(req.put_type(), req.key_size(), req.value_size())
    }

    /// Check and split root if needed (matches C++ Btree::check_split_root lines 275-327)
    async fn check_split_root<ReqT>(&self, req: &ReqT) -> Result<(), BtreeError>
    where
        ReqT: BtreeRequest,
    {
        let _tree_lock = self.lock_tree_exclusive().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Write).await?;

        // Double-check split still needed, possibly some one else concurrently split the root already.
        if !self.is_split_needed(&root, req) {
            tracing::debug!(node_id = root_id, "Root split no longer needed");
            return Ok(());
        }

        tracing::debug!(
            root_id = root_id,
            level = root.level(),
            entries = root.total_entries(),
            "Root is full, creating new root"
        );

        // Create new interior node to put the new root
        let new_root = self.create_interior_node(root.node_variant()).await?;
        new_root.set_level(root.level() + 1);
        tracing::debug!(new_root_id = new_root.node_id(), "Created new root");

        // Old root becomes child_node, new_root becomes root
        let child_node = root;

        // Split the old root into two and put the pointer to the new root in the parent.
        let split_result = self.split_child_node(&new_root, &child_node, new_root.total_entries()).await;
        match split_result {
            Ok(_split_key) => {
                tracing::info!(old_root=root_id, new_root=new_root.node_id(),
                               split_key=?_split_key, "Root split successful");
                self.root_node_id.store(new_root.node_id(), std::sync::atomic::Ordering::Relaxed); // Update root node ID
                self.storage.on_root_changed(new_root.node_id()).await?;
                return Ok(());
            }
            Err(e) => {
                tracing::error!(error=?e, "Root split failed");
                self.storage.delete_node(new_root.node_id()).await?;
                return Err(e);
            }
        }
    }

    /// Split a child node
    async fn split_child_node(&self, parent_node: &Node, child_node: &Node, parent_idx: u32) -> Result<K, BtreeError> {
        debug_assert_eq!(parent_node.lock_type(), LockType::Write);
        debug_assert_eq!(child_node.lock_type(), LockType::Write);

        let child_node1 = child_node;

        // Create sibling node (child_node2)
        let child_node2 = match child_node1.is_leaf() {
            true => self.create_leaf_node(child_node1.node_variant()).await?,
            false => self.create_interior_node(child_node1.node_variant()).await?,
        };

        // Set sibling links
        child_node2.set_next_node(child_node1.get_next_node());
        child_node1.set_next_node(child_node2.node_id());
        child_node2.set_level(child_node1.level());

        // Calculate split size (C++ lines 343-346)
        let child1_filled_size = child_node1.node_data_size() - child_node1.available_size::<K, V>();
        let split_size = self.config.split_size(child1_filled_size);

        // Move entries from child1 to child2 by size (C++ line 346)
        let moved = child_node1.move_out_to_right_by_size::<K, V>(&child_node2, split_size);
        debug_assert!(moved > 0, "Unable to split entries in the child node");
        debug_assert!(child_node1.total_entries() > 0, "Child node1 should have entries after split");

        // Get the split key - last key in left child (auto-dispatches to correct value type)
        let split_key: K = child_node1.get_last_key::<K, V>().expect("Child node1 should have entries after split");

        tracing::debug!(parent=parent_node.node_id(), idx=parent_idx,
                        child1=child_node1.node_id(), child2=child_node2.node_id(),
                        split_key=?split_key, c1_entries=child_node1.total_entries(),
                        c2_entries=child_node2.total_entries(), moved=moved, "Split done");

        // Update parent: first update existing entry to point to second child, then insert split key (C++ lines
        // 360-366) Don't change the order. First update the parent node and then insert the new key. This is
        // important for cases where the split key is the last key in the parent node. In this case, the split
        // key should be inserted in the parent node. If we insert the split key first, then the split key will
        // be inserted in the parent node and the last key in the parent node will be lost. This will lead to
        // inconsistency in the tree. In case of empty parent (i.e., new root) or updating the edge, this order
        // made sure that edge is updated.
        parent_node.update_child::<K>(parent_idx, &child_node2.node_id())?;
        parent_node.insert_child::<K>(parent_idx, &split_key, &child_node1.node_id())?;
        tracing::trace!(idx=parent_idx, key=?split_key, child1=child_node1.node_id(),
                        child2=child_node2.node_id(), "Updated parent: insert+update done");
        tracing::debug!("Child1 after split: {}", child_node1.to_string::<K, V>());
        tracing::debug!("Child2 after split: {}", child_node2.to_string::<K, V>());
        tracing::debug!("Parent after split: {}", parent_node.to_string::<K, BNodeId>());

        // Write all three nodes
        self.storage.write_node(child_node1).await?;
        self.storage.write_node(&child_node2).await?;
        self.storage.write_node(parent_node).await?;

        Ok(split_key)
    }

    //////////////////////////////////////////////////////////////////////////////
    // Helper methods
    //////////////////////////////////////////////////////////////////////////////
    async fn apply_put_filter(
        &self,
        node: &Node,
        idx: u32,
        filter: Option<&dyn PutFilter<K, V>>,
    ) -> Result<PutFilterDecision, BtreeError> {
        if filter.is_none() {
            return Ok(PutFilterDecision::Replace);
        }

        let filter = filter.unwrap();
        let key = node.get_nth_key::<K, V>(idx, /* copy= */ false);

        if !filter.always_need_value() {
            let decision = filter.check_key(&key);
            if decision != PutFilterDecision::NeedOldValue {
                return Ok(decision);
            }
        }

        let old_val = node.get_nth_value::<K, V>(idx, /* copy= */ false).resolve(self.storage.as_ref(), false).await?;
        Ok(filter.check_kv(&key, &old_val))
    }

    async fn replace_value(&self, node: &Node, idx: u32, value: &V) -> Result<(), BtreeError> {
        let mut old_overflow_id: Option<BNodeId> = None;

        // Before update get the old overflow id (if it is infact an overflow value)
        if node.is_nth_value_overflow::<K, V>(idx) {
            let old_val = node.get_nth_value::<K, V>(idx, false);
            old_overflow_id = Some(old_val.expect_overflow("Expected overflow value, got inline value"));
        }

        let v = ValueOrOverflow::build(self.storage.as_ref(), value, self.config.overflow_threshold).await?;
        node.update::<K, V>(idx, &v)?;

        // Cleanup old overflow if it existed
        if let Some(old_id) = old_overflow_id {
            self.storage.delete_overflow(old_id).await?;
        }

        Ok(())
    }
}
