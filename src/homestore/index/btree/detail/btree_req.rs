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

//! Btree Request Abstraction
//!
//! This module defines the request types for btree operations, matching C++ btree_req.hpp structure.
//! 
//! Structure:
//! - BtreeRequest: Base trait (empty for now)
//! - BtreeRangeRequest: Base for range operations (input_range, working_range, batch_size)
//! - BtreeRangePutRequest: Range PUT (embeds BtreeRangeRequest + put_type, value)
//! - BtreeSinglePutRequest: Single PUT

use super::super::btree_kvs::{BtreeKey, BtreeValue};

//================================================================================
// Filter Types (matches C++ btree_kv.hpp filter callbacks)
//================================================================================

/// Filter decision for PUT operations (matches C++ put_filter_decision)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PutFilterDecision {
    /// Don't modify the entry, keep existing value
    Keep,
    /// Update with new value
    Replace,
    /// Delete the entry
    Remove,
}

/// PUT filter function: receives (key, existing_value, new_value) -> decision
/// Matches C++ put_filter_cb_t
pub type PutFilterFn<K, V> = dyn Fn(&K, &V, &V) -> PutFilterDecision + Send + Sync;

/// REMOVE filter function: receives (key, value) -> bool (true = remove)
/// Matches C++ remove_filter_cb_t
pub type RemoveFilterFn<K, V> = dyn Fn(&K, &V) -> bool + Send + Sync;

/// GET/QUERY filter function: receives (key, value) -> bool (true = include)
/// Matches C++ get_filter_cb_t
pub type GetFilterFn<K, V> = dyn Fn(&K, &V) -> bool + Send + Sync;

/// Key range for range operations (matches C++ BtreeKeyRange)
#[derive(Debug, Clone)]
pub struct BtreeKeyRange<K: BtreeKey> {
    pub start_key: K,
    pub end_key: K,
    pub start_incl: bool,
    pub end_incl: bool,
}

impl<K: BtreeKey> BtreeKeyRange<K> {
    pub fn new(start_key: K, start_incl: bool, end_key: K, end_incl: bool) -> Self {
        Self { start_key, end_key, start_incl, end_incl }
    }
}

/// Put operation type (matches C++ btree_put_type)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BtreePutType {
    /// Insert only - fail if key exists
    Insert,
    /// Update only - fail if key doesn't exist
    Update,
    /// Insert or update (default)
    Upsert,
}

/// Base trait for all btree requests (matches C++ BtreeRequest)
///
/// Provides common methods needed for split decisions and operation type identification.
pub trait BtreeRequest: Send {
    /// Get the PUT type (Insert, Update, Upsert)
    fn put_type(&self) -> BtreePutType;
    
    /// Get key size for split check
    /// - For single requests: returns the single key size
    /// - For range requests: returns first key size (since we check if at least one entry fits)
    fn key_size(&self) -> u32;
    
    /// Get value size for split check
    fn value_size(&self) -> u32;
}

/// Base for all range operations (matches C++ BtreeRangeRequest + BtreeTraversalState)
///
/// Contains:
/// - input_range: Original range (never modified)
/// - working_range: Current subrange being processed (modified during traversal)
/// - batch_size: Maximum number of entries to process per operation
pub struct BtreeRangeRequest<K: BtreeKey> {
    input_range: BtreeKeyRange<K>,
    working_range: BtreeKeyRange<K>,
    batch_size: u32,
}

impl<K: BtreeKey> BtreeRangeRequest<K> {
    pub fn new(input_range: BtreeKeyRange<K>, batch_size: u32) -> Self {
        let working_range = input_range.clone();
        Self {
            input_range,
            working_range,
            batch_size,
        }
    }

    /// Get the original input range (never changes)
    pub fn input_range(&self) -> &BtreeKeyRange<K> {
        &self.input_range
    }

    /// Get the current working range (may be trimmed/shifted during traversal)
    pub fn working_range(&self) -> &BtreeKeyRange<K> {
        &self.working_range
    }

