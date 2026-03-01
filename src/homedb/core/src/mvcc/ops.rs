//! `MvccOps` — MVCC `IndexOps` implementation backed by a `BtreeIndex`.
//!
//! Keys are stored as `MvccKey<DbKey>` (user key + inverted seq_id) so that
//! newer versions sort first within the same user key. Values are `MvccValue<DbValue>`
//! (user value + tombstone flag).
//!
//! # Write path
//! `put` and `remove` use `scan_and_put_one` so that the commit timestamp is
//! stamped by `MvccGcFilter::mutate_key` **inside the btree leaf write lock**,
//! and inline GC removes old versions below the oldest active snapshot in the
//! same `write_node()` call.
//!
//! For versions that spill across leaf boundaries, `MvccGcFilter` emits
//! `GcEvent`s into the `MvccGc` queue for deferred cleanup by the background task.
//!
//! # Read path
//! `get` queries all versions of a key (newest-first) and returns the first
//! non-tombstone. Snapshot reads seek to `(user_key, !snapshot_ts)` so that
//! only versions with `seq_id ≤ snapshot_ts` are considered.

use std::sync::Arc;
use homestore::index::btree::{underlying::mem::MemBtree, BtreeConfig};
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, PutFilter};
use crate::index::btree_index::BtreeIndex;
use crate::common::db_kv::{DbKey, DbValue};
use crate::common::error::{HomeDbError, Result};
use crate::index::index_ops::IndexOps;
use crate::index::iterator::RangeIterator;
use crate::common::key_value_spec::{KeySpec, ValueSpec};
use super::gc::{GcEvent, MvccGc};
use super::key::{MvccGcFilter, MvccKey, MvccValue};
use super::snapshot::SnapshotRegistry;
use crate::index::unsharded_btree::UnshardedBtree;
use crate::index::sharded_btree::ShardedBtree;

pub struct MvccOps {
    pub(crate) btree: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>>,
    pub(crate) registry: Arc<SnapshotRegistry>,
    pub(crate) key_spec: KeySpec,
    pub(crate) value_spec: ValueSpec,
    pub(crate) gc: Arc<MvccGc>,
}

