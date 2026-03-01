//! Range query iterator over a BtreeIndex (single or partitioned).
//!
//! `RangeIterator` is the public type for both plain and MVCC range scans.
//! Internally it wraps either a `PlainRangeIterator` (raw pass-through) or an
//! `MvccRangeIterator` (snapshot-filtered, per-user-key deduplicated).

use std::sync::Arc;

use homestore::index::btree::detail::btree_req::BtreeKeyRange;

use super::btree_index::{BtreeIndex, IndexQueryHandle};
use crate::common::db_kv::{DbKey, DbValue};
use crate::mvcc::key::{MvccKey, MvccValue};
use crate::common::error::{HomeDbError, Result};
use crate::common::key_value_spec::KeySpec;

// ============================================================================
// PlainRangeIterator — raw pass-through for non-MVCC tables
// ============================================================================

struct PlainRangeIterator {
    index: Arc<dyn BtreeIndex<DbKey, DbValue>>,
    handle: Option<Box<dyn IndexQueryHandle<DbKey, DbValue>>>,
    current_batch: Vec<(DbKey, DbValue)>,
    start_key: Vec<u8>,
    end_key: Vec<u8>,
    batch_size: u32,
    reverse: bool,
    key_spec: KeySpec,
}

unsafe impl Send for PlainRangeIterator {}

