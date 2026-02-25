//! Range query iterator over a BtreeIndex (single or partitioned).

use std::sync::Arc;

use homestore::index::btree::detail::btree_req::BtreeKeyRange;

use crate::btree_index::{BtreeIndex, IndexQueryHandle};
use crate::db_kv::{DbKey, DbValue};
use crate::{HomeDbError, KeySpec, Result};

/// Iterator for range queries over any BtreeIndex (single or partitioned).
pub struct RangeIterator {
    index: Arc<dyn BtreeIndex>,
    handle: Option<Box<dyn IndexQueryHandle>>,
    current_batch: Vec<(DbKey, DbValue)>,
    start_key: Vec<u8>,
    end_key: Vec<u8>,
    batch_size: u32,
    reverse: bool,
    key_spec: KeySpec,
}

unsafe impl Send for RangeIterator {}

impl RangeIterator {
    /// Create from a BtreeIndex and the initial query handle.
    pub fn new(
        index: Arc<dyn BtreeIndex>,
        handle: Box<dyn IndexQueryHandle>,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        key_spec: KeySpec,
    ) -> Self {
        let results = handle.results().to_vec();
        let has_more = handle.has_more();
        Self {
            index,
            handle: if has_more { Some(handle) } else { None },
            current_batch: results,
            start_key,
            end_key,
            batch_size,
            reverse,
            key_spec,
        }
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn next(&mut self) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        loop {
            if !self.current_batch.is_empty() {
                let (key, value) = self.current_batch.remove(0);
                return Ok(Some((key.into_vec(), value.into_vec())));
            }

            match self.handle.take() {
                Some(h) if h.has_more() => {
                    let next_handle = self
                        .index
                        .query_next_batch(h)
                        .await
                        .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
                    self.current_batch = next_handle.results().to_vec();
                    self.handle = if next_handle.has_more() { Some(next_handle) } else { None };
                }
                _ => return Ok(None),
            }
        }
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn collect(mut self) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let mut results = Vec::new();
        while let Some(item) = self.next().await? {
            results.push(item);
        }
        Ok(results)
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn seek(&mut self, key: &[u8]) -> Result<bool> {
        self.handle = None;
        self.current_batch.clear();

        let (range_start, range_end) = if self.reverse {
            (DbKey::new(self.start_key.clone(), &self.key_spec), DbKey::new(key.to_vec(), &self.key_spec))
        } else {
            (DbKey::new(key.to_vec(), &self.key_spec), DbKey::new(self.end_key.clone(), &self.key_spec))
        };

        // Forward seek: end is exclusive (stop before end_key boundary).
        // Reverse seek (seek_for_prev): end is inclusive so the target key itself is first result.
        let range = BtreeKeyRange::new(range_start, true, range_end, self.reverse);

        let new_handle = self
            .index
            .query(range, self.batch_size, None, self.reverse)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;

        if new_handle.results().is_empty() {
            return Ok(false);
        }

        self.current_batch = new_handle.results().to_vec();
        self.handle = if new_handle.has_more() { Some(new_handle) } else { None };
        Ok(true)
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn seek_for_prev(&mut self, key: &[u8]) -> Result<bool> {
        if !self.reverse {
            return Err(HomeDbError::InvalidOperation("seek_for_prev only valid for reverse iterators".to_string()));
        }
        self.seek(key).await
    }
}
