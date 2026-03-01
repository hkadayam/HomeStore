//! TableIndex — schema-validated wrapper around a `BtreeIndex` backend.
//!
//! Backend selection (runtime `partition_key_size`):
//!   `partition_key_size == 0`  → `UnshardedBtree` (single btree, no sharding)
//!   `partition_key_size >= 1`  → `ShardedBtree`   (hash-sharded by partition key prefix)
//!
//! When `TableSpec::mvcc_supported = true`, the btree stores `MvccKey<DbKey>` /
//! `MvccValue<DbValue>` instead of plain `DbKey` / `DbValue`. All existing methods
//! (put, get, remove, get_range, etc.) return the latest live version. Snapshot-
//! isolated reads are obtained by calling `TableIndex::get_snapshot(Arc<Self>)`,
//! then using the returned `Snapshot`'s read methods.
//!
//! # Architecture
//!
//! `TableIndex` holds an `Arc<dyn IndexOps>` — a trait defined in `homedb_core`
//! that abstracts over `NonTxnOps` (non-MVCC) and `MvccOps` (MVCC). All btree
//! interactions, GC, and snapshot logic live inside the respective `IndexOps`
//! implementation. `TableIndex` only validates inputs and delegates — no match arms,
//! no MVCC branching.

use std::sync::Arc;
use homestore::index::btree::BtreeConfig;
use homedb_core::{
    IndexOps, MvccGc, MvccOps, NonTxnOps,
    RangeIterator, Snapshot, SnapshotOps, GLOBAL_SEQ,
};
use crate::{HomeDbError, KeySpec, KeyType, PrefixType, Result, TableSpec, ValueSpec};

// ============================================================================
// Public API types
// ============================================================================

/// Index type classification.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndexType {
    /// Primary index — stores the full row data.
    Primary,
    /// Secondary index — stores a pointer to the primary key.
    Secondary,
}

// ============================================================================
// TableIndex
// ============================================================================

/// A physical B-tree index within a table.
///
/// Holds an `Arc<dyn IndexOps>` (either `NonTxnOps` or `MvccOps`) and provides
/// schema-validated operations. All branching on plain-vs-MVCC lives inside the
/// `IndexOps` implementation — `TableIndex` methods are branchless delegates.
///
/// For snapshot-isolated reads, use `TableIndex::get_snapshot(Arc<Self>)` which
/// returns a self-contained `Snapshot` with its own read methods.
#[derive(Clone)]
pub struct TableIndex {
    name: String,
    index_type: IndexType,
    spec: TableSpec,
    ops: Arc<dyn IndexOps>,
    /// Maximum user-facing key size for `put` validation.
    /// Plain: `btree_config.max_key_size()`.
    /// MVCC: `btree_config.max_key_size() - 8` (MvccKey appends 8-byte inv_seq).
    max_key_size: u32,
}

impl TableIndex {
    /// Create a new index with the given specification (single constructor, no split).
    ///
    /// Selects MVCC or plain backend, chooses `UnshardedBtree` or `ShardedBtree`
    /// based on `partition_key_size`, and validates key/value sizes against btree capacity.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(name: String, index_type: IndexType, spec: TableSpec) -> Result<Self> {
        // MVCC always requires VarObjNode (3); plain chooses based on key/value types.
        let node_variant =
            if spec.mvcc_supported { 3 } else { Self::determine_node_variant(&spec) };

        let mut config = BtreeConfig::new(spec.node_size, name.clone());
        config.leaf_node_variant = node_variant;
        config.int_node_variant = node_variant;

        // Prefix compression is only meaningful for plain tables.
        if !spec.mvcc_supported {
            if let PrefixType::Prefixable(Some(prefix_size)) = spec.key_spec.prefix_type {
                config.expected_prefix_size = prefix_size as u16;
            }
        }

        // MvccValue serialized size = 1 (tombstone flag) + user_value_size.
        let raw_value_size = spec.value_spec.max_size() as u32;
        config.suggest_inline_value_size(
            if spec.mvcc_supported { 1 + raw_value_size } else { raw_value_size },
        );

        let node_size = config.node_size;
        let inline_value_size = config.inline_value_size;
        let btree_max_key = config.max_key_size();
        // MVCC key = user_key + 8-byte inv_seq; subtract 8 to get user-facing capacity.
        let max_key_size =
            if spec.mvcc_supported { btree_max_key.saturating_sub(8) } else { btree_max_key };

        let spec_max_key = spec.key_spec.max_size() as u32;
        if spec_max_key > max_key_size {
            return if spec.mvcc_supported {
                Err(HomeDbError::Config(format!(
                    "Key size {} exceeds MVCC btree capacity {} \
                     (node_size={}, inline_value_size={}, 8 bytes reserved for seq_id)",
                    spec_max_key, max_key_size, node_size, inline_value_size
                )))
            } else {
                Err(HomeDbError::Config(format!(
                    "Key size {} exceeds btree capacity {} (node_size={}, inline_value_size={})",
                    spec_max_key, max_key_size, node_size, inline_value_size
                )))
            };
        }

        let ops: Arc<dyn IndexOps> = if spec.mvcc_supported {
            Arc::new(
                MvccOps::new(
                    config,
                    spec.partition_key_size,
                    spec.key_spec.clone(),
                    spec.value_spec.clone(),
                )
                .await?,
            )
        } else {
            Arc::new(
                NonTxnOps::new(
                    config,
                    spec.partition_key_size,
                    spec.key_spec.clone(),
                    spec.value_spec.clone(),
                )
                .await?,
            )
        };

