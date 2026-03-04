//! MVCC `PutFilter` implementations for the write path.
//!
//! Two filters are provided:
//!
//! - `MvccInlineGcFilter` — full inline GC. Scans existing versions newest-first,
//!   removes versions below the oldest active snapshot inside the btree write lock.
//!   Requires `min_active_snapshot_ts()` on every write (holds `gc_fence` write lock).
//!
//! - `MvccDeferredGcFilter` — deferred-only GC. Scans at most one existing version
//!   (`max_scan = 1`), pushes a `GcEvent::Add` for it, and always returns `Keep`.
//!   Never calls `min_active_snapshot_ts()`; the background GC handles all cleanup.
//!   Use this when eliminating `gc_fence` contention on the hot read/write path matters.

use std::sync::Arc;
use homestore::index::btree::detail::btree_req::{PutFilter, PutFilterDecision};
use crate::common::db_kv::{DbKey, DbValue};
use super::gc::{GcEvent, GcQueue};
use super::key::{MvccKey, MvccValue};

// ============================================================================
// MvccInlineGcFilter — inline GC during scan_and_put_one
// ============================================================================

/// `PutFilter` that performs inline GC while inserting a new MVCC version.
///
/// Scans existing versions of the same user key (newest-first in btree order):
/// - `seq_id >= min_snap`: kept (visible to some active snapshot).
/// - First version below `min_snap` (the **anchor**):
///   pushes `GcEvent::Add` and returns `Keep`.  `anchor_seq` is set to `seq_id`
///   so that re-encountering the same anchor on btree retry is idempotent.
/// - All `seq_id < anchor_seq`: push `GcEvent::Removed` and return `Remove`.
///
/// `check_kv` is never called — no value reads are needed.
pub struct MvccInlineGcFilter {
    /// Minimum active snapshot timestamp (from `SnapshotRegistry::min_active_snapshot_ts`).
    pub min_snap: u64,
    /// seq_id of the GC anchor (first version below min_snap), once discovered.
    /// Using the actual seq_id rather than a boolean prevents stale state from
    /// causing the anchor to be removed on btree retry.
    anchor_seq: std::cell::Cell<Option<u64>>,
    /// Lock-free queue for GC events. `None` if GC is not wired up.
    gc_queue: Option<Arc<GcQueue>>,
    /// The `commit_ts` stamped by `mutate_key`; read by `MvccOps` after the call returns
    /// to emit the tombstone `GcEvent::Add` for remove operations.
    pub last_commit_ts: std::cell::Cell<u64>,
}

// Safety: accessed only under the btree leaf write lock; Cell<_> fields never
// escape that single-threaded context.
unsafe impl Sync for MvccInlineGcFilter {}

impl MvccInlineGcFilter {
    pub fn new(min_snap: u64, gc_queue: Option<Arc<GcQueue>>) -> Self {
        Self {
            min_snap,
            anchor_seq: std::cell::Cell::new(None),
            gc_queue,
            last_commit_ts: std::cell::Cell::new(0),
        }
    }
}

impl PutFilter<MvccKey<DbKey>, MvccValue<DbValue>> for MvccInlineGcFilter {
    fn check_key(&self, key: &MvccKey<DbKey>) -> PutFilterDecision {
        let seq = key.seq_id();
        if seq >= self.min_snap {
            // Still visible to some active snapshot — must keep.
            PutFilterDecision::Keep
        } else {
            match self.anchor_seq.get() {
                // First version below min_snap: the GC anchor.
                // Push Add unconditionally; deferred GC will read the value to decide fate.
                None => {
                    self.anchor_seq.set(Some(seq));
                    if let Some(ref q) = self.gc_queue {
                        q.push(GcEvent::Add { key_bytes: key.inner.as_bytes().to_vec(), seq_id: seq });
                    }
                    PutFilterDecision::Keep
                }
                // Strictly below the anchor — shadowed; remove inline.
                Some(x) if seq < x => {
                    if let Some(ref q) = self.gc_queue {
                        q.push(GcEvent::Removed { key_bytes: key.inner.as_bytes().to_vec(), seq_id: seq });
                    }
                    PutFilterDecision::Remove
                }
                // seq == anchor_seq: re-encountered on btree retry — push Add again
                // (GcQueue BTreeSet deduplicates) and keep.
                Some(_) => {
                    if let Some(ref q) = self.gc_queue {
                        q.push(GcEvent::Add { key_bytes: key.inner.as_bytes().to_vec(), seq_id: seq });
                    }
                    PutFilterDecision::Keep
                }
            }
        }
    }

    fn check_kv(&self, _key: &MvccKey<DbKey>, _val: &MvccValue<DbValue>) -> PutFilterDecision {
        unreachable!("MvccInlineGcFilter never returns NeedOldValue from check_key")
    }

    fn mutate_key(&self, key: &mut MvccKey<DbKey>) {
        let commit_ts = super::snapshot::GLOBAL_SEQ
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        key.inv_seq = !commit_ts;
        self.last_commit_ts.set(commit_ts);
    }
}

// ============================================================================
// MvccDeferredGcFilter — deferred-only GC (no inline removes)
// ============================================================================

/// `PutFilter` that defers all GC to the background task.
///
/// Used when `MvccOps::inline_gc = false`. Paired with `max_scan = 1` so only
/// the most recent existing version of the key is visited.
///
/// For that one entry: pushes `GcEvent::Add` from `check_key` (no value read needed)
/// and returns `Keep`. The background GC will reclaim old versions once no snapshot
/// pins them.
///
/// `min_active_snapshot_ts()` is **never called** — no `gc_fence` contention on the
/// write path.
pub struct MvccDeferredGcFilter {
    /// Lock-free queue for GC events.
    gc_queue: Option<Arc<GcQueue>>,
    /// The `commit_ts` stamped by `mutate_key`; read by `MvccOps` after the call returns.
    pub last_commit_ts: std::cell::Cell<u64>,
}

unsafe impl Sync for MvccDeferredGcFilter {}

impl MvccDeferredGcFilter {
    pub fn new(gc_queue: Option<Arc<GcQueue>>) -> Self {
        Self {
            gc_queue,
            last_commit_ts: std::cell::Cell::new(0),
        }
    }
}

impl PutFilter<MvccKey<DbKey>, MvccValue<DbValue>> for MvccDeferredGcFilter {
    fn check_key(&self, key: &MvccKey<DbKey>) -> PutFilterDecision {
        // Blindly enqueue as a GC candidate — background GC will decide what to keep.
        if let Some(ref q) = self.gc_queue {
            q.push(GcEvent::Add {
                key_bytes: key.inner.as_bytes().to_vec(),
                seq_id: key.seq_id(),
            });
        }
        PutFilterDecision::Keep
    }

    fn check_kv(&self, _key: &MvccKey<DbKey>, _val: &MvccValue<DbValue>) -> PutFilterDecision {
        unreachable!("MvccDeferredGcFilter never returns NeedOldValue from check_key")
    }

    fn mutate_key(&self, key: &mut MvccKey<DbKey>) {
        let commit_ts = super::snapshot::GLOBAL_SEQ
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        key.inv_seq = !commit_ts;
        self.last_commit_ts.set(commit_ts);
    }
}