    /// Get batch size
    pub fn batch_size(&self) -> u32 {
        self.batch_size
    }

    /// Set batch size
    pub fn set_batch_size(&mut self, size: u32) {
        self.batch_size = size;
    }

    /// Get the first key in working range
    pub fn first_key(&self) -> K {
        self.working_range.start_key.clone()
    }
    
    /// Get the serialized size of the first key
    pub fn first_key_size(&self) -> u32 {
        self.working_range.start_key.serialized_size()
    }

    /// Trim working range to child's boundary
    ///
    /// Used when descending to a child node - limits the working range to not exceed
    /// the child's key boundary.
    pub fn trim_working_range(&mut self, end_key: K, end_incl: bool) {
        self.working_range.end_key = end_key;
        self.working_range.end_incl = end_incl;
    }

    /// Shift working range forward
    ///
    /// # Arguments
    /// * `start_key` - If Some(key), set working start to this key.
    ///                 If None, set working start to current working end (continue from where we left off)
    ///
    /// In both cases, working end is reset to input_range.end
    pub fn shift_working_range(&mut self, new_start_key: Option<K>) {
        if let Some(key) = new_start_key {
            // Explicit key provided - use it as new start
            self.working_range.start_key = key;
            self.working_range.start_incl = true;
        } else {
            // No key - continue from current working end
            let old_end = self.working_range.end_key.clone();
            let old_end_incl = self.working_range.end_incl;
            self.working_range.start_key = old_end;
            self.working_range.start_incl = !old_end_incl; // Flip inclusivity
        }
        
        // Reset end to input range end
        self.working_range.end_key = self.input_range.end_key.clone();
        self.working_range.end_incl = self.input_range.end_incl;
    }
}

/// Single key-value PUT request (matches C++ BtreeSinglePutRequest)
pub struct BtreeSinglePutRequest<'a, K: BtreeKey, V: BtreeValue> {
    key: &'a K,
    value: &'a V,
    put_type: BtreePutType,
    filter_fn: Option<&'a PutFilterFn<K, V>>,
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeSinglePutRequest<'a, K, V> {
    /// Create a new single PUT request (matches C++ constructor)
    ///
    /// # Arguments
    /// * `key` - Key to insert/update
    /// * `value` - Value to insert/update
    /// * `put_type` - Type of put operation (Insert/Update/Upsert)
    /// * `filter_fn` - Optional filter function for conditional updates
    pub fn new(
        key: &'a K,
        value: &'a V,
        put_type: BtreePutType,
        filter_fn: Option<&'a PutFilterFn<K, V>>,
    ) -> Self {
        Self {
            key,
            value,
            put_type,
            filter_fn,
        }
    }

    pub fn key(&self) -> &K {
        self.key
    }

    pub fn value(&self) -> &V {
        self.value
    }

    pub fn put_type(&self) -> BtreePutType {
        self.put_type
    }

    pub fn filter_fn(&self) -> Option<&PutFilterFn<K, V>> {
        self.filter_fn
    }

    pub fn key_size(&self) -> u32 {
        self.key.serialized_size()
    }

    pub fn value_size(&self) -> u32 {
        self.value.serialized_size()
    }
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeRequest for BtreeSinglePutRequest<'a, K, V> {
    fn put_type(&self) -> BtreePutType {
        self.put_type
    }
    
    fn key_size(&self) -> u32 {
        BtreeSinglePutRequest::key_size(self)
    }
    
    fn value_size(&self) -> u32 {
        BtreeSinglePutRequest::value_size(self)
    }
}

