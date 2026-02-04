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
    working_range: std::cell::RefCell<BtreeKeyRange<K>>,
    batch_size: u32,
}

impl<K: BtreeKey> BtreeRangeRequest<K> {
    pub fn new(input_range: BtreeKeyRange<K>, batch_size: u32) -> Self {
        let working_range = input_range.clone();
        Self {
            input_range,
            working_range: std::cell::RefCell::new(working_range),
            batch_size,
        }
    }

    /// Get the original input range (never changes)
    pub fn input_range(&self) -> &BtreeKeyRange<K> {
        &self.input_range
    }

    /// Get the current working range (may be trimmed/shifted during traversal)
    pub fn working_range(&self) -> std::cell::Ref<BtreeKeyRange<K>> {
        self.working_range.borrow()
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
        self.working_range.borrow().start_key.clone()
    }

    /// Get the serialized size of the first key
    pub fn first_key_size(&self) -> u32 {
        self.working_range.borrow().start_key.serialized_size()
    }

    /// Trim working range to child's boundary
    ///
    /// Used when descending to a child node - limits the working range to not exceed
    /// the child's key boundary.
    pub fn trim_working_range(&self, end_key: K, end_incl: bool) {
        let mut range = self.working_range.borrow_mut();
        range.end_key = end_key;
        range.end_incl = end_incl;
    }

    /// Shift working range forward
    ///
    /// # Arguments
    /// * `start_key` - If Some(key), set working start to this key.
    ///                 If None, set working start to current working end (continue from where we left off)
    ///
    /// In both cases, working end is reset to input_range.end
    pub fn shift_working_range(&self, new_start_key: Option<K>) {
        let mut w_range = self.working_range.borrow_mut();
        
        if let Some(key) = new_start_key {
            // Explicit key provided - use it as new start
            w_range.start_key = key;
            w_range.start_incl = true;
        } else {
            // No key - continue from current working end
            w_range.start_key = w_range.end_key.clone();
            w_range.start_incl = !w_range.end_incl; // Flip inclusivity
        }
        
        // Reset end to input range end
        w_range.end_key = self.input_range.end_key.clone();
        w_range.end_incl = self.input_range.end_incl;
    }
}

