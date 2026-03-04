//! `NonTxnOps` — non-transactional (no-MVCC) `IndexOps` implementation backed by a `BtreeIndex`.
//!
//! Keys and values are stored as-is (`DbKey` / `DbValue`) with no versioning.
//! All snapshot methods return `Err(InvalidOperation)`.

use std::sync::Arc;
use homestore::index::btree::{underlying::mem::MemBtree, BtreeConfig};
use homestore::index::btree::detail::btree_req::BtreeKeyRange;
use crate::index::btree_index::BtreeIndex;
use crate::common::db_kv::{DbKey, DbValue};
use crate::common::error::{HomeDbError, Result};
use crate::index::index_ops::{IndexOps, err_not_mvcc};
use crate::index::iterator::RangeIterator;
use crate::common::key_value_spec::{KeySpec, ValueSpec};
use crate::index::unsharded_btree::UnshardedBtree;
use crate::index::sharded_btree::{ShardedBtree, DEFAULT_MAX_PARTITIONS};

pub struct NonTxnOps {
    pub(crate) btree: Arc<dyn BtreeIndex<DbKey, DbValue>>,
    pub(crate) key_spec: KeySpec,
    pub(crate) value_spec: ValueSpec,
}

impl NonTxnOps {
    /// Create a new `NonTxnOps` with an `UnshardedBtree` or `ShardedBtree` backend.
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
        let btree: Arc<dyn BtreeIndex<DbKey, DbValue>> = if partition_key_size == 0 {
            Arc::new(
                UnshardedBtree::new(config, |c| Box::new(MemBtree::new(&c)))
                    .await
                    .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?,
            )
        } else {
            Arc::new(
                ShardedBtree::new(config, partition_key_size, DEFAULT_MAX_PARTITIONS, |c| Box::new(MemBtree::new(&c)))
                    .await
                    .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?,
            )
        };
        Ok(Self { btree, key_spec, value_spec })
    }
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl IndexOps for NonTxnOps {
    async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        let db_key = DbKey::new(key, &self.key_spec);
        let db_value = DbValue::new(value, &self.value_spec);
        self.btree
            .put(&db_key, &db_value)
            .await
            .map(|_| ())
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))
    }

    async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        let db_key = DbKey::new(key, &self.key_spec);
        self.btree
            .get(&db_key)
            .await
            .map(|opt| opt.map(|v| v.into_vec()))
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))
    }

    async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        let db_key = DbKey::new(key, &self.key_spec);
        self.btree
            .remove(&db_key)
            .await
            .map(|opt| opt.map(|v| v.into_vec()))
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))
    }

    async fn get_range(&self, start: Vec<u8>, end: Vec<u8>, batch_size: u32, reverse: bool) -> Result<RangeIterator> {
        let start_copy = start.clone();
        let end_copy = end.clone();
        let db_start = DbKey::new(start, &self.key_spec);
        let db_end = DbKey::new(end, &self.key_spec);
        let handle = self
            .btree
            .query(BtreeKeyRange::new(db_start, true, db_end, false), batch_size, None, reverse)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        Ok(RangeIterator::new(
            Arc::clone(&self.btree),
            handle,
            start_copy,
            end_copy,
            batch_size,
            reverse,
            self.key_spec.clone(),
        ))
    }

    async fn remove_any(&self, start: Vec<u8>, end: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        let db_start = DbKey::new(start, &self.key_spec);
        let db_end = DbKey::new(end, &self.key_spec);
        let handle = self
            .btree
            .query(BtreeKeyRange::new(db_start, true, db_end, false), 1, None, false)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        let maybe_key = handle.results().first().map(|(k, _)| k.clone());
        drop(handle);

        if let Some(key) = maybe_key {
            let removed = self.btree.remove(&key).await.map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
            Ok(removed.map(|v| (key.into_vec(), v.into_vec())))
        } else {
            Ok(None)
        }
    }

    fn register_snapshot(&self) -> Result<u64> { Err(err_not_mvcc("get_snapshot")) }

    fn release_snapshot(&self, _ts: u64) {
        // Non-transactional tables have no snapshot registry — no-op.
    }

    async fn snapshot_get(&self, _key: Vec<u8>, _ts: u64) -> Result<Option<Vec<u8>>> {
        Err(err_not_mvcc("snapshot_get"))
    }

    async fn snapshot_get_range(
        &self,
        _start: Vec<u8>,
        _end: Vec<u8>,
        _batch_size: u32,
        _reverse: bool,
        _ts: u64,
    ) -> Result<RangeIterator> {
        Err(err_not_mvcc("snapshot_get_range"))
    }
}