/// Range PUT request
///
/// Embeds BtreeRangeRequest for range management and adds PUT-specific fields.
pub struct BtreeRangePutRequest<'a, K: BtreeKey, V: BtreeValue> {
    range_request: BtreeRangeRequest<K>,
    put_type: BtreePutType,
    value: &'a V,
    filter_fn: Option<&'a PutFilterFn<K, V>>,
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeRangePutRequest<'a, K, V> {
    /// Create a new range PUT request
    ///
    /// # Arguments
    /// * `input_range` - The full key range to operate on
    /// * `put_type` - Type of put operation (typically Update for ranges)
    /// * `value` - Value to insert/update for all keys in range
    /// * `batch_size` - Maximum number of entries to process per operation
    /// * `filter_fn` - Optional filter function for conditional updates
    pub fn new(
        input_range: BtreeKeyRange<K>,
        put_type: BtreePutType,
        value: &'a V,
        batch_size: u32,
        filter_fn: Option<&'a PutFilterFn<K, V>>,
    ) -> Self {
        Self {
            range_request: BtreeRangeRequest::new(input_range, batch_size),
            put_type,
            value,
            filter_fn,
        }
    }

    // Delegate to embedded range_request
    pub fn input_range(&self) -> &BtreeKeyRange<K> {
        self.range_request.input_range()
    }

    pub fn working_range(&self) -> &BtreeKeyRange<K> {
        self.range_request.working_range()
    }

    pub fn batch_size(&self) -> u32 {
        self.range_request.batch_size()
    }

    pub fn first_key(&self) -> K {
        self.range_request.first_key()
    }

    pub fn first_key_size(&self) -> u32 {
        self.range_request.first_key_size()
    }

    pub fn trim_working_range(&mut self, end_key: K, end_incl: bool) {
        self.range_request.trim_working_range(end_key, end_incl)
    }

    pub fn shift_working_range(&mut self, start_key: Option<K>) {
        self.range_request.shift_working_range(start_key)
    }

    // Own methods
    pub fn value(&self) -> &V {
        self.value
    }

    pub fn put_type(&self) -> BtreePutType {
        self.put_type
    }

    pub fn filter_fn(&self) -> Option<&PutFilterFn<K, V>> {
        self.filter_fn
    }

    pub fn value_size(&self) -> u32 {
        self.value.serialized_size()
    }
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeRequest for BtreeRangePutRequest<'a, K, V> {
    fn put_type(&self) -> BtreePutType {
        self.put_type
    }
    
    fn key_size(&self) -> u32 {
        self.first_key_size()
    }
    
    fn value_size(&self) -> u32 {
        BtreeRangePutRequest::value_size(self)
    }
}

//================================================================================
// GET Request Types
//================================================================================

/// Single key GET request
pub struct BtreeGetRequest<'a, K: BtreeKey> {
    key: &'a K,
}

impl<'a, K: BtreeKey> BtreeGetRequest<'a, K> {
    pub fn new(key: &'a K) -> Self {
        Self { key }
    }
    
    pub fn key(&self) -> &K {
        self.key
    }
}

/// Get any key in range request (optimization for range queries)
/// Returns the first key-value pair found in the range
pub struct BtreeGetAnyRequest<K: BtreeKey> {
    range: BtreeKeyRange<K>,
}

impl<K: BtreeKey> BtreeGetAnyRequest<K> {
    pub fn new(range: BtreeKeyRange<K>) -> Self {
        Self { range }
    }
    
    pub fn range(&self) -> &BtreeKeyRange<K> {
        &self.range
    }
}

//================================================================================
// QUERY Request Types (Sweep Query)
//================================================================================

