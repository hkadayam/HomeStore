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

//! Btree Mutation Operations
//!
//! This module contains PUT, REMOVE, and other mutation operations.
//! Corresponds to btree mutate operations in C++ btree implementation.

use super::super::btree_node::{Node, BNodeId, LockType, EMPTY_BNODEID};
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree::Btree;
use super::super::btree_types::BtreeError;
use super::btree_req::{
    BtreeSinglePutRequest, BtreeRangePutRequest, BtreeRequest, BtreePutType, PutFilter,
    PutFilterDecision, PutStats,
};

//================================================================================
// Internal Implementation (called from btree.rs public API)
//================================================================================

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    //================================================================================
    // Single-key PUT Implementation
    //================================================================================

    /// Single-key PUT request with retry loop.
    /// Returns `(PutStats, hit_boundary)`.  `hit_boundary = true` when a scan-and-put
    /// scan range extends beyond the target leaf (caller should schedule deferred cleanup).
    pub(in super::super) async fn put_one_internal<'a>(
        &self,
        mut req: BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<(PutStats, bool), BtreeError> {
        let mut hit_boundary = false;
        loop {
            let tree_lock = self.lock_tree_shared().await;
            let root_id = self.root_node_id();
            let root = self.read_and_lock_node(root_id, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&root, &req) {
                drop(root);
                drop(tree_lock);
                self.check_split_root(&req).await?;
                continue; // Retry from root
            }

            match self.put_one_walk(root, &mut req).await {
                Ok(hb) => { hit_boundary = hb; break; }
                Err(BtreeError::Retry) => continue,
                Err(e) => return Err(e),
            }
        }
        Ok((req.into_stats(), hit_boundary))
    }

    /// Recursive PUT for single-key requests. Returns `hit_boundary`.
    #[cfg_attr(feature = "async_code", async_recursion::async_recursion)]
    async fn put_one_walk<'a>(
        &self,
        mut my_node: Node,
        req: &mut BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<bool, BtreeError> {
        if my_node.is_leaf() {
            return if req.is_scan_put() {
                self.scan_and_put_one_in_leaf(&my_node, req).await
            } else {
                self.put_one_in_leaf(&my_node, req).await.map(|()| false)
            };
        }

        // Interior node: find child and traverse
        loop {
            let (_, idx) = my_node.find::<K, V>(req.key());
            let mut child = self.get_child_and_lock(&my_node, idx, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&child, req) {
                tracing::debug!(parent = my_node.node_id(), child = child.node_id(), idx, "Child needs split");
                (my_node, child) = self.upgrade_node_locks(my_node, child).await?;
                let _split_key = self.split_child_node(&my_node, &child, idx).await?;
                tracing::debug!(split_key=?_split_key, "Split completed, retrying");
                continue;
            }

            drop(my_node);
            return self.put_one_walk(child, req).await;
        }
    }

    /// Write to leaf node for a direct (non-scan) single-key PUT request.
    /// Updates `req.stats` in-place.
    async fn put_one_in_leaf<'a>(
        &self,
        node: &Node,
        req: &mut BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        debug_assert!(node.is_leaf(), "Put operation on node is supported only for leaf nodes");
        debug_assert!(!req.is_scan_put(), "put_one_in_leaf called with scan-and-put request");
        tracing::trace!(node=node.node_id(), key=?req.key(), "Leaf mutation");

        let key = req.key();
        let value = req.value();
        let put_type = req.put_type();
        let filter = req.filter();

        let (found, idx) = node.find::<K, V>(key);

        if found {
            let decision = self.apply_put_filter(node, idx, filter).await?;
            match decision {
                PutFilterDecision::Keep => {
                    req.stats.updated += 1;
                    return Ok(());
                }
                PutFilterDecision::Remove => {
                    // Existing entry removed; no new key inserted.
                    node.remove::<K, V>(idx)?;
                    self.storage.write_node(node).await?;
                    req.stats.removed += 1;
                    return Ok(());
                }
                PutFilterDecision::Replace => {} // Fall through
                PutFilterDecision::NeedOldValue => unreachable!(),
            }
        }

        match put_type {
            BtreePutType::Insert => {
                if found { return Err(BtreeError::KeyAlreadyExists); }
                let v = ValueOrOverflow::build(self.storage.as_ref(), value, self.config.inline_value_size).await?;
                node.insert::<K, V>(idx, key, &v)?;
                req.stats.inserted += 1;
            }
            BtreePutType::Update => {
                if !found { return Err(BtreeError::KeyNotFound); }
                self.replace_value(node, idx, value).await?;
                req.stats.updated += 1;
            }
            BtreePutType::Upsert => {
                if found {
                    self.replace_value(node, idx, value).await?;
                    req.stats.updated += 1;
                } else {
                    let v = ValueOrOverflow::build(self.storage.as_ref(), value, self.config.inline_value_size).await?;
                    node.insert::<K, V>(idx, key, &v)?;
                    req.stats.inserted += 1;
                }
            }
        }

        tracing::debug!(node=node.node_id(), entries=node.total_entries(), "Mutation done");
        self.storage.write_node(node).await?;
        Ok(())
    }

    /// Scan-and-put leaf operation.
    ///
    /// 1. Checks whether `scan_range` extends past this leaf into a sibling → `hit_boundary`.
    /// 2. Scans entries in `scan_range` within this leaf; applies `filter` to each
    ///    (removes eligible old entries in the same `write_node` call).
    /// 3. Stamps `insert_key` via `filter.mutate_key()` inside the leaf write lock.
    /// 4. Inserts the stamped key and writes the node once.
    ///
    /// Updates `req.stats` in-place; returns `hit_boundary`.
    async fn scan_and_put_one_in_leaf<'a>(
        &self,
        node: &Node,
        req: &mut BtreeSinglePutRequest<'a, K, V>,
    ) -> Result<bool, BtreeError> {
        debug_assert!(node.is_leaf(), "Put operation on node is supported only for leaf nodes");

        let (insert_key, scan_range, filter, value, max_scan) =
            req.scan_put_info_mut().expect("scan_and_put_one_in_leaf called without ScanPutInfo");
        tracing::trace!(node=node.node_id(), "Scan-and-put leaf mutation");

        // 1. Determine hit_boundary before any modifications.
        let hit_boundary = node.get_next_node() != EMPTY_BNODEID
            && node
                .get_last_key::<K, V>()
                .map(|last_key| {
                    scan_range.end_key > last_key
                        || (scan_range.end_key == last_key && scan_range.end_incl)
                })
                .unwrap_or(false);

        // 2. Scan scan_range within this leaf; apply filter to each old entry.
        // end_idx is maintained manually: each removal shifts entries down by one,
        // so we decrement end_idx instead of re-calling match_range each iteration.
        let (matched, start_idx, mut end_idx) = node.match_range::<K, V>(scan_range);
        let mut removed: u32 = 0;
        if matched {
            let mut idx = start_idx;
            let mut scanned: usize = 0;
            while idx <= end_idx && scanned < max_scan {
                let decision = self.apply_put_filter(node, idx, filter).await?;
                scanned += 1;
                if decision == PutFilterDecision::Remove {
                    node.remove::<K, V>(idx)?;
                    removed += 1;
                    match end_idx.checked_sub(1) {
                        Some(new_end) => end_idx = new_end,
                        None => break,
                    }
                    // Don't increment idx: the next entry has shifted into position idx.
                } else {
                    idx += 1;
                }
            }
        }

        // 3. Stamp the insert key inside the write lock.
        if let Some(f) = filter {
            f.mutate_key(insert_key);
        }

        // 4. Insert the stamped key and write the node once.
        let (_, insert_idx) = node.find::<K, V>(insert_key);
        let v = ValueOrOverflow::build(self.storage.as_ref(), value, self.config.inline_value_size).await?;
        node.insert::<K, V>(insert_idx, insert_key, &v)?;

        tracing::debug!(node=node.node_id(), entries=node.total_entries(), hit_boundary, removed, "Scan-and-put done");
        self.storage.write_node(node).await?;

        req.stats.inserted += 1;
        req.stats.removed += removed;
        Ok(hit_boundary)
    }

    //================================================================================
    // Range PUT Implementation
    //================================================================================

    /// Range PUT request with retry loop.
    /// Returns `PutStats` accumulated across all touched leaves.
    pub(in super::super) async fn put_range_internal<'a>(
        &self,
        mut req: BtreeRangePutRequest<'a, K, V>,
    ) -> Result<PutStats, BtreeError> {
        loop {
            let tree_lock = self.lock_tree_shared().await;
            let root_id = self.root_node_id();
            let root = self.read_and_lock_node(root_id, LockType::ReadInteriorWriteLeaf).await?;

            if self.is_split_needed(&root, &req) {
                drop(root);
                drop(tree_lock);
                self.check_split_root(&req).await?;
                continue;
            }

            match self.put_range_walk(root, &mut req).await {
                Ok(()) => return Ok(req.into_stats()),
                Err(BtreeError::Retry) => continue,
                Err(e) => return Err(e),
            }
        }
    }

    /// Main PUT recursive traversal for range operations.
    /// Updates `req.stats` in-place as leaves are processed.
    #[cfg_attr(feature = "async_code", async_recursion::async_recursion)]
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
            let working_range = req.working_range().clone();
            let (matched, start_idx, end_idx) = my_node.match_range::<K, V>(&working_range);
            if !matched {
                return Err(BtreeError::KeyNotFound);
            }

            let mut curr_idx = start_idx;
            while curr_idx <= end_idx {
                let mut child = self.get_child_and_lock(&my_node, curr_idx, LockType::ReadInteriorWriteLeaf).await?;

                if self.is_split_needed(&child, req) {
                    (my_node, child) = self.upgrade_node_locks(my_node, child).await?;
                    let _split_key = self.split_child_node(&my_node, &child, curr_idx).await?;
                    continue 'outer;
                }

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

    /// Write to leaf node for range operations.
    /// Updates `req.stats` in-place.
    async fn put_range_in_leaf<'a>(
        &self,
        node: &Node,
        req: &mut BtreeRangePutRequest<'a, K, V>,
    ) -> Result<(), BtreeError> {
        let put_type = BtreePutType::Update; // Range operations always update
        debug_assert!(node.is_leaf(), "Multi put only for leaf nodes");

        let (matched, start_idx, end_idx) = node.match_range::<K, V>(req.working_range());
        if !matched {
            return Err(BtreeError::KeyNotFound);
        }

        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap_or(0);
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap_or(0);
        let new_value = req.value();
        let filter = req.filter();

        let mut idx = start_idx;
        let mut last_failed_key: Option<K> = None;
        let mut n_updated: u32 = 0;
        let mut n_removed: u32 = 0;

        while idx <= end_idx {
            if !node.has_room_for_put::<K, V>(put_type, key_size, val_size) {
                last_failed_key = Some(node.get_nth_key::<K, V>(idx, /* copy= */ true));
                break;
            }

            let decision = self.apply_put_filter(node, idx, filter).await?;
            match decision {
                PutFilterDecision::Keep => {
                    idx += 1;
                }
                PutFilterDecision::Replace => {
                    self.replace_value(node, idx, new_value).await?;
                    n_updated += 1;
                    idx += 1;
                }
                PutFilterDecision::Remove => {
                    node.remove::<K, V>(idx)?;
                    n_removed += 1;
                    // Don't increment idx — next entry shifts down.
                }
                PutFilterDecision::NeedOldValue => unreachable!(),
            }
        }
        // new_value and filter borrows end at their last use above; safe to mutably borrow req now.
        req.stats.updated += n_updated;
        req.stats.removed += n_removed;

        req.shift_working_range(last_failed_key);
        self.storage.write_node(node).await?;
        Ok(())
    }

    //================================================================================
    // Split related methods
    //================================================================================

    fn is_split_needed<ReqT>(&self, node: &Node, req: &ReqT) -> bool
    where
        ReqT: BtreeRequest + ?Sized,
    {
        if !node.is_leaf() {
            return !node.has_room_for_put::<K, V>(
                BtreePutType::Upsert,
                self.config.max_key_size(),
                std::mem::size_of::<BNodeId>() as u32,
            );
        }
        !node.has_room_for_put::<K, V>(req.put_type(), req.key_size(), req.value_size())
    }

    async fn check_split_root<ReqT>(&self, req: &ReqT) -> Result<(), BtreeError>
    where
        ReqT: BtreeRequest,
    {
        let _tree_lock = self.lock_tree_exclusive().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Write).await?;

        if !self.is_split_needed(&root, req) {
            tracing::debug!(node_id = root_id, "Root split no longer needed");
            return Ok(());
        }

        tracing::debug!(
            root_id,
            level = root.level(),
            entries = root.total_entries(),
            "Root is full, creating new root"
        );

        let new_root = self.create_interior_node(root.node_variant()).await?;
        new_root.set_level(root.level() + 1);
        tracing::debug!(new_root_id = new_root.node_id(), "Created new root");

        let child_node = root;
        let split_result = self.split_child_node(&new_root, &child_node, new_root.total_entries()).await;
        match split_result {
            Ok(_split_key) => {
                tracing::info!(old_root=root_id, new_root=new_root.node_id(),
                               split_key=?_split_key, "Root split successful");
                self.root_node_id.store(new_root.node_id(), std::sync::atomic::Ordering::Relaxed);
                self.storage.on_root_changed(new_root.node_id()).await?;
                Ok(())
            }
            Err(e) => {
                tracing::error!(error=?e, "Root split failed");
                self.storage.delete_node(new_root.node_id()).await?;
                Err(e)
            }
        }
    }

    async fn split_child_node(&self, parent_node: &Node, child_node: &Node, parent_idx: u32) -> Result<K, BtreeError> {
        debug_assert_eq!(parent_node.lock_type(), LockType::Write);
        debug_assert_eq!(child_node.lock_type(), LockType::Write);

        let child_node1 = child_node;

        let child_node2 = match child_node1.is_leaf() {
            true  => self.create_leaf_node(child_node1.node_variant()).await?,
            false => self.create_interior_node(child_node1.node_variant()).await?,
        };

        child_node2.set_next_node(child_node1.get_next_node());
        child_node1.set_next_node(child_node2.node_id());
        child_node2.set_level(child_node1.level());

        let child1_filled_size = child_node1.node_data_size() - child_node1.available_size::<K, V>();
        let split_size = self.config.split_size(child1_filled_size);

        let moved = child_node1.move_out_to_right_by_size::<K, V>(&child_node2, split_size);
        debug_assert!(moved > 0, "Unable to split entries in the child node");
        debug_assert!(child_node1.total_entries() > 0, "Child node1 should have entries after split");

        let split_key: K = child_node1.get_last_key::<K, V>().expect("Child node1 should have entries after split");

        tracing::debug!(parent=parent_node.node_id(), idx=parent_idx,
                        child1=child_node1.node_id(), child2=child_node2.node_id(),
                        split_key=?split_key, c1_entries=child_node1.total_entries(),
                        c2_entries=child_node2.total_entries(), moved, "Split done");

        parent_node.update_child::<K>(parent_idx, &child_node2.node_id())?;
        parent_node.insert_child::<K>(parent_idx, &split_key, &child_node1.node_id())?;
        tracing::trace!(idx=parent_idx, key=?split_key, child1=child_node1.node_id(),
                        child2=child_node2.node_id(), "Updated parent: insert+update done");

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

        if node.is_nth_value_overflow::<K, V>(idx) {
            let old_val = node.get_nth_value::<K, V>(idx, false);
            old_overflow_id = Some(old_val.expect_overflow("Expected overflow value, got inline value"));
        }

        let v = ValueOrOverflow::build(self.storage.as_ref(), value, self.config.inline_value_size).await?;
        node.update::<K, V>(idx, &v)?;

        if let Some(old_id) = old_overflow_id {
            self.storage.delete_overflow(old_id).await?;
        }

        Ok(())
    }
}
