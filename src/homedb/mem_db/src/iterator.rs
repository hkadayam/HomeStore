//! Range query iterator for MemDB

use std::sync::Arc;
use homestore::index::btree::{
    btree::Btree,
    detail::btree_req::QueryResultHandle,
};
use crate::{table_index::{DbKey, DbValue}, error::{MemDbError, Result}};

/// Iterator for range queries
/// 
/// RangeIterator owns the B-tree Arc and manages pagination internally.
pub struct RangeIterator {
    btree: Arc<Btree<DbKey, DbValue>>,
    handle: Option<QueryResultHandle<'static, DbKey, DbValue>>,
    current_batch: Vec<(DbKey, DbValue)>,
}

// Safety: RangeIterator only contains Owned DbKey/DbValue (never Borrowed)
// because query results are deserialized with copy=true from btree
unsafe impl Send for RangeIterator {}

impl RangeIterator {
    /// Create a new range iterator from a query result handle
    pub(crate) fn new(
        btree: Arc<Btree<DbKey, DbValue>>,
        handle: QueryResultHandle<'static, DbKey, DbValue>,
    ) -> Self {
        let results = handle.results.clone();
        let has_more = handle.has_more();
        
        Self {
            btree,
            handle: if has_more { Some(handle) } else { None },
            current_batch: results,
        }
    }
    
    /// Get the next key-value pair
    /// 
    /// Returns:
    /// - Ok(Some((key, value))) if there's a next item
    /// - Ok(None) if iteration is complete
    /// - Err if btree operation fails
    pub async fn next(&mut self) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        loop {
            // Try to get from current batch
            if !self.current_batch.is_empty() {
                // Remove first item to move it out (avoids cloning Vec inside)
                let (key, value) = self.current_batch.remove(0);
                return Ok(Some((key.into_vec(), value.into_vec())));
            }
            
            // Current batch exhausted, try to fetch next batch
            match self.handle.take() {
                Some(handle) if handle.has_more() => {
                    let next_handle = self.btree
                        .query_next_batch(handle)
                        .await
                        .map_err(|e| MemDbError::BtreeError(format!("{:?}", e)))?;
                    
                    self.current_batch = next_handle.results.clone();
                    
                    if next_handle.has_more() {
                        self.handle = Some(next_handle);
                    }
                    // Loop continues to try getting from new batch
                }
                _ => {
                    // No more results
                    return Ok(None);
                }
            }
        }
    }
    
    /// Collect all remaining items into a vector
    pub async fn collect(mut self) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let mut results = Vec::new();
        while let Some(item) = self.next().await? {
            results.push(item);
        }
        Ok(results)
    }
}
