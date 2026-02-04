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

//! Btree Mutation Operations
//!
//! This module contains PUT, REMOVE, and other mutation operations.
//! Corresponds to btree mutate operations in C++ btree implementation.

use super::super::btree_node::{Node, BNodeId, LockType};
use super::super::btree_kvs::{BtreeKey, BtreeValue};
use super::super::btree::{BtreeError, Btree};
use super::btree_req::{BtreeSinglePutRequest, BtreeRangePutRequest, BtreeRequest, BtreePutType};

//================================================================================
// PUT Result Type
//================================================================================

/// Result of PUT operation
#[derive(Debug, PartialEq, Eq)]
pub enum PutResult {
    Success,   // New key inserted
    Updated,   // Existing key updated
}

//================================================================================
// Internal Implementation (called from btree.rs public API)
//================================================================================

impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    //================================================================================
    // Single-key PUT Implementation
    //================================================================================
    
    /// Single-key PUT request with retry loop
    pub(in super::super) async fn put_single_request<'a>(&self, req: &'a BtreeSinglePutRequest<'a, K, V>) 
        -> Result<PutResult, BtreeError> {
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

            match self.do_put_single(root, req).await {
                Ok(()) => return Ok(PutResult::Success),
                Err(BtreeError::Retry) => continue, // Retriable errors
                Err(e) => return Err(e), // Non-retriable errors
            }
        }
    }

    /// Recursive PUT for single-key requests (matches C++ do_put)
    async fn do_put_single<'a>(&self, mut my_node: Node, req: &'a BtreeSinglePutRequest<'a, K, V>) 
        -> Result<(), BtreeError> {
        if my_node.is_leaf() {
            return self.mutate_write_leaf_single(&my_node, req).await;
        }

        // Interior node: find child and traverse
        loop {
            let (_, idx) = my_node.find::<K, V>(req.key());
            let mut child = self.get_child_node_locked(&my_node, idx, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&child, req) {
                // Upgrade both the locks - if it fails, retry from root
                (my_node, child) = self.upgrade_node_locks(my_node, child).await?;

                // Perform split - if it fails, propagate error to retry from root
                let _split_key = self.split_child_node(&my_node, &child, idx).await?;
                continue; // After successful split, retry search from this node
            }

            drop(my_node); // Can unlock the parent node now
            return self.do_put_single(child, req).await;
        }
    }

    /// Write to leaf node for single-key request (matches C++ mutate_write_leaf_node lines 259-269)
    async fn mutate_write_leaf_single<'a>(&self, my_node: &Node, req: &'a BtreeSinglePutRequest<'a, K, V>) 
        -> Result<(), BtreeError> {
        // Call node's put method - node variant handles the actual insertion logic
        // TODO: Replace None with filter_cb result when filter_cb is implemented
        my_node.put(req.key(), req.value(), req.put_type())?;
        self.storage.write_node(my_node).await?;
        Ok(())
    }

    //================================================================================
    // Range PUT Implementation
    //================================================================================
    
    /// Range PUT request with retry loop (matches C++ lines 71-78 for has_more handling)
    pub(in super::super) async fn put_range_request<'a>(&self, req: &'a BtreeRangePutRequest<'a, K, V>) 
        -> Result<PutResult, BtreeError> {
        loop {
            let tree_lock = self.lock_tree_shared().await;
            let root_id = self.root_node_id();
            let root = self.read_and_lock_node(root_id, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&root, req) {
                drop(root); 
                drop(tree_lock);
                self.check_split_root(req).await?;
                continue; // Retry from root after split
            }

            match self.do_put_range(root, req).await {
                Ok(result) => return Ok(result),
                Err(BtreeError::Retry) => continue, // Retriable errors
                Err(e) => return Err(e), // Non-retriable errors
            }
        }
    }
    
    /// Main PUT recursive traversal for range operations (matches C++ Btree::do_put lines 108-243)
    async fn do_put_range<'a>(&self, mut my_node: Node, req: &'a BtreeRangePutRequest<'a, K, V>) 
        -> Result<PutResult, BtreeError> {
        if my_node.is_leaf() {
            return self.mutate_write_leaf_node_range(&my_node, req).await;
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
                let mut child = self.get_child_node_locked(&my_node, curr_idx, LockType::ReadInteriorWriteLeaf).await?;

                // Check if split needed
                if self.is_split_needed(&child, req) {
                    (my_node, child) = self.upgrade_node_locks(my_node, child).await?;
                    let _split_key = self.split_child_node(&my_node, &child, curr_idx).await?;
                    continue 'outer; // Restart from this node again after split
                }

                // Trim working range for leaf child to how much ever the node link's highest key is.
                if child.is_leaf() && curr_idx < my_node.total_entries() {
                    let child_end_key: K = my_node.get_nth_key::<K, V>(curr_idx, /*copy=*/true);
                    if child_end_key < req.working_range().end_key {
                        req.trim_working_range(child_end_key, /*end_incl=*/true);
                    }
                }

                if curr_idx == end_idx {
                    drop(my_node);
                    return self.do_put_range(child, req).await;
                } else {
                    self.do_put_range(child, req).await?;
                    curr_idx += 1;
                }
            }

            return Ok(PutResult::Success);
        }
    }

    /// Write to leaf node for range operations (matches C++ lines 250-257)
    async fn mutate_write_leaf_node_range<'a>(&self, my_node: &Node, req: &'a BtreeRangePutRequest<'a, K, V>) 
        -> Result<PutResult, BtreeError> {
        // Call node's multi_put method - node variant handles the actual insertion logic (line 251-252)
        let mut last_failed_key: Option<K> = None;
        let put_type = super::btree_req::BtreePutType::Update; // Range operations always update
        match my_node.multi_put::<K, V>(&*req.working_range(), req.value(), put_type, 
                                        last_failed_key.as_mut()) {
            Err(BtreeError::HasMore) => {
                // Shift to working range starting from the last failed key
                req.shift_working_range(last_failed_key);
            }
            Ok(()) => {
                // Since entire current working range has been processed successfully, shift to the next batch.
                req.shift_working_range(None);
            }
            Err(e) => return Err(e), // Propagate other errors
        }
        
        // Write the modified node to storage (matches C++ line 268)
        self.storage.write_node(my_node).await?;
        Ok(PutResult::Success)
    }

    //================================================================================
    // Split related method
    //================================================================================

    /// Check if node needs a split in case any insertion has to be done in this node. 
    /// Applicable for both interior and leaf nodes. (matches C++ lines 383-393)
    fn is_split_needed<ReqT>(&self, node: &Node, req: &ReqT) -> bool
    where
        ReqT: BtreeRequest + ?Sized {
        if !node.is_leaf() {
            // Interior node: size is at most one additional entry with max key and nodeid
            return !node.has_room_for_put::<K, V>(BtreePutType::Upsert, K::get_max_size(),
                std::mem::size_of::<BNodeId>() as u32);
        }
    
        // For leaf nodes, check if there's room for the key and value from the request
        !node.has_room_for_put::<K, V>(req.put_type(), req.key_size(), req.value_size())
    }

    /// Check and split root if needed (matches C++ Btree::check_split_root lines 275-327)
    async fn check_split_root<ReqT>(&self, req: &ReqT) -> Result<(), BtreeError>
    where
        ReqT: BtreeRequest {
        let _tree_lock = self.lock_tree_exclusive().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Write).await?;

        // Double-check split still needed, possibly some one else concurrently split the root already.
        if !self.is_split_needed(&root, req) {
            return Ok(());
        }

        // Create new interior node to put the new root
        let new_root = self.create_interior_node(root.node_variant()).await?;
        new_root.set_level(root.level() + 1);

        // Old root becomes child_node, new_root becomes root
        let child_node = root;

        // Split the old root into two and put the pointer to the new root in the parent.
        let split_result = self.split_child_node(&new_root, &child_node, new_root.total_entries()).await;
        match split_result {
            Ok(_split_key) => {
                self.root_node_id.set(new_root.node_id()); // Update root node ID
                self.storage.on_root_changed(new_root.node_id()).await?;
                return Ok(());
            }
            Err(e) => {
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
        let split_key: K = child_node1.get_last_key::<K, V>()
            .expect("Child node1 should have entries after split");

        // Update parent: first update existing entry to point to second child, then insert split key (C++ lines 360-366)
        // Don't change the order. First update the parent node and then insert the new key. This is important for
        // cases where the split key is the last key in the parent node. In this case, the split key should be
        // inserted in the parent node. If we insert the split key first, then the split key will be inserted in the
        // parent node and the last key in the parent node will be lost. This will lead to inconsistency in the tree.
        // In case of empty parent (i.e., new root) or updating the edge, this order made sure that edge is updated.
        parent_node.update_child::<K>(parent_idx, &child_node2.node_id())?;
        parent_node.insert_child::<K>(parent_idx, &split_key, &child_node1.node_id())?;

        // Write all three nodes
        self.storage.write_node(child_node1).await?;
        self.storage.write_node(&child_node2).await?;
        self.storage.write_node(parent_node).await?;

        Ok(split_key)
    }

    /// Get child node and lock it (combines get_nth_child_id + read_and_lock_node)
    async fn get_child_node_locked(&self, parent: &Node, idx: u32, lock_type: LockType) 
        -> Result<Node, BtreeError> {
        let child_id = parent.get_nth_child_id::<K>(idx);
        self.read_and_lock_node(child_id, lock_type).await
    }
}
