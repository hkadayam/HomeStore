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
 **************************************************************************/

//! Btree Remove Operations
//!
//! This module contains REMOVE and merge operations.
//! Corresponds to btree_remove_impl.ipp in C++ btree implementation.

use std::io;
use super::super::btree_node::{Node, BNodeId, LockType};
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree::{BtreeError, Btree};
use super::btree_req::{
    BtreeRemoveRequest, BtreeRemoveAnyRequest, BtreeRangeRemoveRequest, BtreeKeyRange, RemoveFilter,
    RemoveFilterDecision,
};

//================================================================================
// Remove Request Trait (for compile-time dispatch like C++ templates)
//================================================================================

/// Trait for different remove request types (compile-time polymorphism)
/// This provides static dispatch similar to C++ template specialization.
trait RemoveContext<K: BtreeKey, V: BtreeValue>: Send {
    /// Execute removal on a leaf node
    async fn execute_on_leaf(&mut self, btree: &Btree<K, V>, leaf: &mut Node) -> Result<u32, BtreeError>;

    /// Find child index range in interior node (non-async, pure calculation)
    /// Returns (start_idx, end_idx) - both inclusive
    fn find_child_indices(&self, interior: &Node) -> (u32, u32);
}

/// Context for single-key removal
struct RemoveOneContext<'a, K: BtreeKey, V: BtreeValue> {
    key: &'a K,
    filter: Option<&'a dyn RemoveFilter<K, V>>,
    result: &'a mut Option<V>,
}

impl<'a, K: BtreeKey + 'static, V: BtreeValue + 'static> RemoveContext<K, V> for RemoveOneContext<'a, K, V> {
    async fn execute_on_leaf(&mut self, btree: &Btree<K, V>, leaf: &mut Node) -> Result<u32, BtreeError> {
        let (found, idx) = leaf.find::<K, V>(self.key);
        if !found {
            return Ok(0);
        }

        // Apply filter if provided
        let (decision, value) = btree.apply_remove_filter(leaf, idx, self.filter).await?;
        match decision {
            RemoveFilterDecision::Remove => {
                // Remove existing overflow node if was overflowed
                if leaf.is_nth_value_overflow::<K, V>(idx) {
                    let old_val = leaf.get_nth_value::<K, V>(idx, false);
                    btree.storage.delete_overflow(old_val.unwrap_overflow()).await?;
                }

                // Remove the entry from the node
                leaf.remove::<K, V>(idx)?;
                *self.result = Some(value.unwrap());
                btree.storage.write_node(leaf).await?;
                Ok(1)
            }
            RemoveFilterDecision::Skip => Ok(0),
            RemoveFilterDecision::NeedValue => {
                panic!("apply_remove_filter returned NeedValue - this should not happen");
            }
        }
    }

    fn find_child_indices(&self, interior: &Node) -> (u32, u32) {
        let idx = interior.find::<K, V>(self.key).1;
        (idx, idx) // Single key = same start and end
    }
}

/// Context for remove-any (remove one key from range)
struct RemoveAnyContext<'a, K: BtreeKey, V: BtreeValue> {
    range: &'a BtreeKeyRange<K>,
    result_key: &'a mut Option<K>,
    result_value: &'a mut Option<V>,
}

impl<'a, K: BtreeKey + 'static, V: BtreeValue + 'static> RemoveContext<K, V> for RemoveAnyContext<'a, K, V> {
    async fn execute_on_leaf(&mut self, btree: &Btree<K, V>, leaf: &mut Node) -> Result<u32, BtreeError> {
        let (matched, start_idx, end_idx) = leaf.match_range::<K, V>(self.range);
        if !matched {
            return Ok(0);
        }

        let idx = (start_idx + end_idx) / 2;
        let key = leaf.get_nth_key::<K, V>(idx, true);

        // Need value to decide
        let copy = true;
        let value = leaf.get_nth_value::<K, V>(idx, copy).resolve(btree.storage.as_ref(), copy).await?;

        // Remove overflow node if needed and then entry from node
        if leaf.is_nth_value_overflow::<K, V>(idx) {
            btree.storage.delete_overflow(leaf.get_nth_value::<K, V>(idx, false).unwrap_overflow()).await?;
        }
        leaf.remove::<K, V>(idx)?;

        *self.result_key = Some(key);
        *self.result_value = Some(value);
        btree.storage.write_node(leaf).await?;
        Ok(1)
    }

