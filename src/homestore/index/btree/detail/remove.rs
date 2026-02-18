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
use super::super::btree_kvs::{BtreeKey, BtreeValue};
use super::super::btree::Btree;
use super::super::btree_types::{BtreeError, MergePolicy};
use super::btree_req::{
    BtreeRemoveRequest, BtreeRemoveAnyRequest, BtreeRangeRemoveRequest, BtreeKeyRange, RemoveFilter,
    RemoveFilterDecision,
};
use crate::{btree_io_err};

//================================================================================
// Remove Request Trait (for compile-time dispatch like C++ templates)
//================================================================================

/// Trait for different remove request types (compile-time polymorphism)
/// This provides static dispatch similar to C++ template specialization.
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
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

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
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

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
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

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
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
            let _ = node.get_nth_key::<K, V>(idx, /* copy= */ true);

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
                #[cfg(feature = "async_code")]
                {
                    total_removed += Box::pin(self.interior_walk_for_remove(child, ctx)).await?;
                }
                #[cfg(feature = "sync_code")]
                {
                    total_removed += self.interior_walk_for_remove(child, ctx)?;
                }
                break;
            }

            #[cfg(feature = "async_code")]
            {
                total_removed += Box::pin(self.interior_walk_for_remove(child, ctx)).await?;
            }
            #[cfg(feature = "sync_code")]
            {
                total_removed += self.interior_walk_for_remove(child, ctx)?;
            }
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
        if self.config.merge_policy == MergePolicy::Never {
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
        self.config.merge_policy != MergePolicy::Never && (child.occupied_size::<K, V>() < self.config.suggested_min_size)
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
        // Early return if invalid range or merge is disabled
        if end_idx <= start_idx || self.config.merge_policy == MergePolicy::Never {
            return Ok(false);
        }

        tracing::debug!("Merge child nodes: parent_node={}, leftmost_node={}, start_idx={}, end_idx={}",
            parent_node.node_id(), leftmost_node.node_id(), start_idx, end_idx);

        // Collections for old and new nodes
        let mut old_nodes: Vec<Node> = Vec::with_capacity(3);
        let mut new_nodes: Vec<Node> = Vec::with_capacity(3);
        let mut cur_new_node = leftmost_node.clone_temp(LockType::Write).await;
        let mut idx = start_idx + 1;

        // Main merge loop: read old nodes and pack into new nodes
        while idx <= end_idx {
            // Read the old node at this index
            let old_node = self.get_child_and_lock(parent_node, idx, LockType::Write).await?;
            tracing::trace!("Merge loop: processing node at idx={}, node_id={}, entries={}",
                idx, old_node.node_id(), old_node.total_entries());

            let mut src_cursor: u32 = 0;
            let mut src_has_more = true;

            // For the last node, we copy only if it fits, because we don't want to leave half moved last node at the
            // end, which might cause merge to be more expensive.
            let copy_only_if_fits = idx == end_idx;

            // Inner loop: Pack all entries from this old_node into new nodes
            while src_has_more {
                let prev_cursor = src_cursor;
                src_has_more = cur_new_node.append_copy_in_upto_size::<K, V>(
                    &old_node,
                    &mut src_cursor,
                    self.config.ideal_fill_size,
                    copy_only_if_fits,
                );
                tracing::trace!("cursor {} -> {}, has_more={}, copy_only_if_fits={}, cur_new_entries={}",
                    prev_cursor, src_cursor, src_has_more, copy_only_if_fits, cur_new_node.total_entries());

                if src_has_more {
                    if copy_only_if_fits {
                        // Last old node doesn't fit completely.
                        break;
                    }

                    // Current node is full - save it and create a fresh one
                    debug_assert_ne!(cur_new_node.total_entries(), 0, "New node after append still empty");
                    new_nodes.push(cur_new_node);
                    cur_new_node = self.create_new_node(leftmost_node.is_leaf(), leftmost_node.node_variant()).await?;
                }
            }

            if src_has_more && copy_only_if_fits {
                // Last old node doesn't fit completely.
                break;
            }

            // Save the old node for later cleanup
            old_nodes.push(old_node);
            idx += 1;
        }

        // After all old nodes processed: handle the final working node
        if cur_new_node.total_entries() > 0 {
            new_nodes.push(cur_new_node);
        } else {
            // Empty node created but never used - delete it, but only if it's NOT the temp clone of leftmost_node
            if cur_new_node.node_id() != leftmost_node.node_id() {
                self.storage.delete_node(cur_new_node.node_id()).await?;
            }
        }

        if new_nodes.len() == 0 {
            debug_assert!(false, "Turns out that commit merge result in all empty nodes");
            return btree_io_err!(InvalidData,
                format!("Merge resulted in all empty nodes - leftmost, {} old nodes were empty", old_nodes.len()));
        }

        // Debug: Dump nodes before merge decision
        tracing::trace!("Before merge decision - leftmost node: {}", leftmost_node.to_string::<K, V>());
        for (i, node) in old_nodes.iter().enumerate() {
            tracing::trace!("Before merge - old node {}: {}", i + 1, node.to_string::<K, V>());
        }
        for (i, node) in new_nodes.iter().enumerate() {
            tracing::trace!("Before merge - new node {}: {}", i, node.to_string::<K, V>());
        }
        tracing::trace!("Before merge - parent node: {}", parent_node.to_string::<K, BNodeId>());

        // Decision point: should we commit this merge?
        if !self.should_accept_merge(end_idx - start_idx + 1, old_nodes.len() + 1, new_nodes.len()) {
            tracing::info!("Merge rejected by {:?} policy: attempted={}, old={}, new={}", 
                self.config.merge_policy, end_idx - start_idx + 1, old_nodes.len() + 1, new_nodes.len());

            // Cleanup: delete the new nodes we created
            for (i, node) in new_nodes.iter().enumerate() {
                // Skip first node if it has the same ID as leftmost_node (it's the temp clone)
                if node.node_id() == leftmost_node.node_id() {
                    debug_assert_eq!(i, 0, "First new node should be the temp clone of leftmost node");
                    continue;
                }
                self.storage.delete_node(node.node_id()).await?;
            }
            return Ok(false);
        }

        // We are committing the merge at this point. We are going to overwrite the leftmost node with the temp node
        // and delete the temp node. We are also going to update the parent node entries for the new nodes.
        tracing::info!("Committing merge: reducing {} nodes to {}", old_nodes.len() + 1, new_nodes.len());

        // Step 1: Swap the contents of the first new node with the leftmost node, because leftmost node is updated
        // in-place. We will remove the first new node after this step.
        leftmost_node.overwrite(&new_nodes[0]);
        new_nodes.remove(0);

        // Step 2: Remove the excess entries from the parent node
        parent_node
            .remove_range::<K, BNodeId>(start_idx + 1 + new_nodes.len() as u32, start_idx + old_nodes.len() as u32)?;

        // Step 3: Walk through the new nodes in reverse order and update both the next node and
        // the parent node entries for the new nodes.
        let mut next_node_id = old_nodes.last().unwrap().get_next_node();
        let mut parent_idx = start_idx + new_nodes.len() as u32;
        for node in new_nodes.iter().rev() {
            node.set_next_node(next_node_id);
            let last_key = node.get_last_key::<K, V>().ok_or(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Last key not found for new node {}", node.node_id()),
            )))?;
            parent_node.update_child_with_key::<K>(parent_idx, &last_key, &node.node_id())?;
            next_node_id = node.node_id();
            parent_idx -= 1;
        }

        // Step 4: Update the leftmost node's next pointer
        leftmost_node.set_next_node(next_node_id);
        let last_key = leftmost_node.get_last_key::<K, V>().ok_or(BtreeError::Io(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Last key not found for leftmost node {}", leftmost_node.node_id()),
        )))?;
        parent_node.update_child_with_key::<K>(start_idx, &last_key, &leftmost_node.node_id())?;

        // Debug: Dump merged nodes state
        tracing::trace!("Leftmost node after merge: {}", leftmost_node.to_string::<K, V>());
        for (i, node) in new_nodes.iter().enumerate() {
            tracing::trace!("New node {} after merge: {}", i + 1, node.to_string::<K, V>());
        }
        tracing::trace!("Parent after merge: {}", parent_node.to_string::<K, BNodeId>());

        // Step 5: Delete the old nodes and write the leftmost node and parent node to the storage
        for node in old_nodes.iter() {
            self.storage.delete_node(node.node_id()).await?;
        }
        self.storage.write_node(leftmost_node).await?;
        self.storage.write_node(parent_node).await?;

        // Step 6: Write the new nodes to the storage
        for node in new_nodes.iter() {
            self.storage.write_node(node).await?;
        }

        Ok(true)
    }
        
    /// Policy-based merge acceptance decision
    ///
    /// Determines whether to commit a merge based on configured MergePolicy:
    /// - Aggressive: Accept any reduction in node count
    /// - Conservative: Accept only if 2+ nodes saved, or 1+ when only 2 attempted (edge case)
    ///
    /// # Arguments
    /// * `attempted` - Number of nodes we attempted to merge (end_idx - start_idx + 1)
    /// * `old_total` - Total nodes before merge (old_nodes.len() + 1 for leftmost)
    /// * `new_total` - Total nodes after merge (new_nodes.len())
    fn should_accept_merge(&self, attempted: u32, old_total: usize, new_total: usize) -> bool {
        // Always reject if no reduction
        if new_total >= old_total {
            return false;
        }

        let nodes_saved = old_total - new_total;

        match self.config.merge_policy {
            MergePolicy::Never => false,
            MergePolicy::Aggressive => nodes_saved > 0,
            MergePolicy::Conservative => {
                // For edge merges (only 2 nodes available), accept any savings
                if attempted == 2 {
                    nodes_saved >= 1
                } else {
                    // For middle merges (3+ nodes), require 2+ savings
                    nodes_saved >= 2
                }
            }
        }
    }
}
