//! Range query iterator for MemDB

use std::sync::Arc;
use homestore::index::btree::{
    btree::Btree,
    detail::btree_req::{QueryResultHandle, BtreeKeyRange},
};
use crate::{table_index::{DbKey, DbValue}, error::{MemDbError, Result}, KeySpec};

/// Iterator for range queries
///
/// RangeIterator owns the B-tree Arc and manages pagination internally.
pub struct RangeIterator {
    btree: Arc<Btree<DbKey, DbValue>>,
    handle: Option<QueryResultHandle<'static, DbKey, DbValue>>,
    current_batch: Vec<(DbKey, DbValue)>,
    start_key: Vec<u8>,
    end_key: Vec<u8>,
    batch_size: u32,
    reverse: bool,
    key_spec: KeySpec,
}

// Safety: RangeIterator only contains Owned DbKey/DbValue (never Borrowed)
// because query results are deserialized with copy=true from btree
unsafe impl Send for RangeIterator {}

impl RangeIterator {
    /// Create a new range iterator from a query result handle
    pub(crate) fn new(
        btree: Arc<Btree<DbKey, DbValue>>,
        handle: QueryResultHandle<'static, DbKey, DbValue>,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        key_spec: KeySpec,
    ) -> Self {
        let results = handle.results.clone();
        let has_more = handle.has_more();

        Self {
            btree,
            handle: if has_more { Some(handle) } else { None },
            current_batch: results,
            start_key,
            end_key,
            batch_size,
            reverse,
            key_spec,
        }
    }
    
    /// Get the next key-value pair
    /// 
    /// Returns:
    /// - Ok(Some((key, value))) if there's a next item
    /// - Ok(None) if iteration is complete
    /// - Err if btree operation fails
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
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
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn collect(mut self) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let mut results = Vec::new();
        while let Some(item) = self.next().await? {
            results.push(item);
        }
        Ok(results)
    }

    /// Seek to the first key >= target key
    ///
    /// Returns true if found, false if target is beyond end of range.
    /// After a successful seek, calling next() will return the key seeked to or the next available key.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn seek(&mut self, key: &[u8]) -> Result<bool> {
        // Discard current state
        self.handle = None;
        self.current_batch.clear();

        // For forward iteration: [key, end)
        // For reverse iteration: [start, key+1) and iterate backwards
        let (range_start, range_end) = if self.reverse {
            // Reverse: seek to key means iterate backwards from key towards start
            (
                DbKey::new(self.start_key.clone(), &self.key_spec),
                DbKey::new(key.to_vec(), &self.key_spec),
            )
        } else {
            // Forward: seek to key means iterate forward from key towards end
            (
                DbKey::new(key.to_vec(), &self.key_spec),
                DbKey::new(self.end_key.clone(), &self.key_spec),
            )
        };

        let range = BtreeKeyRange::new(
            range_start,
            true,  // inclusive start
            range_end,
            self.reverse,  // For reverse, make end inclusive
        );

        // Query from new position
        let new_handle = if self.reverse {
            self.btree
                .query_traversal(range, self.batch_size, None, true)
                .await
        } else {
            self.btree
                .query(range, self.batch_size, None)
                .await
        }
        .map_err(|e| MemDbError::BtreeError(format!("{:?}", e)))?;

        if new_handle.results.is_empty() {
            return Ok(false);  // Key not found or past end
        }

        self.current_batch = new_handle.results.clone();
        if new_handle.has_more() {
            self.handle = Some(new_handle);
        }

        Ok(true)
    }

    /// Seek to the last key <= target key (for reverse iteration)
    ///
    /// Returns true if found, false if target is before start of range.
    /// This method is only valid for reverse iterators.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn seek_for_prev(&mut self, key: &[u8]) -> Result<bool> {
        if !self.reverse {
            return Err(MemDbError::InvalidOperation(
                "seek_for_prev only valid for reverse iterators".to_string()
            ));
        }

        self.seek(key).await
    }
}