    fn find_child_indices(&self, interior: &Node) -> (u32, u32) {
        let (matched, start_idx, end_idx) = interior.match_range::<K, V>(self.range);
        if !matched {
            return (0, 0);
        }
        let mid_idx = (start_idx + end_idx) / 2;
        (mid_idx, mid_idx) // Pick middle child for remove-any
    }
}

/// Context for range removal (remove all keys in range)
struct RemoveRangeContext<'a, K: BtreeKey, V: BtreeValue> {
    range: &'a BtreeKeyRange<K>,
    filter: Option<&'a dyn RemoveFilter<K, V>>,
    _phantom: std::marker::PhantomData<V>,
}

impl<'a, K: BtreeKey + 'static, V: BtreeValue + 'static> RemoveContext<K, V> for RemoveRangeContext<'a, K, V> {
    async fn execute_on_leaf(&mut self, btree: &Btree<K, V>, leaf: &mut Node) -> Result<u32, BtreeError> {
        // Remove all matching entries in this leaf (no batch limit)
        let num_removed = btree.multi_remove_from_leaf(leaf, self.range, u32::MAX, self.filter).await?;
        if num_removed > 0 {
            btree.storage.write_node(leaf).await?;
        }
        Ok(num_removed)
    }

    fn find_child_indices(&self, interior: &Node) -> (u32, u32) {
        let (matched, start_idx, end_idx) = interior.match_range::<K, V>(self.range);
        if !matched {
            return (0, 0);
        }
        (start_idx, end_idx) // Return FULL range for range operations
    }
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
    // Multi-REMOVE: Remove multiple entries from leaf with overflow and filtering
    //================================================================================

    /// Remove multiple entries from leaf node matching range
    ///
    /// Implementation with:
    /// - Async overflow value resolution
    /// - Two-phase filtering (check_key, then check_kv)
    ///
    /// # Returns
    /// Number of entries actually removed
    async fn multi_remove_from_leaf(
        &self,
        node: &Node,
        range: &BtreeKeyRange<K>,
        max_count: u32,
        filter: Option<&dyn RemoveFilter<K, V>>,
    ) -> Result<u32, BtreeError> {
        debug_assert!(node.is_leaf(), "Multi remove only for leaf nodes");

        let (matched, start_idx, end_idx) = node.match_range::<K, V>(range);
        if !matched {
            return Ok(0); // No matches, return 0
        }

        let mut removed = 0u32;
        let mut idx = start_idx;

        // Iterate through range entries
        while idx <= end_idx && removed < max_count {
            let key = node.get_nth_key::<K, V>(idx, /* copy= */ true);

            let (decision, _value) = self.apply_remove_filter(node, idx, filter).await?;
            match decision {
                RemoveFilterDecision::Remove => {
                    // Remove existing overflow node if was overflowed
                    if node.is_nth_value_overflow::<K, V>(idx) {
                        let old_val = node.get_nth_value::<K, V>(idx, false);
                        self.storage.delete_overflow(old_val.unwrap_overflow()).await?;
                    }

                    // Remove the entry from the node
                    node.remove::<K, V>(idx)?;
                    removed += 1;
                    // Don't increment idx - entries shift down after removal
                }
                RemoveFilterDecision::Skip => {
                    idx += 1; // Skip this entry
                }
                RemoveFilterDecision::NeedValue => {
                    panic!("apply_remove_filter returned NeedValue - this should not happen");
                }
            }
        }

        Ok(removed)
    }
}

//================================================================================
// Remove Context Implementations
//================================================================================

impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    /// Internal single-key remove (matches C++ remove(BtreeSingleRemoveRequest))
    pub(in super::super) async fn remove_one_internal(
        &self,
        req: BtreeRemoveRequest<'_, K, V>,
    ) -> Result<Option<V>, BtreeError> {
        let mut result = None;
        let mut ctx = RemoveOneContext {
            key: req.key(),
            filter: req.filter(),
            result: &mut result,
        };
        self.root_walk_for_remove(&mut ctx).await?;
        Ok(result)
    }

    /// Internal remove-any (matches C++ remove(BtreeRemoveAnyRequest))
    pub(in super::super) async fn remove_any_internal(
        &self,
        req: BtreeRemoveAnyRequest<K>,
    ) -> Result<Option<(K, V)>, BtreeError> {
        let mut result_key = None;
        let mut result_value = None;
        let mut ctx = RemoveAnyContext {
            range: req.range(),
            result_key: &mut result_key,
            result_value: &mut result_value,
        };
        self.root_walk_for_remove(&mut ctx).await?;

        Ok(match (result_key, result_value) {
            (Some(k), Some(v)) => Some((k, v)),
            _ => None,
        })
    }

    /// Internal range remove (matches C++ remove(BtreeRangeRemoveRequest))
    pub(in super::super) async fn remove_range_internal<'a>(
        &self,
        req: BtreeRangeRemoveRequest<'a, K, V>,
    ) -> Result<u32, BtreeError> {
        let mut ctx = RemoveRangeContext::<K, V> {
            range: req.input_range(),
            filter: req.filter(),
            _phantom: std::marker::PhantomData,
        };
        self.root_walk_for_remove(&mut ctx).await
    }

    /// Apply remove filter to an entry
    ///
    /// Similar to apply_get_filter but for remove operations.
    /// Returns (decision, value) where value is always Some when decision is Remove.
    async fn apply_remove_filter(
        &self,
        node: &Node,
        idx: u32,
        filter: Option<&dyn RemoveFilter<K, V>>,
    ) -> Result<(RemoveFilterDecision, Option<V>), BtreeError> {
        if filter.is_none() {
            let copy = true; // We copy the value to return to the caller
            let value = node.get_nth_value::<K, V>(idx, copy).resolve(self.storage.as_ref(), copy).await?;
            return Ok((RemoveFilterDecision::Remove, Some(value)));
        }

        let filter = filter.unwrap();
        let key = node.get_nth_key::<K, V>(idx, /* copy= */ false);

        let mut decision = RemoveFilterDecision::NeedValue;
        if !filter.always_needs_value() {
            decision = filter.check_key(&key);
            if decision == RemoveFilterDecision::Skip {
                return Ok((decision, None));
            }
        }

        // Need value to decide
        let copy = true;
        let value = node.get_nth_value::<K, V>(idx, copy).resolve(self.storage.as_ref(), copy).await?;
        if decision == RemoveFilterDecision::NeedValue {
            let decision = filter.check_kv(&key, &value);
            #[rustfmt::skip]
            debug_assert_ne!(decision, RemoveFilterDecision::NeedValue, 
                "Filter returned NeedValue after receiving value");
        }

        Ok((decision, Some(value)))
    }

    /// Common walk down from root for all remove operations (generic over request context)
    ///
    /// Handles:
    /// - Tree-level locking
    /// - Root node acquisition
    /// - Empty root detection and collapse
    /// - Retry logic on concurrent modifications
    async fn root_walk_for_remove<R>(&self, ctx: &mut R) -> Result<u32, BtreeError>
    where
        R: RemoveContext<K, V>,
    {
        'retry: loop {
            // Step 1: Lock tree shared and read root
            let _tree_guard = self.lock_tree_shared().await;
            let root_id = self.root_node_id();
            let root = self.read_and_lock_node(root_id, LockType::ReadInteriorWriteLeaf).await?;

            // Step 2: Check if root is empty
            if root.total_entries() == 0 {
                if root.is_leaf() {
                    return Ok(0); // Empty leaf root - not found
                }

                // Empty interior root - try to collapse
                drop(root);
                drop(_tree_guard);

                self.check_collapse_root().await?;
                continue 'retry; // Root collapsed, try from new root
            }

            // Step 3: Do the actual remove operation
            match self.interior_walk_for_remove(root, ctx).await {
                Ok(num_removed) => {
                    return Ok(num_removed);
                }
                Err(BtreeError::Retry) => {
                    continue 'retry;
                }
                Err(e) => return Err(e),
            }
        }
    }

    /// Core recursive remove logic (generic over request context)
    ///
    /// Recursively walks down the tree to find and remove the target key/range.
    /// Handles both leaf removal and interior node traversal with merge detection.
    async fn interior_walk_for_remove<R>(&self, mut my_node: Node, ctx: &mut R) -> Result<u32, BtreeError>
    where
        R: RemoveContext<K, V>,
    {
        if my_node.is_leaf() {
            // Leaf node - execute removal
            return ctx.execute_on_leaf(self, &mut my_node).await;
        }

        // Interior node - find child range and recurse
        let (start_idx, end_idx) = ctx.find_child_indices(&my_node);
        let mut curr_idx = start_idx;
        let mut total_removed = 0;

        // Iterate through all children in range
        while curr_idx <= end_idx {
            let mut child = self.get_child_and_lock(&my_node, curr_idx, LockType::ReadInteriorWriteLeaf).await?;

            // Check if merge is needed
            if self.is_merge_needed(&child) {
                let node_end_idx =
                    if my_node.has_valid_edge() { my_node.total_entries() } else { my_node.total_entries() - 1 };

                let merge_end_idx = std::cmp::min(node_end_idx, curr_idx + self.config.max_merge_nodes - 1);
                if merge_end_idx > curr_idx {
                    // Upgrade locks for merge
                    (my_node, child) = self.upgrade_node_locks(my_node, child).await?;

                    // Try merge
                    if self.merge_child_nodes(&my_node, &mut child, curr_idx, merge_end_idx).await? {
                        // Merge succeeded, retry from parent
                        return Err(BtreeError::Retry);
                    }
                }
            }

            // If we have reached the last index, unlock before traversing down, because we no longer need
            // this lock. Holding this lock will impact performance unncessarily.
            if curr_idx == end_idx {
                drop(my_node);
                // Need an explicit call to satisy the compiler, to ensure we drop and exit under same condition
                total_removed += Box::pin(self.interior_walk_for_remove(child, ctx)).await?;
                break;
            }

            total_removed += Box::pin(self.interior_walk_for_remove(child, ctx)).await?;
            curr_idx += 1;
        }
        Ok(total_removed)
    }

    ////////////////////////////////////////////////////////////////////////////////
    //                    Helper functions for remove operations                  //
    ///////////////////////////////////////////////////////////////////////////////

    /// Check if root should be collapsed (means that it has only edge child and no entries) and if so,
    /// collapse it and set the new root to the edge child
    async fn check_collapse_root(&self) -> Result<bool, BtreeError> {
        if !self.config.merge_turned_on {
            return Ok(false);
        }

        let _tree_guard = self.lock_tree_exclusive().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Write).await?;

        if root.total_entries() != 0 || root.is_leaf() {
            // Root not empty or is leaf - no collapse needed
            return Ok(false);
        }

        debug_assert!(root.has_valid_edge(), "Empty interior root must have edge");
        let child_id = root.get_edge_value();
        self.root_node_id.store(child_id, std::sync::atomic::Ordering::Relaxed);
        self.storage.on_root_changed(child_id).await?;

        // Delete old root
        self.storage.delete_node(root.node_id()).await?;

        Ok(true)
    }

    /// Check if child node needs merging (matches C++ is_merge_needed)
    fn is_merge_needed(&self, child: &Node) -> bool {
        self.config.merge_turned_on && (child.occupied_size::<K, V>() < self.config.suggested_min_size)
    }

    /// Merge child nodes to reduce the number of nodes
    ///
    /// This function attempts to merge multiple child nodes (from start_idx to end_idx)
    /// into fewer nodes by packing entries more efficiently up to ideal_fill_size.
    ///
    /// # Arguments
    /// * `parent_node` - The parent node containing references to children
    /// * `leftmost_node` - The leftmost child node in the merge range (modified in-place)
    /// * `start_idx` - Starting index in parent node
    /// * `end_idx` - Ending index in parent node (inclusive)
    ///
    /// # Returns
    /// * `Ok(true)` - Merge succeeded and reduced node count
    /// * `Ok(false)` - Merge didn't reduce nodes or is disabled
    /// * `Err(...)` - Other errors (I/O, node not found, etc.)
    pub(in super::super) async fn merge_child_nodes(
        &self,
        parent_node: &Node,
        leftmost_node: &mut Node,
        start_idx: u32,
        end_idx: u32,
    ) -> Result<bool, BtreeError> {
        // Early return if invalid range
        if end_idx <= start_idx {
            return Ok(false);
        }

        // Check if merge is enabled
        if !self.config.merge_turned_on {
            return Ok(false);
        }

        // Collections for old and new nodes
        let mut old_nodes: Vec<Node> = Vec::with_capacity(3);
        let mut new_nodes: Vec<Node> = Vec::with_capacity(3);

        // Clone leftmost node as starting point for merge
        new_nodes.push(leftmost_node.clone_temp(LockType::Write).await);

        // Loop variables
        let mut idx = start_idx + 1;
        let mut new_idx = 0;
        let mut old_node: Option<Node> = None;
        let mut src_cursor: u32 = 0;
        let mut next_node_id: BNodeId;

        // Main merge loop: read old nodes and pack into new nodes
        while idx <= end_idx {
            // Read old node if needed
            if old_node.is_none() {
                let child = self.get_child_and_lock(parent_node, idx, LockType::Write).await?;
                old_node = Some(child);
                src_cursor = 0;
            }

            // Get mutable reference to current working node
            let new_ref = &mut new_nodes[new_idx];
            let old_ref = old_node.as_ref().unwrap();

            if idx == end_idx {
                let copied = new_ref.append_copy_in_upto_size::<K, V>(
                    old_ref,
                    &mut src_cursor,
                    self.config.ideal_fill_size,
                    /* copy_only_if_fits= */ true,
                );
                if !copied {
                    if new_ref.total_entries() == 0 {
                        // Remove empty last node
                        let empty = new_nodes.pop().unwrap();
                        self.storage.delete_node(empty.node_id()).await?;
                    }
                } else {
                    old_nodes.push(old_node.take().unwrap()); // Move old node to vector
                }
                break;
            } else {
                new_ref.append_copy_in_upto_size::<K, V>(
                    old_ref,
                    &mut src_cursor,
                    self.config.ideal_fill_size,
                    /* copy_only_if_fits= */ false,
                );

                if src_cursor == old_ref.total_entries() {
                    old_nodes.push(old_node.take().unwrap()); // Finished with this old node
                    idx += 1;
                } else {
                    // Current new node is full - create another
                    let node = self.create_new_node(leftmost_node.is_leaf(), leftmost_node.node_variant()).await?;
                    new_nodes.push(node);
                    new_idx += 1; // Work on the new node
                }
            }
        }
        // Only commit merge if we actually reduced the number of nodes
        // Total OLD = leftmost + old_nodes.len()
        // Total NEW = new_nodes.len() (including temp at [0])
        // But since leftmost is reused, effective NEW = leftmost + (new_nodes.len() - 1)
        // Reduce: new_nodes.len() < old_nodes.len() + 1
        if new_nodes.len() > old_nodes.len() {
            // No benefit from merge, cleanup and return
            for node in new_nodes.iter().rev() {
                self.storage.delete_node(node.node_id()).await?;
            }
            return Ok(false);
        }

        // Commit the merge:

        // 1. Remove excess entries from parent node
        // We're going from (end_idx - start_idx + 1) children to new_nodes.len() children
        // Remove parent entries [start_idx + new_nodes.len(), end_idx]
        parent_node.remove_range::<K, BNodeId>(start_idx + new_nodes.len() as u32, end_idx)?;

        // 2. Update parent node entries for NEW nodes only (skip new_nodes[0] which is temp)
        // Start from the last NEW node (not temp) and work backwards
        idx = start_idx + new_nodes.len() as u32 - 1;
        next_node_id = old_nodes.last().unwrap().get_next_node();

        // Process only the actual new nodes (new_nodes[1..]), skip the temp clone at [0]
        for new_node_ref in new_nodes[1..].iter().rev() {
            new_node_ref.set_next_node(next_node_id);
            let this_node_id = new_node_ref.node_id();

            if new_node_ref.total_entries() > 0 {
                let last_key = new_node_ref.get_last_key::<K, V>().ok_or(BtreeError::Io(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Last key not found for new node {}", this_node_id),
                )))?;
                parent_node.update_child_with_key::<K>(idx, &last_key, &this_node_id)?;
                idx -= 1;
            }
            next_node_id = this_node_id;
        }

        // 3. Update leftmost_node in-place with temp's content
        leftmost_node.overwrite(&new_nodes[0]);
        // Set next pointer: if there are new nodes, point to first new node; else use saved next
        if new_nodes.len() > 1 {
            leftmost_node.set_next_node(new_nodes[1].node_id());
        } else {
            leftmost_node.set_next_node(next_node_id);
        }

        if leftmost_node.total_entries() > 0 {
            let last_key = leftmost_node.get_last_key::<K, V>().ok_or(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Last key not found for leftmost node {}", leftmost_node.node_id()),
            )))?;
            parent_node.update_child_with_key::<K>(start_idx, &last_key, &leftmost_node.node_id())?;
        }

        // 4. Commit transaction with storage layer
        // Write only the actual new nodes (skip temp at index 0)
        for node in new_nodes[1..].iter() {
            self.storage.write_node(node).await?;
        }
        // Delete the temp node
        self.storage.delete_node(new_nodes[0].node_id()).await?;
        for node in old_nodes.iter() {
            self.storage.delete_node(node.node_id()).await?;
        }
        self.storage.write_node(leftmost_node).await?;
        self.storage.write_node(parent_node).await?;
        Ok(true)
    }
}