/// Query request for sweep queries (follows sibling links)
/// Corresponds to C++ BtreeQueryRequest in btree_query_impl.ipp
pub struct BtreeQueryRequest<'a, K: BtreeKey, V: BtreeValue> {
    base: BtreeRangeRequest<K>,
    filter_fn: Option<&'a GetFilterFn<K, V>>,
    reverse_order: bool,
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeQueryRequest<'a, K, V> {
    /// Create new query request
    ///
    /// # Arguments
    /// * `range` - Key range to query
    /// * `batch_size` - Maximum number of results per batch
    /// * `filter_fn` - Optional filter function to include/exclude entries
    /// * `reverse_order` - If true, iterate in reverse order (for TiKV integration)
    pub fn new(range: BtreeKeyRange<K>, batch_size: u32, filter_fn: Option<&'a GetFilterFn<K, V>>,
            reverse_order: bool) -> Self {
        Self {
            base: BtreeRangeRequest::new(range, batch_size),
            filter_fn,
            reverse_order,
        }
    }

    /// Get working range (current query window)
    pub fn working_range(&self) -> &BtreeKeyRange<K> {
        self.base.working_range()
    }

    /// Get input range (original query range)
    pub fn input_range(&self) -> &BtreeKeyRange<K> {
        self.base.input_range()
    }

    /// Get batch size (max results to return)
    pub fn batch_size(&self) -> u32 {
        self.base.batch_size()
    }

    /// Get filter function
    pub fn filter_fn(&self) -> Option<&GetFilterFn<K, V>> {
        self.filter_fn
    }

    /// Get reverse order flag
    pub fn reverse_order(&self) -> bool {
        self.reverse_order
    }

    /// Get the first key in working range (for finding starting child in interior nodes)
    pub fn first_key(&self) -> K {
        self.base.first_key()
    }

    /// Shift working range forward (or backward if reverse) for next batch
    /// 
    /// Corresponds to C++ BtreeQueryRequest::shift_working_range()
    /// 
    /// # Arguments
    /// * `last_key` - Last key returned in the previous batch
    /// * `incl` - Whether the key is inclusive (false = exclusive, skip the key)
    pub fn shift_working_range(&mut self, last_key: K, incl: bool) {
        if self.reverse_order {
            // For reverse: shift END key backward (last key becomes new end)
            self.base.working_range.end_key = last_key;
            self.base.working_range.end_incl = incl;
            // Keep start unchanged
        } else {
            // For forward: shift START key forward (last key becomes new start)
            self.base.working_range.start_key = last_key;
            self.base.working_range.start_incl = incl;
            // Reset end to input_range.end
            self.base.working_range.end_key = self.base.input_range.end_key.clone();
            self.base.working_range.end_incl = self.base.input_range.end_incl;
        }
    }
}

/// Handle for query results with pagination support
/// Contains results from a query batch and internal state for continuation
pub struct QueryResultHandle<'a, K: BtreeKey, V: BtreeValue> {
    /// Results from this batch
    pub results: Vec<(K, V)>,
    /// Internal query request (used for query_next() continuation)
    request: BtreeQueryRequest<'a, K, V>,
    /// Whether there are more results beyond this batch
    has_more: bool,
}

impl<'a, K: BtreeKey, V: BtreeValue> QueryResultHandle<'a, K, V> {
    /// Create new result handle
    pub(crate) fn new(results: Vec<(K, V)>, request: BtreeQueryRequest<'a, K, V>, has_more: bool) -> Self {
        Self { results, request, has_more }
    }

    /// Check if there are more results available
    /// 
    /// If true, caller can call btree.query_next(handle) to fetch the next batch
    pub fn has_more(&self) -> bool {
        self.has_more
    }

    /// Get the internal request (for query_next() continuation)
    pub(crate) fn request(self) -> BtreeQueryRequest<'a, K, V> {
        self.request
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_put_request_creation() {
        let key = 42u64;
        let value = 100u64;
        let req = BtreeSinglePutRequest::new(&key, &value, BtreePutType::Upsert, None);

        assert_eq!(*req.key(), 42);
        assert_eq!(*req.value(), 100);
        assert_eq!(req.put_type(), BtreePutType::Upsert);
        assert_eq!(req.key_size(), 8); // u64 is 8 bytes
        assert_eq!(req.value_size(), 8);
    }

    #[test]
    fn test_range_request_creation() {
        let range = BtreeKeyRange::new(10u64, true, 100u64, false);
        let value = 999u64;
        let req = BtreeRangePutRequest::new(range, BtreePutType::Update, &value, 1000, None);

        assert_eq!(req.working_range().start_key, 10);
        assert_eq!(req.working_range().end_key, 100);
        assert_eq!(req.batch_size(), 1000);
        assert_eq!(*req.value(), 999);
    }