impl MvccOps {
    /// Create a new `MvccOps` with an `UnshardedBtree` or `ShardedBtree` backend.
    ///
    /// `partition_key_size == 0` → `UnshardedBtree` (single btree).
    /// `partition_key_size >= 1` → `ShardedBtree` (hash-sharded by partition prefix).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(
        config: BtreeConfig,
        partition_key_size: usize,
        key_spec: KeySpec,
        value_spec: ValueSpec,
    ) -> Result<Self> {
        let btree: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>> =
            if partition_key_size == 0 {
                Arc::new(
                    UnshardedBtree::<MvccKey<DbKey>, MvccValue<DbValue>>::new(
                        config,
                        |c| Box::new(MemBtree::new(&c)),
                    )
                    .await
                    .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?,
                )
            } else {
                Arc::new(
                    ShardedBtree::<MvccKey<DbKey>, MvccValue<DbValue>>::new(
                        config,
                        partition_key_size,
                        |c| Box::new(MemBtree::new(&c)),
                    )
                    .await
                    .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?,
                )
            };
        let registry = SnapshotRegistry::new();
        let gc = MvccGc::new(Arc::clone(&btree), Arc::clone(&registry), key_spec.clone());
        Arc::clone(&gc).spawn_background();
        Ok(Self { btree, registry, key_spec, value_spec, gc })
    }

    /// Build an `MvccRangeIterator`-backed `RangeIterator` for `[start_key, end_key)`.
    ///
    /// Raw btree range: `[MvccKey{inner: start, inv_seq: 0} .. MvccKey{inner: end, inv_seq: 0})`
    /// covers ALL versions of ALL user keys in `[start_key, end_key)` (newest-first per key).
    ///
    /// `snapshot_ts = u64::MAX` means "latest version" semantics (no snapshot filter).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn range_iter(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        snapshot_ts: u64,
    ) -> Result<RangeIterator> {
        let mvcc_start = MvccKey::placeholder(start_key.clone(), &self.key_spec);
        let mvcc_end = MvccKey::placeholder(end_key.clone(), &self.key_spec);
        let handle = self
            .btree
            .query(
                BtreeKeyRange::new(mvcc_start, true, mvcc_end, false),
                batch_size,
                None,
                reverse,
            )
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        Ok(RangeIterator::new_mvcc(
            Arc::clone(&self.btree),
            handle,
            start_key,
            end_key,
            batch_size,
            reverse,
            snapshot_ts,
            self.key_spec.clone(),
        ))
    }
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl IndexOps for MvccOps {
    async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        let insert_key = MvccKey::placeholder(key.clone(), &self.key_spec);
        let mvcc_val = MvccValue::live(value, &self.value_spec);
        let scan_range = MvccKey::all_versions_for(key, &self.key_spec);
        let filter = Arc::new(MvccGcFilter::new(
            self.registry.min_active_snapshot_ts(),
            Some(Arc::clone(&self.gc.queue)),
        ));
        self.btree
            .scan_and_put_one(insert_key, mvcc_val, scan_range, Some(filter))
            .await
            .map(|_| ())
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))
    }

    async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        // all_versions_for covers all seq_ids newest-first; batch_size=1 returns the newest.
        let scan_range = MvccKey::all_versions_for(key, &self.key_spec);
        let handle = self
            .btree
            .query(scan_range, 1, None, false)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        match handle.results().first() {
            None => Ok(None),
            Some((_, mvcc_val)) => {
                if mvcc_val.is_tombstone() {
                    Ok(None)
                } else {
                    Ok(mvcc_val.inner().map(|v| v.clone().into_vec()))
                }
            }
        }
    }

    async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        let insert_key = MvccKey::placeholder(key.clone(), &self.key_spec);
        let tombstone = MvccValue::<DbValue>::tombstone();
        let scan_range = MvccKey::all_versions_for(key.clone(), &self.key_spec);
        let filter = Arc::new(MvccGcFilter::new(
            self.registry.min_active_snapshot_ts(),
            Some(Arc::clone(&self.gc.queue)),
        ));
        let filter_dyn: Arc<dyn PutFilter<MvccKey<DbKey>, MvccValue<DbValue>>> = filter.clone();
        self.btree
            .scan_and_put_one(insert_key, tombstone, scan_range, Some(filter_dyn))
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        // Tombstone needs deferred GC — emit Add so the background task can eventually
        // remove it (and everything below it) once no snapshot pins it.
        let commit_ts = filter.last_commit_ts.get();
        self.gc.queue.push(GcEvent::Add {
            key_bytes: DbKey::new(key, &self.key_spec).into_vec(),
            seq_id: commit_ts,
        });
        Ok(None) // MVCC remove always returns None; caller must get() first if old value needed.
    }

    async fn remove_any(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        let found = {
            let mut iter = self.range_iter(start, end, 16, false, u64::MAX).await?;
            iter.next().await?
        };

        if let Some((key_bytes, value_bytes)) = found {
            let insert_key = MvccKey::placeholder(key_bytes.clone(), &self.key_spec);
            let tombstone = MvccValue::<DbValue>::tombstone();
            let scan_range = MvccKey::all_versions_for(key_bytes.clone(), &self.key_spec);
            let filter = Arc::new(MvccGcFilter::new(
                self.registry.min_active_snapshot_ts(),
                Some(Arc::clone(&self.gc.queue)),
            ));
            let filter_dyn: Arc<dyn PutFilter<MvccKey<DbKey>, MvccValue<DbValue>>> = filter.clone();
            self.btree
                .scan_and_put_one(insert_key, tombstone, scan_range, Some(filter_dyn))
                .await
                .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
            // Emit Add for the tombstone (same logic as remove).
            let commit_ts = filter.last_commit_ts.get();
            self.gc.queue.push(GcEvent::Add {
                key_bytes: DbKey::new(key_bytes.clone(), &self.key_spec).into_vec(),
                seq_id: commit_ts,
            });
            Ok(Some((key_bytes, value_bytes)))
        } else {
            Ok(None)
        }
    }

    async fn get_range(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
        batch_size: u32,
        reverse: bool,
    ) -> Result<RangeIterator> {
        self.range_iter(start, end, batch_size, reverse, u64::MAX).await
    }

    async fn get_any(&self, start: Vec<u8>, end: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        let mut iter = self.range_iter(start, end, 16, false, u64::MAX).await?;
        iter.next().await
    }

    fn register_snapshot(&self, ts: u64) -> Result<u64> {
        Ok(self.registry.register(ts))
    }

    fn release_snapshot(&self, ts: u64, id: u64) {
        self.registry.release(ts, id);
    }

    fn mvcc_gc(&self) -> Option<Arc<MvccGc>> {
        Some(Arc::clone(&self.gc))
    }

    async fn snapshot_get(&self, key: Vec<u8>, ts: u64) -> Result<Option<Vec<u8>>> {
        // A snapshot at ts sees versions with seq_id < ts (exclusive).
        // Seeking to inv_seq = !(ts-1) finds the newest version with seq_id ≤ ts-1.
        // ts=0 means nothing was committed before snapshot creation — return None immediately.
        if ts == 0 { return Ok(None); }
        let seek_start = MvccKey::seek(key.clone(), &self.key_spec, ts - 1);
        let range_end = MvccKey::oldest_bound(key, &self.key_spec);
        let range = BtreeKeyRange::new(seek_start, true, range_end, true);
        let handle = self
            .btree
            .query(range, 1, None, false)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        match handle.results().first() {
            None => Ok(None),
            Some((_, mvcc_val)) => {
                if mvcc_val.is_tombstone() {
                    Ok(None)
                } else {
                    Ok(mvcc_val.inner().map(|v| v.clone().into_vec()))
                }
            }
        }
    }

    async fn snapshot_get_range(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        ts: u64,
    ) -> Result<RangeIterator> {
        self.range_iter(start, end, batch_size, reverse, ts).await
    }
}

impl Drop for MvccOps {
    fn drop(&mut self) {
        self.gc.shutdown();
    }
}