        Ok(Self { name, index_type, spec, ops, max_key_size })
    }

    /// Determine btree node variant for plain (non-MVCC) tables.
    fn determine_node_variant(spec: &TableSpec) -> u8 {
        // Prefixable keys always use PrefixCompressNode regardless of the underlying KeyType,
        // because prefix compression produces variable-length storage.
        if matches!(spec.key_spec.prefix_type, PrefixType::Prefixable(_)) {
            return 4; // PrefixCompressNode
        }
        // DbKey/DbValue report FIXED_SERIALIZED_SIZE = None (runtime-determined).
        // SimpleNode requires compile-time constant sizes; use VarObjNode for runtime fixed.
        match (&spec.key_spec.key_type, &spec.value_spec) {
            (KeyType::Fixed(_), ValueSpec::Fixed(_)) => 3,       // VarObjNode
            (KeyType::Variable(_), ValueSpec::Fixed(_)) => 1,    // VarKeyNode
            (KeyType::Fixed(_), ValueSpec::Variable(_)) => 2,    // VarValueNode
            (KeyType::Variable(_), ValueSpec::Variable(_)) => 3, // VarObjNode
        }
    }

    pub fn name(&self) -> &str { &self.name }
    pub fn index_type(&self) -> IndexType { self.index_type }
    pub fn spec(&self) -> &TableSpec { &self.spec }

    // =========================================================================
    // Snapshot creation (static method — needs Arc<Self> to give to Snapshot)
    // =========================================================================

    /// Create a `Snapshot` capturing the current write sequence number.
    ///
    /// The snapshot holds a strong `Arc<TableIndex>` (via `Arc<dyn SnapshotOps>`) and
    /// exposes `get` / `get_range` / `get_range_reverse` for snapshot-isolated reads.
    /// Dropping the snapshot automatically unregisters it from the registry.
    ///
    /// Returns `Err(InvalidOperation)` if called on a non-MVCC table.
    ///
    /// # Example
    /// ```ignore
    /// let snap = TableIndex::get_snapshot(table.primary_index())?;
    /// let val  = snap.get(key).await?;
    /// ```
    pub fn get_snapshot(this: Arc<Self>) -> Result<Snapshot> {
        let ts = GLOBAL_SEQ.load(std::sync::atomic::Ordering::Acquire);
        let id = this.ops.register_snapshot(ts)?; // Err for NonTxnOps
        Ok(Snapshot::new(ts, id, this))
    }

    /// Return the deferred GC controller for MVCC tables, or `None` for plain tables.
    ///
    /// Primarily used in tests to drive `run_cycle` directly without waiting for
    /// the background task.
    pub fn mvcc_gc(&self) -> Option<Arc<MvccGc>> {
        self.ops.mvcc_gc()
    }

    // =========================================================================
    // Write operations
    // =========================================================================

    /// Put a single key-value pair.
    ///
    /// On MVCC tables: inserts a new versioned entry via `scan_and_put_one`. The
    /// commit timestamp is stamped by `MvccGcFilter::mutate_key` inside the btree
    /// leaf write lock. Old same-key versions below the oldest active snapshot are
    /// removed inline (GC) in the same `write_node()` call.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        if key.len() as u32 > self.max_key_size {
            return Err(HomeDbError::KeyTooLarge {
                size: key.len(),
                max: self.max_key_size as usize,
            });
        }
        self.spec.validate(&key, &value)?;
        self.ops.put(key, value).await
    }

    // =========================================================================
    // Read operations
    // =========================================================================

    /// Get the latest (or only) value for `key`. Returns `None` if absent.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        self.ops.get(key).await
    }

    // =========================================================================
    // Remove operations
    // =========================================================================

    /// Remove a single key. Returns the old value if present.
    ///
    /// On MVCC tables: inserts a tombstone and always returns `None`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        self.ops.remove(key).await
    }

    // =========================================================================
    // Range operations
    // =========================================================================

    /// Query `[start_key, end_key)`, returning a batch iterator (forward order).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        self.ops.get_range(start_key, end_key, batch_size, false).await
    }

    /// Query `[start_key, end_key)`, returning a batch iterator (reverse order).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range_reverse(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        self.ops.get_range(start_key, end_key, batch_size, true).await
    }

    /// Return any one key-value pair in `[start_key, end_key)`, or `None`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_any(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        self.ops.get_any(start_key, end_key).await
    }

    /// Remove any one key in `[start_key, end_key)`. Returns the removed pair if found.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove_any(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        self.ops.remove_any(start_key, end_key).await
    }
}

// ============================================================================
// SnapshotOps impl — lets Arc<TableIndex> coerce to Arc<dyn SnapshotOps>
// ============================================================================

/// `TableIndex` implements `SnapshotOps` so that `get_snapshot` can hand
/// `Arc<TableIndex>` to `Snapshot::new` as `Arc<dyn SnapshotOps>`.
/// All methods validate keys then delegate to the inner `IndexOps`.
#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl SnapshotOps for TableIndex {
    async fn snapshot_get(&self, key: Vec<u8>, ts: u64) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        self.ops.snapshot_get(key, ts).await
    }

    async fn snapshot_get_range(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        ts: u64,
    ) -> Result<RangeIterator> {
        self.spec.key_spec.validate_key(&start)?;
        self.spec.key_spec.validate_key(&end)?;
        self.ops.snapshot_get_range(start, end, batch_size, reverse, ts).await
    }

    fn release_snapshot(&self, ts: u64, id: u64) {
        self.ops.release_snapshot(ts, id);
    }
}