/// Single key-value PUT request (matches C++ BtreeSinglePutRequest)
pub struct BtreeSinglePutRequest<'a, K: BtreeKey, V: BtreeValue> {
    key: &'a K,
    value: &'a V,
    put_type: BtreePutType,
    // TODO: Add filter_cb: Option<FilterCallback> to replace existing_val functionality
    // The filter_cb can be used to conditionally update values and access existing values
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeSinglePutRequest<'a, K, V> {
    /// Create a new single PUT request (matches C++ constructor)
    ///
    /// # Arguments
    /// * `key` - Key to insert/update
    /// * `value` - Value to insert/update
    /// * `put_type` - Type of put operation (Insert/Update/Upsert)
    pub fn new(
        key: &'a K,
        value: &'a V,
        put_type: BtreePutType,
    ) -> Self {
        Self {
            key,
            value,
            put_type,
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
    // TODO: filter_cb: Option<FilterCallback>,
}

impl<'a, K: BtreeKey, V: BtreeValue> BtreeRangePutRequest<'a, K, V> {
    /// Create a new range PUT request
    ///
    /// # Arguments
    /// * `input_range` - The full key range to operate on
    /// * `put_type` - Type of put operation (typically Update for ranges)
    /// * `value` - Value to insert/update for all keys in range
    /// * `batch_size` - Maximum number of entries to process per operation
    pub fn new(
        input_range: BtreeKeyRange<K>,
        put_type: BtreePutType,
        value: &'a V,
        batch_size: u32,
    ) -> Self {
        Self {
            range_request: BtreeRangeRequest::new(input_range, batch_size),
            put_type,
            value,
        }
    }

    // Delegate to embedded range_request
    pub fn input_range(&self) -> &BtreeKeyRange<K> {
        self.range_request.input_range()
    }

    pub fn working_range(&self) -> std::cell::Ref<BtreeKeyRange<K>> {
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

    pub fn trim_working_range(&self, end_key: K, end_incl: bool) {
        self.range_request.trim_working_range(end_key, end_incl)
    }

    pub fn shift_working_range(&self, start_key: Option<K>) {
        self.range_request.shift_working_range(start_key)
    }

    // Own methods
    pub fn value(&self) -> &V {
        self.value
    }

    pub fn put_type(&self) -> BtreePutType {
        self.put_type
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
pub struct BtreeQueryRequest<K: BtreeKey> {
    base: BtreeRangeRequest<K>,
}

impl<K: BtreeKey> BtreeQueryRequest<K> {
    /// Create new query request
    pub fn new(range: BtreeKeyRange<K>, batch_size: u32) -> Self {
        Self {
            base: BtreeRangeRequest::new(range, batch_size),
        }
    }

    /// Get working range (current query window)
    pub fn working_range(&self) -> std::cell::Ref<BtreeKeyRange<K>> {
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

    /// Get the first key in working range (for finding starting child in interior nodes)
    pub fn first_key(&self) -> K {
        self.base.first_key()
    }

    /// Shift working range forward for next batch
    /// 
    /// Corresponds to C++ BtreeQueryRequest::shift_working_range()
    /// 
    /// # Arguments
    /// * `new_start_key` - Start key for next batch
    /// * `start_incl` - Whether new start is inclusive (false = exclusive, skip the key)
    pub fn shift_working_range(&self, new_start_key: K, start_incl: bool) {
        let mut w_range = self.base.working_range.borrow_mut();
        w_range.start_key = new_start_key;
        w_range.start_incl = start_incl;
        // Reset end to input_range.end
        w_range.end_key = self.base.input_range.end_key.clone();
        w_range.end_incl = self.base.input_range.end_incl;
    }
}

/// Handle for query results with pagination support
/// Contains results from a query batch and internal state for continuation
pub struct QueryResultHandle<K: BtreeKey, V: BtreeValue> {
    /// Results from this batch
    pub results: Vec<(K, V)>,
    /// Internal query request (used for query_next() continuation)
    request: BtreeQueryRequest<K>,
    /// Whether there are more results beyond this batch
    has_more: bool,
}

impl<K: BtreeKey, V: BtreeValue> QueryResultHandle<K, V> {
    /// Create new result handle
    pub(crate) fn new(results: Vec<(K, V)>, request: BtreeQueryRequest<K>, has_more: bool) -> Self {
        Self { results, request, has_more }
    }

    /// Check if there are more results available
    /// 
    /// If true, caller can call btree.query_next(handle) to fetch the next batch
    pub fn has_more(&self) -> bool {
        self.has_more
    }

    /// Get the internal request (for query_next() continuation)
    pub(crate) fn request(self) -> BtreeQueryRequest<K> {
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
        let req = BtreeSinglePutRequest::new(&key, &value, BtreePutType::Upsert);

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
        let req = BtreeRangePutRequest::new(range, BtreePutType::Update, &value, 1000);

        assert_eq!(req.working_range().start_key, 10);
        assert_eq!(req.working_range().end_key, 100);
        assert_eq!(req.batch_size(), 1000);
        assert_eq!(*req.value(), 999);
    }

    #[test]
    fn test_trim_working_range() {
        let range = BtreeKeyRange::new(10u64, true, 100u64, false);
        let value = 999u64;
        let req = BtreeRangePutRequest::new(range, BtreePutType::Update, &value, 1000);

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
        let req = BtreeRangePutRequest::new(range, BtreePutType::Update, &value, 1000);

        // Trim to [10, 50)
        req.trim_working_range(50, false);
        
        // Shift - should move to [50, 100) (continuing from where we left off)
        req.shift_working_range(None);
        assert_eq!(req.working_range().start_key, 50);
        assert_eq!(req.working_range().start_incl, true); // Flipped from false
        assert_eq!(req.working_range().end_key, 100); // Reset to input end
    }
}