impl PlainRangeIterator {
    fn new(
        index: Arc<dyn BtreeIndex<DbKey, DbValue>>,
        handle: Box<dyn IndexQueryHandle<DbKey, DbValue>>,
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
    async fn next(&mut self) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
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
    async fn seek(&mut self, key: &[u8]) -> Result<bool> {
        self.handle = None;
        self.current_batch.clear();

        let (range_start, range_end) = if self.reverse {
            (DbKey::new(self.start_key.clone(), &self.key_spec), DbKey::new(key.to_vec(), &self.key_spec))
        } else {
            (DbKey::new(key.to_vec(), &self.key_spec), DbKey::new(self.end_key.clone(), &self.key_spec))
        };

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
}

// ============================================================================
// MvccRangeIterator — snapshot-filtered, per-user-key deduplicated
// ============================================================================

/// Iterates over MVCC key entries applying snapshot filtering and per-user-key deduplication.
///
/// Raw btree range: `[MvccKey{inner: start_key, inv_seq: 0} .. MvccKey{inner: end_key, inv_seq: 0})`
/// covering all versions of all keys in `[start_key, end_key)`, newest-first within each key.
///
/// For each unique user key, at most one version is emitted:
/// - Skip entries with `seq_id > snapshot_ts` (too new).
/// - Skip entries for a user key already resolved (same inner key as `last_user_key`).
/// - If the first visible version for a user key is a tombstone → key is deleted, skip.
/// - If the first visible version is live → emit `(user_key_bytes, value_bytes)`.
///
/// `snapshot_ts = u64::MAX` behaves as "latest" — used by non-snapshot `get_range` on MVCC tables.
struct MvccRangeIterator {
    index: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>>,
    handle: Option<Box<dyn IndexQueryHandle<MvccKey<DbKey>, MvccValue<DbValue>>>>,
    current_batch: Vec<(MvccKey<DbKey>, MvccValue<DbValue>)>,
    snapshot_ts: u64,
    last_user_key: Option<DbKey>,
    start_key: Vec<u8>,
    end_key: Vec<u8>,
    batch_size: u32,
    reverse: bool,
    key_spec: KeySpec,
}

unsafe impl Send for MvccRangeIterator {}

impl MvccRangeIterator {
    fn new(
        index: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>>,
        handle: Box<dyn IndexQueryHandle<MvccKey<DbKey>, MvccValue<DbValue>>>,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        snapshot_ts: u64,
        key_spec: KeySpec,
    ) -> Self {
        let results = handle.results().to_vec();
        let has_more = handle.has_more();
        Self {
            index,
            handle: if has_more { Some(handle) } else { None },
            current_batch: results,
            snapshot_ts,
            last_user_key: None,
            start_key,
            end_key,
            batch_size,
            reverse,
            key_spec,
        }
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn next(&mut self) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        loop {
            // Drain the current batch with MVCC filtering.
            while !self.current_batch.is_empty() {
                let (mvcc_key, mvcc_val) = self.current_batch.remove(0);

                // Skip entries that are at or newer than the snapshot (exclusive upper bound).
                // A snapshot at ts sees versions with seq_id < ts only.
                if mvcc_key.seq_id() >= self.snapshot_ts {
                    continue;
                }

                // If we already resolved this user key in a previous iteration, skip all
                // remaining versions of it (they are older and would be shadowed).
                if self.last_user_key.as_ref() == Some(&mvcc_key.inner) {
                    continue;
                }

                // First visible version for this user key — record it.
                self.last_user_key = Some(mvcc_key.inner.clone());

                if mvcc_val.is_tombstone() {
                    // Key is deleted at or before snapshot_ts — skip the key.
                    continue;
                }

                // Live version — emit (user_key_bytes, value_bytes).
                let user_key_bytes = mvcc_key.inner.into_vec();
                let value_bytes = mvcc_val.into_inner()
                    .map(|v| v.into_vec())
                    .unwrap_or_default();
                return Ok(Some((user_key_bytes, value_bytes)));
            }

            // Fetch next batch.
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
    async fn seek(&mut self, key: &[u8]) -> Result<bool> {
        self.handle = None;
        self.current_batch.clear();
        self.last_user_key = None;

        // Build the MvccKey seek target: (user_key, inv_seq=0) → newest version of user_key.
        let user_key = DbKey::new(key.to_vec(), &self.key_spec);
        let (range_start, range_end) = if self.reverse {
            (
                MvccKey { inner: DbKey::new(self.start_key.clone(), &self.key_spec), inv_seq: 0 },
                MvccKey { inner: user_key, inv_seq: 0 },
            )
        } else {
            (
                MvccKey { inner: user_key, inv_seq: 0 },
                MvccKey { inner: DbKey::new(self.end_key.clone(), &self.key_spec), inv_seq: 0 },
            )
        };

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
}

// ============================================================================
// RangeIterator — public wrapper over Plain or Mvcc inner iterator
// ============================================================================

enum RangeIteratorInner {
    Plain(PlainRangeIterator),
    Mvcc(MvccRangeIterator),
}

/// Public iterator for range queries over any BtreeIndex (plain or MVCC).
///
/// Created by `TableIndex::get_range`, `get_range_reverse`, `snapshot_get_range`, etc.
/// The plain variant does a raw pass-through; the MVCC variant applies snapshot filtering
/// and deduplicates per user key.
pub struct RangeIterator {
    inner: RangeIteratorInner,
}

unsafe impl Send for RangeIterator {}

impl RangeIterator {
    /// Create from a plain (non-MVCC) btree query handle.
    pub fn new_plain(
        index: Arc<dyn BtreeIndex<DbKey, DbValue>>,
        handle: Box<dyn IndexQueryHandle<DbKey, DbValue>>,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        key_spec: KeySpec,
    ) -> Self {
        Self {
            inner: RangeIteratorInner::Plain(PlainRangeIterator::new(
                index, handle, start_key, end_key, batch_size, reverse, key_spec,
            )),
        }
    }

    /// Create from an MVCC btree query handle.
    ///
    /// `snapshot_ts` is the snapshot timestamp for isolation. Pass `u64::MAX` for
    /// "latest version" semantics (non-snapshot `get_range` on MVCC tables).
    pub fn new_mvcc(
        index: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>>,
        handle: Box<dyn IndexQueryHandle<MvccKey<DbKey>, MvccValue<DbValue>>>,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        snapshot_ts: u64,
        key_spec: KeySpec,
    ) -> Self {
        Self {
            inner: RangeIteratorInner::Mvcc(MvccRangeIterator::new(
                index, handle, start_key, end_key, batch_size, reverse, snapshot_ts, key_spec,
            )),
        }
    }

    /// Compatibility constructor: same signature as the old `RangeIterator::new` for plain tables.
    pub fn new(
        index: Arc<dyn BtreeIndex<DbKey, DbValue>>,
        handle: Box<dyn IndexQueryHandle<DbKey, DbValue>>,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        key_spec: KeySpec,
    ) -> Self {
        Self::new_plain(index, handle, start_key, end_key, batch_size, reverse, key_spec)
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn next(&mut self) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        match &mut self.inner {
            RangeIteratorInner::Plain(it) => it.next().await,
            RangeIteratorInner::Mvcc(it) => it.next().await,
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
        match &mut self.inner {
            RangeIteratorInner::Plain(it) => it.seek(key).await,
            RangeIteratorInner::Mvcc(it) => it.seek(key).await,
        }
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn seek_for_prev(&mut self, key: &[u8]) -> Result<bool> {
        match &mut self.inner {
            RangeIteratorInner::Plain(it) => {
                if !it.reverse {
                    return Err(HomeDbError::InvalidOperation(
                        "seek_for_prev only valid for reverse iterators".to_string(),
                    ));
                }
                it.seek(key).await
            }
            RangeIteratorInner::Mvcc(it) => {
                if !it.reverse {
                    return Err(HomeDbError::InvalidOperation(
                        "seek_for_prev only valid for reverse iterators".to_string(),
                    ));
                }
                it.seek(key).await
            }
        }
    }
}
