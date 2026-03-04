//! Snapshot registry and snapshot handle for key-level MVCC snapshot isolation.
//!
//! # Design
//!
//! A global `GLOBAL_SEQ: AtomicU64` is the monotonic write-sequence counter.
//! Each snapshot captures `snapshot_ts = GLOBAL_SEQ.load(Acquire)` at creation
//! time. Readers using a snapshot never see writes with `commit_ts > snapshot_ts`.
//!
//! `SnapshotRegistry` tracks all live snapshots and exposes
//! `min_active_snapshot_ts()` — the oldest snapshot's ts — which both the inline
//! and background GC use to decide whether old key versions can be discarded.
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
use parking_lot::RwLock;
use homestore::index::btree::detail::btree_req::{GetFilter, GetFilterDecision};
use crate::common::db_kv::{DbKey, DbValue};
use crate::common::error::Result;
use crate::index::iterator::RangeIterator;
use super::key::{MvccKey, MvccValue};

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
/// Key: `snapshot_ts` — each snapshot gets a unique ts from `GLOBAL_SEQ.fetch_add`,
/// so no composite key is needed. One entry per live `Snapshot` handle.
/// `remove` is called exactly once in `Snapshot::drop`.
///
/// `min_active_snapshot_ts()` is O(1) via `SkipMap::front()`.
///
/// # Registration / GC ordering
///
/// There is a window inside `register()` between `GLOBAL_SEQ.fetch_add` (which bumps
/// the global counter so new writes get commit_ts > snapshot_ts) and the subsequent
/// `SkipMap::insert` (which makes the snapshot visible to `min_active_snapshot_ts`).
/// If background GC reads `min_active_snapshot_ts` during this window it will not
/// see the in-flight snapshot and may GC a version that the snapshot needs.
///
/// `gc_fence` closes this window:
///   - `register()` holds the **read lock** for the entire fetch_add + insert sequence. Multiple registrations proceed
///     concurrently; GC is the only exclusive user.
///   - `min_active_snapshot_ts()` (GC-safe path) holds the **write lock**, which blocks until every in-flight
///     `register()` has completed its insert, then reads the minimum ts. Only the background GC task and write-path
///     inline GC call this.
///
/// # Empty registry fallback
///
/// When no snapshots are active, `min_active_snapshot_ts()` returns `GLOBAL_SEQ.load()`.
/// This is critical for correctness: any snapshot registered after the call gets
/// `ts >= GLOBAL_SEQ` (from `fetch_add`), so GC only reclaims versions strictly below the current sequence tip — the
/// anchor it keeps is exactly what those future snapshots will read.
pub struct SnapshotRegistry {
    snapshots: SkipMap<u64, ()>, // `snapshot_ts` → `()`
    gc_fence: RwLock<()>,
}

impl SnapshotRegistry {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            snapshots: SkipMap::new(),
            gc_fence: RwLock::new(()),
        })
    }

    /// Bump the global sequence counter and register the new ts as a live snapshot.
    ///
    /// Holds `gc_fence` read lock for the entire fetch_add + insert so that
    /// `gc_min_snapshot_ts` (write lock) always sees a fully-inserted snapshot.
    ///
    /// Returns the unique `ts` for the new snapshot.
    pub fn register(&self) -> u64 {
        let _guard = self.gc_fence.read();
        let ts = GLOBAL_SEQ.fetch_add(1, Ordering::SeqCst);
        self.snapshots.insert(ts, ());
        ts
    }

    /// Deregister the snapshot identified by `ts`.
    pub fn release(&self, ts: u64) { self.snapshots.remove(&ts); }

    /// The oldest snapshot ts currently active, safe for all GC decisions.
    ///
    /// Takes the `gc_fence` write lock, which blocks until every in-flight
    /// `register()` has completed its `SkipMap::insert`. The returned ts is
    /// therefore a true lower bound — no registered snapshot is missed.
    ///
    /// When no snapshots are active, returns `GLOBAL_SEQ.load()` so that GC only
    /// reclaims versions strictly below the current sequence tip — any
    /// snapshot registered after this call has `ts >= GLOBAL_SEQ` and will see the
    /// anchor GC keeps.
    pub fn min_active_snapshot_ts(&self) -> u64 {
        let _guard = self.gc_fence.write();
        self.snapshots.front().map(|e| *e.key()).unwrap_or_else(|| GLOBAL_SEQ.load(Ordering::Acquire))
    }

    /// True when no snapshots are currently active.
    pub fn is_empty(&self) -> bool { self.snapshots.is_empty() }
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
    fn release_snapshot(&self, ts: u64);
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
    ops: Arc<dyn SnapshotOps>,
}

