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

//! Btree Query Operations
//!
//! This module contains GET, GET_ANY, and QUERY operations.
//! Corresponds to btree_get_impl.ipp and btree_query_impl.ipp in C++ btree implementation.

use super::super::btree_node::{Node, LockType, EMPTY_BNODEID, PaginationStatus};
use super::super::btree_kvs::{BtreeKey, BtreeValue};
use super::super::btree::{BtreeError, Btree};
use super::btree_req::{BtreeGetRequest, BtreeGetAnyRequest, BtreeKeyRange, BtreeQueryRequest, QueryResultHandle};

impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    //================================================================================
    // Single key GET Implementation
    //================================================================================
    
    /// Single key GET (public API wrapper)
    pub(in super::super) async fn get_one_request<'a>(&self, req: &'a BtreeGetRequest<'a, K>) 
        -> Result<Option<V>, BtreeError> {
        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;
        
        self.do_get(root, req).await
    }
    
    /// Recursive GET traversal
    async fn do_get<'a>(&self, node: Node, req: &'a BtreeGetRequest<'a, K>) -> Result<Option<V>, BtreeError> {
        if node.is_leaf() {
            return self.get_from_leaf(&node, req.key());
        }
        
        // Interior node: find child and traverse
        let (_, idx) = node.find::<K, V>(req.key());
        let child_id = node.get_nth_child_id::<K>(idx);
        let child = self.read_and_lock_node(child_id, LockType::Read).await?;
        
        drop(node); // Release parent lock
        Box::pin(self.do_get(child, req)).await
    }
    
    /// Read value from leaf node
    fn get_from_leaf(&self, node: &Node, key: &K) -> Result<Option<V>, BtreeError> {
        debug_assert!(node.is_leaf());
        
        let (found, idx) = node.find::<K, V>(key);
        if found {
            let value = node.get_nth_value::<K, V>(idx, /*copy=*/true);
            Ok(Some(value))
        } else {
            Ok(None)
        }
    }
    
    //================================================================================
    // GET_ANY Implementation (optimization for range queries)
    //================================================================================
    
    /// Get any key in range (returns first found)
    pub(in super::super) async fn get_any_request(&self, req: &BtreeGetAnyRequest<K>) 
        -> Result<Option<(K, V)>, BtreeError> {
        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;
        
        self.do_get_any(root, req).await
    }
    
    /// Recursive GET_ANY traversal
    async fn do_get_any(&self, node: Node, req: &BtreeGetAnyRequest<K>) -> Result<Option<(K, V)>, BtreeError> {
        if node.is_leaf() {
            return self.get_any_from_leaf(&node, req.range());
        }
        
        // Interior node: match range and pick first child
        let (matched, start_idx, _) = node.match_range::<K, V>(req.range());
        if !matched {
            return Ok(None);
        }
        
        let child_id = node.get_nth_child_id::<K>(start_idx);
        let child = self.read_and_lock_node(child_id, LockType::Read).await?;
        
        drop(node);
        Box::pin(self.do_get_any(child, req)).await
    }
    
    /// Get any key-value from leaf in range
    fn get_any_from_leaf(&self, node: &Node, range: &BtreeKeyRange<K>) -> Result<Option<(K, V)>, BtreeError> {
        debug_assert!(node.is_leaf());
        
        let (matched, start_idx, _) = node.match_range::<K, V>(range);
        if matched {
            let key = node.get_nth_key::<K, V>(start_idx, /*copy=*/true);
            let value = node.get_nth_value::<K, V>(start_idx, /*copy=*/true);
            Ok(Some((key, value)))
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
    pub(in super::super) async fn query_request(&self, req: BtreeQueryRequest<K>) 
        -> Result<QueryResultHandle<K, V>, BtreeError> {       
        if req.batch_size() == 0 {
            return Ok(QueryResultHandle::new(Vec::new(), req, false));
        }
        
        let _tree_lock = self.lock_tree_shared().await;
        let root_id = self.root_node_id();
        let root = self.read_and_lock_node(root_id, LockType::Read).await?;
        
        let mut results = Vec::new();
        let ret = self.do_sweep_query(root, &req, &mut results).await;
        
        // Determine if there are more results
        let has_more = matches!(ret, Ok(true));
        
        // Shift working range if we have results and has_more
        if !results.is_empty() && has_more {
            let last_key = &results.last().unwrap().0;
            // Shift past the last returned key (exclusive)
            req.shift_working_range(Clone::clone(last_key), /*start_incl=*/false);
        } else if has_more {
            // Should not happen: HasMore without results
            debug_assert!(false, "Query returned has_more, but no values added");
            return Err(BtreeError::Io(std::io::Error::new(std::io::ErrorKind::Other,
                "Query returned has_more, but no values added")));
        }
        
        Ok(QueryResultHandle::new(results, req, has_more))
    }
   
    /// Sweep query implementation - follows C++ do_sweep_query
    /// 
    /// Corresponds to C++ btree_query_impl.ipp::do_sweep_query() (lines 72-130)
    /// 
    /// Recursively descends to leaf level, then uses multi_get() to extract entries
    /// and follows sibling links to collect up to batch_size results.
    async fn do_sweep_query(&self, mut my_node: Node, req: &BtreeQueryRequest<K>, 
                            out_values: &mut Vec<(K, V)>) -> Result<bool, BtreeError> {
        if my_node.is_leaf() {
            // Leaf node: use multi_get and follow sibling links (C++ lines 75-116)
            let mut count = out_values.len() as u32;
            
            loop {
                // Call multi_get on current leaf
                let remaining = req.batch_size().saturating_sub(count);
                let (cur_count, pagination_status) = my_node.multi_get::<K, V>(&req.working_range().clone(),
                                                           remaining, out_values, /*filter_fn=*/None);
                count += cur_count;
                
                // Handle pagination status
                match pagination_status {
                    PaginationStatus::Completed => {
                        return Ok(/*has_more=*/false);  // Range query completed - no more results
                    }
                    PaginationStatus::Continue => {
                        // Stopped due to max_count - assert and return HasMore
                        debug_assert_eq!(count, req.batch_size(), 
                            "Continue status but count {} != batch_size {}", count, req.batch_size());
                        return Ok(/*has_more=*/true);
                    }
                    PaginationStatus::Unknown => {
                        // Reached end of node - check if batch is full or continue to sibling
                        if count >= req.batch_size() {
                            return Ok(/*has_more=*/true);
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
            
            return Ok(/*has_more=*/false);
        }
        
        // Interior node: find first matching child and descend (C++ lines 119-129)
        let (_, idx) = my_node.find::<K, V>(&req.first_key());
        let child_id = my_node.get_nth_child_id::<K>(idx);
        let child = self.read_and_lock_node(child_id, LockType::Read).await?;
        
        drop(my_node); // Release parent lock
        Box::pin(self.do_sweep_query(child, req, out_values)).await
    }
}
