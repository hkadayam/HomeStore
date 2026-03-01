//! Snapshot registry and snapshot handle for key-level MVCC snapshot isolation.
//!
//! # Design
//!
//! A global `GLOBAL_SEQ: AtomicU64` is the monotonic write-sequence counter.
//! Each snapshot captures `snapshot_ts = GLOBAL_SEQ.load(Acquire)` at creation
//! time. Readers using a snapshot never see writes with `commit_ts > snapshot_ts`.
//!
//! `SnapshotRegistry` tracks all live snapshots and exposes
//! `min_active_snapshot_ts()` — the oldest snapshot's ts — which the inline GC
//! filter uses to decide whether old key versions can be discarded.
//!
//! # Thread safety
//!
//! `SnapshotRegistry` is `Send + Sync` and can be shared via `Arc`.
//! The inner map is a lock-free `crossbeam_skiplist::SkipMap` keyed by
//! `(snapshot_ts, unique_id)`. Each `Snapshot` instance gets its own entry,
//! avoiding the refcount race that a `(ts, AtomicUsize)` design would have.

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use crossbeam_skiplist::SkipMap;
use crate::common::error::Result;
use crate::index::iterator::RangeIterator;

// ============================================================================
// Global sequence counter
// ============================================================================

/// Monotonically increasing write-sequence counter (commit timestamp source).
///
/// Writers call `fetch_add(1, SeqCst)` **inside the btree leaf write lock**
/// (via `scan_and_put_one`'s `mutate_key` callback) to obtain an atomic commit ts.
/// Readers call `load(Acquire)` at snapshot creation time.
pub static GLOBAL_SEQ: AtomicU64 = AtomicU64::new(1);

// ============================================================================
// SnapshotRegistry
// ============================================================================

/// Tracks all live snapshots via a lock-free concurrent skip list.
///
/// Key: `(snapshot_ts, unique_id)` — one entry per live `Snapshot` handle.
/// Using a composite key avoids refcount races: every handle has its own entry,
/// and `remove` is called exactly once in `Snapshot::drop`.
///
/// `min_active_snapshot_ts()` is O(1) via `SkipMap::front()`.
pub struct SnapshotRegistry {
    /// `(snapshot_ts, unique_id)` → `()`
    snapshots: SkipMap<(u64, u64), ()>,
    /// Monotonic ID source. Each new snapshot gets a distinct ID even at the same ts.
    next_id: AtomicU64,
}

impl SnapshotRegistry {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            snapshots: SkipMap::new(),
            next_id: AtomicU64::new(0),
        })
    }

    /// Register a new live snapshot at `ts`. Returns its unique `id` for later `release`.
    ///
    /// The caller must pair every `register` with exactly one `release(ts, id)`.
    pub fn register(&self, ts: u64) -> u64 {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        self.snapshots.insert((ts, id), ());
        id
    }

    /// Deregister the snapshot identified by `(ts, id)`.
    pub fn release(&self, ts: u64, id: u64) {
        self.snapshots.remove(&(ts, id));
    }

    /// The oldest snapshot ts currently active, or `u64::MAX` if none.
    ///
    /// Inline GC in `scan_and_put_one` reads this **before** entering the btree
    /// write lock. A version at `version_ts` can be discarded when:
    ///   `version_ts < min_active_snapshot_ts()` AND a newer version exists.
    pub fn min_active_snapshot_ts(&self) -> u64 {
        self.snapshots.front().map(|e| e.key().0).unwrap_or(u64::MAX)
    }

    /// True when no snapshots are currently active.
    pub fn is_empty(&self) -> bool {
        self.snapshots.is_empty()
    }
}

// ============================================================================
// SnapshotOps — trait for snapshot-isolated reads
// ============================================================================

/// Trait implemented by index backends that support snapshot-isolated reads.
///
/// `TableIndex` (in `mem_db`) implements this trait, allowing `Snapshot` to live
/// in `homedb_core` without a dependency on `mem_db`. `Snapshot` holds an
/// `Arc<dyn SnapshotOps>` — typically an `Arc<TableIndex>`.
///
/// Implementations are responsible for key validation before delegating to the
/// underlying btree. `release_snapshot` is always sync (called from `Drop`).
#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
pub trait SnapshotOps: Send + Sync {
    /// Point lookup at the given snapshot timestamp.
    async fn snapshot_get(&self, key: Vec<u8>, ts: u64) -> Result<Option<Vec<u8>>>;

    /// Range scan over `[start, end)` at the given snapshot timestamp.
    /// `reverse = true` returns results in descending key order.
    async fn snapshot_get_range(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        ts: u64,
    ) -> Result<RangeIterator>;

    /// Deregister the snapshot. Called from `Snapshot::drop` — must be sync.
    fn release_snapshot(&self, ts: u64, id: u64);
}

// ============================================================================
// Snapshot — RAII handle
// ============================================================================

/// A point-in-time read view on a single index.
///
/// Created via `TableIndex::get_snapshot(Arc<TableIndex>)`. The handle holds an
/// `Arc<dyn SnapshotOps>` so it stays valid regardless of the index's lifetime.
/// Dropping the handle automatically deregisters the snapshot via `release_snapshot`.
///
/// # Usage
/// ```ignore
/// let snap = TableIndex::get_snapshot(table.primary_index())?;
/// let val  = snap.get(key).await?;
/// let iter = snap.get_range(start, end, 64).await?;
/// ```
pub struct Snapshot {
    ts: u64,
    id: u64,
    ops: Arc<dyn SnapshotOps>,
}

impl Snapshot {
    pub fn new(ts: u64, id: u64, ops: Arc<dyn SnapshotOps>) -> Self {
        Self { ts, id, ops }
    }

    /// The sequence number captured when this snapshot was created.
    pub fn ts(&self) -> u64 { self.ts }

    /// Snapshot-isolated point lookup.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.ops.snapshot_get(key, self.ts).await
    }

    /// Snapshot-isolated forward range scan over `[start_key, end_key)`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        self.ops.snapshot_get_range(start_key, end_key, batch_size, false, self.ts).await
    }

    /// Snapshot-isolated reverse range scan over `[start_key, end_key)`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range_reverse(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        self.ops.snapshot_get_range(start_key, end_key, batch_size, true, self.ts).await
    }
}

impl Drop for Snapshot {
    fn drop(&mut self) {
        self.ops.release_snapshot(self.ts, self.id);
    }
}

impl std::fmt::Debug for Snapshot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Snapshot(ts={})", self.ts)
    }
}
