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

//! Btree Query Operations
//!
//! This module contains GET, GET_ANY, and QUERY operations.
//! Corresponds to btree_get_impl.ipp and btree_query_impl.ipp in C++ btree implementation.

use super::super::btree_node::{Node, LockType, EMPTY_BNODEID, PaginationStatus};
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree::Btree;
use super::super::btree_types::BtreeError;
use super::btree_req::{
    BtreeGetRequest, BtreeGetAnyRequest, BtreeKeyRange, BtreeQueryRequest, QueryResultHandle, GetFilter,
    GetFilterDecision,
};

//================================================================================
// Helper for multi-get operations
//================================================================================

impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    //================================================================================
    // Single key GET Implementation
    //================================================================================

    /// Single key GET (public API wrapper)
    pub(in super::super) async fn get_one_internal<'a>(
        &self,
        req: &'a BtreeGetRequest<'a, K>,
    ) -> Result<Option<V>, BtreeError> {
        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;

        self.get_one_walk(root, req).await
    }

    /// Recursive GET traversal
    async fn get_one_walk<'a>(&self, node: Node, req: &'a BtreeGetRequest<'a, K>) -> Result<Option<V>, BtreeError> {
        if node.is_leaf() {
            return self.get_one_in_leaf(&node, req.key()).await;
        }

        // Interior node: find child and traverse
        let (_, idx) = node.find::<K, V>(req.key());
        let child_id = node.get_nth_child_id::<K>(idx);
        let child = self.read_and_lock_node(child_id, LockType::Read).await?;

        drop(node); // Release parent lock
        Box::pin(self.get_one_walk(child, req)).await
    }

    /// Read value from leaf node (with overflow resolution)
    async fn get_one_in_leaf(&self, node: &Node, key: &K) -> Result<Option<V>, BtreeError> {
        debug_assert!(node.is_leaf());

        let (found, idx) = node.find::<K, V>(key);
        if found {
            let copy = true;
            let value = node.get_nth_value::<K, V>(idx, copy).resolve(self.storage.as_ref(), copy).await?;
            Ok(Some(value))
        } else {
            Ok(None)
        }
    }

    //================================================================================
    // Multi-GET: Extract multiple KVs from leaf with overflow and filtering
    //================================================================================

    /// Extract multiple key-value pairs from leaf node in range
    ///
    /// Forward iteration with:
    /// - Async overflow value resolution
    /// - Two-phase filtering (check_key, then check_kv)
    /// - Pagination status tracking
    ///
    /// Returns (count, pagination_status)
    async fn multi_get_from_leaf(
        &self,
        node: &Node,
        range: &BtreeKeyRange<K>,
        max_count: u32,
        out_values: &mut Vec<(K, V)>,
        filter: Option<&dyn GetFilter<K, V>>,
        reverse: bool,
    ) -> (u32, PaginationStatus) {
        debug_assert!(node.is_leaf(), "multi_get only for leaf nodes");

        let (matched, start_idx, end_idx) = node.match_range::<K, V>(range);
        if !matched {
            return (0, PaginationStatus::Completed);
        }

        let nentries = node.total_entries();
        let last_idx_in_node = if nentries > 0 { nentries - 1 } else { 0 };

        let mut count = 0u32;
        let indices: Vec<u32> =
            if reverse { (start_idx..=end_idx).rev().collect() } else { (start_idx..=end_idx).collect() };

        let mut last_processed_idx = if reverse { end_idx } else { start_idx };

        for idx in indices {
            if count >= max_count {
                break;
            }
            last_processed_idx = idx;
            let key = node.get_nth_key::<K, V>(idx, /* copy= */ true);

            let (decision, mut value) = match self.apply_get_filter(node, idx, filter).await {
                Ok(result) => result,
                Err(_) => return (count, PaginationStatus::Completed),
            };

            if decision == GetFilterDecision::Skip {
                continue;
            }

            if value.is_none() {
                let copy = true;
                match node.get_nth_value::<K, V>(idx, copy).resolve(self.storage.as_ref(), copy).await {
                    Ok(v) => value = Some(v),
                    Err(_) => return (count, PaginationStatus::Completed),
                }
            }
            out_values.push((key, value.unwrap()));
            count += 1;
        }

        // Determine pagination status
        let status = if reverse {
            if start_idx != 0 {
                if last_processed_idx <= start_idx {
                    PaginationStatus::Completed
                } else {
                    PaginationStatus::Continue
                }
            } else {
                if count > 0 {
                    let last_key = &out_values.last().unwrap().0;
                    if last_key <= &range.start_key { PaginationStatus::Completed } else { PaginationStatus::Unknown }
                } else {
                    PaginationStatus::Unknown
                }
            }
        } else {
            if end_idx != last_idx_in_node {
                if last_processed_idx >= end_idx { PaginationStatus::Completed } else { PaginationStatus::Continue }
            } else {
                if count > 0 {
                    let last_key = &out_values.last().unwrap().0;
                    if last_key >= &range.end_key { PaginationStatus::Completed } else { PaginationStatus::Unknown }
                } else {
                    PaginationStatus::Unknown
                }
            }
        };

        (count, status)
    }

    async fn apply_get_filter(
        &self,
        node: &Node,
        idx: u32,
        filter: Option<&dyn GetFilter<K, V>>,
    ) -> Result<(GetFilterDecision, Option<V>), BtreeError> {
        if filter.is_none() {
            return Ok((GetFilterDecision::Include, None));
        }

        let filter = filter.unwrap();
        let key = node.get_nth_key::<K, V>(idx, /* copy= */ false);

        if !filter.always_needs_value() {
            let decision = filter.check_key(&key);
            if decision != GetFilterDecision::NeedValue {
                return Ok((decision, None));
            }
        }

        let copy = true; // we might use this value for query result, so copy it
        let old_val = node.get_nth_value::<K, V>(idx, copy).resolve(self.storage.as_ref(), copy).await?;
        Ok((filter.check_kv(&key, &old_val), Some(old_val)))
    }

    //================================================================================
    // GET_ANY Implementation (optimization for range queries)
    //================================================================================

    /// Get any key in range (returns first found)
    pub(in super::super) async fn get_any_internal(
        &self,
        req: &BtreeGetAnyRequest<K>,
    ) -> Result<Option<(K, V)>, BtreeError> {
        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;

        self.get_any_walk(root, req).await
    }

    /// Recursive GET_ANY traversal
    async fn get_any_walk(&self, node: Node, req: &BtreeGetAnyRequest<K>) -> Result<Option<(K, V)>, BtreeError> {
        if node.is_leaf() {
            let result = self.get_any_in_leaf(&node, req.range())?;
            if let Some((key, value_ref)) = result {
                let value = value_ref.resolve(self.storage.as_ref(), true).await?;
                return Ok(Some((key, value)));
            }
            return Ok(None);
        }

        // Interior node: match range and pick first child
        let (matched, start_idx, _) = node.match_range::<K, V>(req.range());
        if !matched {
            return Ok(None);
        }

        let child_id = node.get_nth_child_id::<K>(start_idx);
        let child = self.read_and_lock_node(child_id, LockType::Read).await?;

        drop(node);
        Box::pin(self.get_any_walk(child, req)).await
    }

    /// Get any key-value from leaf in range
    fn get_any_in_leaf(
        &self,
        node: &Node,
        range: &BtreeKeyRange<K>,
    ) -> Result<Option<(K, ValueOrOverflow<V>)>, BtreeError> {
        debug_assert!(node.is_leaf());

        let (matched, start_idx, _) = node.match_range::<K, V>(range);
        if matched {
            let key = node.get_nth_key::<K, V>(start_idx, /* copy= */ true);
            let value_ref = node.get_nth_value::<K, V>(start_idx, /* copy= */ true);
            Ok(Some((key, value_ref)))
        } else {
            Ok(None)
        }
    }

    //================================================================================
    // QUERY Implementation (Sweep Query with Sibling Links)
    //================================================================================

    /// Sweep query - returns multiple key-value pairs by following sibling links
    ///
    /// Corresponds to C++ Btree::query() with SWEEP_NON_INTRUSIVE_PAGINATION_QUERY
    ///
    /// # Arguments
    /// * `req` - Query request with range and batch_size
    ///
    /// # Returns
    /// * `Ok(QueryResultHandle)` - Handle with results and has_more() indicator
    /// * `Err(BtreeError)` - Internal errors (not HasMore, which is converted to handle.has_more())
    pub(in super::super) async fn query_internal<'a>(
        &self,
        mut req: BtreeQueryRequest<'a, K, V>,
    ) -> Result<QueryResultHandle<'a, K, V>, BtreeError> {
        if req.batch_size() == 0 {
            return Ok(QueryResultHandle::new(Vec::new(), req, false));
        }

        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;

        let mut results = Vec::new();
        let ret = self.sweep_query_walk(root, &mut req, &mut results).await;

        // Determine if there are more results
        let has_more = matches!(ret, Ok(true));

        // Shift working range if we have results and has_more
        if !results.is_empty() && has_more {
            let last_key = &results.last().unwrap().0;
            // Shift past the last returned key (exclusive)
            req.shift_working_range(Clone::clone(last_key), /* start_incl= */ false);
        } else if has_more {
            // Should not happen: HasMore without results
            debug_assert!(false, "Query returned has_more, but no values added");
            return Err(BtreeError::Io(std::io::Error::new(
                std::io::ErrorKind::Other,
                "Query returned has_more, but no values added",
            )));
        }

        Ok(QueryResultHandle::new(results, req, has_more))
    }

    /// Sweep query implementation -
    ///
    ///
    /// Recursively descends to leaf level, then uses multi_get() to extract entries
    /// and follows sibling links to collect up to batch_size results.
    async fn sweep_query_walk<'a>(
        &self,
        mut my_node: Node,
        req: &mut BtreeQueryRequest<'a, K, V>,
        out_values: &mut Vec<(K, V)>,
    ) -> Result<bool, BtreeError> {
        if my_node.is_leaf() {
            // Leaf node: use multi_get and follow sibling links (C++ lines 75-116)
            let mut count = out_values.len() as u32;

            loop {
                // Call multi_get on current leaf
                let remaining = req.batch_size().saturating_sub(count);
                let (cur_count, pagination_status) = self
                    .multi_get_from_leaf(
                        &my_node,
                        req.working_range(),
                        remaining,
                        out_values,
                        req.filter(),
                        req.reverse_order(),
                    )
                    .await;
                count += cur_count;

                // Handle pagination status
                match pagination_status {
                    PaginationStatus::Completed => {
                        return Ok(/* has_more= */ false); // Range query completed - no more results
                    }
                    PaginationStatus::Continue => {
                        // Stopped due to max_count - assert and return HasMore
                        debug_assert_eq!(count, req.batch_size(), "Continue status but count != batch_size");
                        return Ok(/* has_more= */ true);
                    }
                    PaginationStatus::Unknown => {
                        // Reached end of node - check if batch is full or continue to sibling
                        if count >= req.batch_size() {
                            return Ok(/* has_more= */ true);
                        }
                        // Otherwise, try sibling node
                    }
                }

                let next_id = my_node.get_next_node();
                if next_id == EMPTY_BNODEID {
                    break;
                }

                // Read next sibling
                let next_node = self.read_and_lock_node(next_id, LockType::Read).await?;
                drop(my_node); // Release current node lock
                my_node = next_node;
            }

            return Ok(/* has_more= */ false);
        }

        // Interior node: find first matching child and descend (C++ lines 119-129)
        let (_, idx) = my_node.find::<K, V>(&req.first_key());
        let child_id = my_node.get_nth_child_id::<K>(idx);
        let child = self.read_and_lock_node(child_id, LockType::Read).await?;

        drop(my_node); // Release parent lock
        Box::pin(self.sweep_query_walk(child, req, out_values)).await
    }

    //================================================================================
    // Traversal QUERY Implementation (Required for Reverse Iteration)
    //================================================================================

    /// Traversal query - walks tree parent-to-leaf repeatedly without sibling links
    ///
    /// This method is required for reverse iteration to avoid deadlock. It maintains
    /// strict parent-before-child locking and never follows horizontal sibling links.
    /// Instead, it walks from parent to leaf, processes entries, returns to parent,
    /// and repeats for the next child.
    ///
    /// Corresponds to C++ Btree::do_traversal_query() (lines 133-194)
    ///
    /// # Arguments
    /// * `my_node` - Current node being processed
    /// * `req` - Query request with reverse_order flag
    /// * `out_values` - Vector to accumulate results
    ///
    /// # Returns
    /// * `Ok(true)` - Has more results (batch_size reached)
    /// * `Ok(false)` - No more results (range exhausted)
    /// * `Err(BtreeError)` - Internal error
    pub(in super::super) async fn traversal_query_internal<'a>(
        &self,
        mut req: BtreeQueryRequest<'a, K, V>,
    ) -> Result<QueryResultHandle<'a, K, V>, BtreeError> {
        if req.batch_size() == 0 {
            return Ok(QueryResultHandle::new(Vec::new(), req, false));
        }

        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;

        let mut results = Vec::new();
        let has_more = self.traversal_query_walk(root, &mut req, &mut results).await?;

        // Shift working range if we have results and has_more
        if !results.is_empty() && has_more {
            let last_key = &results.last().unwrap().0;
            // For reverse, shift to before last key; for forward, shift past last key
            req.shift_working_range(Clone::clone(last_key), /* start_incl= */ false);
        } else if has_more {
            // Should not happen: HasMore without results
            debug_assert!(false, "Query returned has_more, but no values added");
            return Err(BtreeError::Io(std::io::Error::new(
                std::io::ErrorKind::Other,
                "Query returned has_more, but no values added",
            )));
        }

        Ok(QueryResultHandle::new(results, req, has_more))
    }

    /// Recursive traversal query walk
    async fn traversal_query_walk<'a>(
        &self,
        my_node: Node,
        req: &mut BtreeQueryRequest<'a, K, V>,
        out_values: &mut Vec<(K, V)>,
    ) -> Result<bool, BtreeError> {
        if my_node.is_leaf() {
            // Leaf node: use multi_get
            let remaining = req.batch_size().saturating_sub(out_values.len() as u32);
            let (_cur_count, _pagination_status) = self
                .multi_get_from_leaf(
                    &my_node,
                    req.working_range(),
                    remaining,
                    out_values,
                    req.filter(),
                    req.reverse_order(),
                )
                .await;

            drop(my_node);

            // Check if we have enough results
            if out_values.len() as u32 >= req.batch_size() {
                return Ok(true); // has_more
            }

            return Ok(false); // no more in this leaf
        }

        // Interior node: find child range (C++ lines 157-168)
        let (_start_found, start_idx) = my_node.find::<K, V>(&req.first_key());
        let (_end_found, mut end_idx) = my_node.find::<K, V>(&req.working_range().end_key);

        let nentries = my_node.total_entries();
        let has_edge = my_node.core.get_persistent_header().edge_id != EMPTY_BNODEID;

        // Handle edge cases (C++ lines 161-165)
        if start_idx == nentries && !has_edge {
            drop(my_node);
            return Ok(false); // no results found
        }
        if end_idx == nentries && !has_edge {
            end_idx = end_idx.saturating_sub(1);
        }

        // Iterate through children (C++ lines 167-189)
        let mut idx = if req.reverse_order() {
            // REVERSE: Start from end_idx, go down to start_idx
            end_idx
        } else {
            // FORWARD: Start from start_idx, go up to end_idx
            start_idx
        };

        let mut my_node = Some(my_node);

        loop {
            // Determine if this is the last child we'll visit
            let is_last = if req.reverse_order() { idx == start_idx } else { idx == end_idx };

            // Get child at current index (node must still be valid here)
            let node_ref = my_node.as_ref().unwrap();
            let child_id = node_ref.get_nth_child_id::<K>(idx);
            let child = self.read_and_lock_node(child_id, LockType::Read).await?;

            // Unlock parent at last index (C++ lines 179-184)
            if is_last {
                drop(my_node.take());
            }

            // Recurse into child
            let has_more = Box::pin(self.traversal_query_walk(child, req, out_values)).await?;

            if has_more {
                // Parent already dropped if is_last, otherwise need to drop
                if let Some(node) = my_node.take() {
                    drop(node);
                }
                return Ok(true); // propagate has_more
            }

            // If this was the last child, we already dropped parent
            if is_last {
                break;
            }

            // Move to next child
            if req.reverse_order() {
                idx -= 1;
            } else {
                idx += 1;
            }
        }

        Ok(false) // no more results
    }
}