impl Snapshot {
    pub fn new(ts: u64, ops: Arc<dyn SnapshotOps>) -> Self { Self { ts, ops } }

    /// The sequence number captured when this snapshot was created.
    pub fn ts(&self) -> u64 { self.ts }

    /// Snapshot-isolated point lookup.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> { self.ops.snapshot_get(key, self.ts).await }

    /// Snapshot-isolated forward range scan over `[start_key, end_key)`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range(&self, start_key: Vec<u8>, end_key: Vec<u8>, batch_size: u32) -> Result<RangeIterator> {
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
    fn drop(&mut self) { self.ops.release_snapshot(self.ts); }
}

impl std::fmt::Debug for Snapshot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "Snapshot(ts={})", self.ts) }
}

// ============================================================================
// MvccQueryFilter — GetFilter for snapshot-isolated range scans
// ============================================================================

/// `GetFilter` for snapshot-isolated range queries.
///
/// Applied during btree range scans (via `BtreeIndex::query`) to:
/// 1. Skip versions too new for the snapshot (`seq_id >= snapshot_ts`).
/// 2. Deduplicate: after the first visible version of a user key is resolved,
///    skip all older versions of the same key.
/// 3. Skip tombstones (the key was deleted at or before this snapshot).
///
/// Entries arrive newest-first within each user key (because `inv_seq` sorts ascending).
/// `last_user_key` tracks which user key was last resolved so stale versions are dropped
/// immediately in `check_key` without a value read.
pub struct MvccQueryFilter {
    /// Snapshot timestamp: versions with `seq_id >= snapshot_ts` are invisible.
    snapshot_ts: u64,
    /// The user key most recently accepted or tombstoned; older versions of the
    /// same key are skipped in `check_key` without reading the value.
    last_user_key: std::cell::RefCell<Option<DbKey>>,
}

// Safety: accessed only from the single-threaded btree query path; RefCell never
// escapes that context.
unsafe impl Sync for MvccQueryFilter {}

impl MvccQueryFilter {
    pub fn new(snapshot_ts: u64) -> Self {
        Self { snapshot_ts, last_user_key: std::cell::RefCell::new(None) }
    }
}

impl GetFilter<MvccKey<DbKey>, MvccValue<DbValue>> for MvccQueryFilter {
    fn check_key(&self, key: &MvccKey<DbKey>) -> GetFilterDecision {
        // Too new for this snapshot.
        if key.seq_id() >= self.snapshot_ts {
            return GetFilterDecision::Skip;
        }
        // Already resolved this user key (deduplication: skip stale older versions).
        if self.last_user_key.borrow().as_ref() == Some(&key.inner) {
            return GetFilterDecision::Skip;
        }
        // First candidate for this user key — need value to check for tombstone.
        GetFilterDecision::NeedValue
    }

    fn check_kv(&self, key: &MvccKey<DbKey>, val: &MvccValue<DbValue>) -> GetFilterDecision {
        // Mark this user key as resolved (even if tombstone) so older versions are skipped.
        *self.last_user_key.borrow_mut() = Some(key.inner.clone());
        if val.is_tombstone() {
            GetFilterDecision::Skip
        } else {
            GetFilterDecision::Include
        }
    }
}