    #[test]
    fn test_trim_working_range() {
        let range = BtreeKeyRange::new(10u64, true, 100u64, false);
        let value = 999u64;
        let mut req = BtreeRangePutRequest::new(range, BtreePutType::Update, &value, 1000, None);

        // Trim to [10, 50)
        req.trim_working_range(50, false);
        assert_eq!(req.working_range().end_key, 50);
        assert_eq!(req.working_range().end_incl, false);
        
        // Input range unchanged
        assert_eq!(req.input_range().end_key, 100);
    }

    #[test]
    fn test_shift_working_range() {
        let range = BtreeKeyRange::new(10u64, true, 100u64, false);
        let value = 999u64;
        let mut req = BtreeRangePutRequest::new(range, BtreePutType::Update, &value, 1000, None);

        // Trim to [10, 50)
        req.trim_working_range(50, false);
        
        // Shift - should move to [50, 100) (continuing from where we left off)
        req.shift_working_range(None);
        assert_eq!(req.working_range().start_key, 50);
        assert_eq!(req.working_range().start_incl, true); // Flipped from false
        assert_eq!(req.working_range().end_key, 100); // Reset to input end
    }
}

//================================================================================
// REMOVE Request Types
//================================================================================

/// Single key REMOVE request (matches C++ BtreeSingleRemoveRequest)
pub struct BtreeRemoveRequest<'a, K: BtreeKey> {
    key: &'a K,
}

impl<'a, K: BtreeKey> BtreeRemoveRequest<'a, K> {
    pub fn new(key: &'a K) -> Self {
        Self { key }
    }
    
    pub fn key(&self) -> &K {
        self.key
    }
}

/// Remove any key in range request (matches C++ BtreeRemoveAnyRequest)
pub struct BtreeRemoveAnyRequest<K: BtreeKey> {
    range: BtreeKeyRange<K>,
}

impl<K: BtreeKey> BtreeRemoveAnyRequest<K> {
    pub fn new(range: BtreeKeyRange<K>) -> Self {
        Self { range }
    }
    
    pub fn range(&self) -> &BtreeKeyRange<K> {
        &self.range
    }
}

/// Range remove request (matches C++ BtreeRangeRemoveRequest)
pub struct BtreeRangeRemoveRequest<'a, K: BtreeKey, V: BtreeValue> {
    base: BtreeRangeRequest<K>,
    filter_fn: Option<&'a RemoveFilterFn<K, V>>,
    _phantom: std::marker::PhantomData<V>,
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeRangeRemoveRequest<'a, K, V> {
    /// Create new range remove request
    ///
    /// # Arguments
    /// * `range` - Key range to remove
    /// * `batch_size` - Maximum number of entries to remove per operation
    /// * `filter_fn` - Optional filter function to select which entries to remove
    pub fn new(range: BtreeKeyRange<K>, batch_size: u32, filter_fn: Option<&'a RemoveFilterFn<K, V>>) -> Self {
        Self {
            base: BtreeRangeRequest::new(range, batch_size),
            filter_fn,
            _phantom: std::marker::PhantomData,
        }
    }

    pub fn working_range(&self) -> &BtreeKeyRange<K> {
        self.base.working_range()
    }

    pub fn input_range(&self) -> &BtreeKeyRange<K> {
        self.base.input_range()
    }

    pub fn batch_size(&self) -> u32 {
        self.base.batch_size()
    }

    pub fn filter_fn(&self) -> Option<&RemoveFilterFn<K, V>> {
        self.filter_fn
    }

    pub fn first_key(&self) -> K {
        self.base.first_key()
    }

    pub fn trim_working_range(&mut self, end_key: K, end_incl: bool) {
        self.base.trim_working_range(end_key, end_incl)
    }

    pub fn shift_working_range(&mut self, new_start_key: Option<K>) {
        self.base.shift_working_range(new_start_key)
    }
}
