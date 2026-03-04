//! Deferred MVCC garbage collection.
//!
//! # Design
//!
//! The inline GC in `MvccGcFilter` (during `scan_and_put_one`) only removes old versions
//! within the current leaf node. When old versions had to be maintained for snapshot reads
//! at the time of mutation or if cleanup spills into other nodes, `MvccGcFilter` emits
//! `GcEvent`s into the lock-free `GcQueue` for deferred cleanup.
//!
//! `MvccGc::spawn_background` starts a single long-running background task that wakes
//! every 100 ms, calls `run_cycle`, and goes back to sleep.  `run_cycle` can also be
//! called directly in tests without any background task running.
//!
//! # GC cycle algorithm
//!
//! `run_cycle(pending)`:
//!   1. Atomically swap the `GcQueue` to drain all pending events.
//!   2. Merge events into `pending: HashMap<key_bytes, BTreeSet<seq_id>>`:
//!        `Add`     → insert seq_id into the set
//!        `Removed` → cancel the seq_id from the set (it was already removed inline)
//!   3. Collect key_bytes of entries whose max seq_id < `min_active_snapshot_ts()`.
//!      (Key-bytes only — no value clone.  Necessary because Rust's borrow checker
//!       does not allow mutating `pending` while an iterator borrows it.)
//!   4. For each eligible key: issue one `remove_range` with `MvccGcRemoveFilter`.
//!      The filter traverses all versions newest-first in a **single** btree pass:
//!        - Skips versions with seq_id ≥ min_snap (visible to some active snapshot).
//!        - On the first version < min_snap (the GC anchor), reads its value:
//!            Tombstone anchor → removes it + everything below; `anchor_kept` = None.
//!            Live anchor      → keeps it; sets `anchor_kept` = Some(seq_id).
//!        - Removes all subsequent versions below the anchor without value reads.
//!      Then updates `pending` in-place:
//!        `anchor_kept = None`      → key fully gone; remove entry from pending.
//!        `anchor_kept = Some(seq)` → retain seq_ids ≥ seq in the entry's set.
//!
//! # Lock-freedom on the write path
//!
//! `GcQueue::push` is fully lock-free (`ArcSwap::load` + `SegQueue::push`).
//! It is safe to call from inside the btree leaf write lock. Only the GC background
//! task calls `GcQueue::swap`, which performs one atomic pointer exchange.
//!
//! # No mutex on `pending`
//!
//! The `pending` map is owned by — and only ever touched by — the single background
//! GC task.  Callers (including tests) that drive `run_cycle` directly own the map
//! and pass it by `&mut`.  No synchronization primitive is needed.

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use arc_swap::ArcSwap;
use crossbeam_queue::SegQueue;

use homestore::index::btree::detail::btree_req::{RemoveFilter, RemoveFilterDecision};

use crate::common::db_kv::{DbKey, DbValue};
use crate::common::key_value_spec::KeySpec;
use crate::index::btree_index::BtreeIndex;
use crate::mvcc::key::{MvccKey, MvccValue};
use crate::mvcc::snapshot::SnapshotRegistry;

// ============================================================================
// GcEvent
// ============================================================================

/// A deferred GC work item emitted by `MvccGcFilter` during writes.
pub enum GcEvent {
    /// A live-value anchor version (or a newly-written tombstone) needs eventual
    /// deferred GC.  Emitted from:
    ///   - `MvccGcFilter::check_kv` when the existing anchor is a live value, OR
    ///   - `MvccOps::remove` / `remove_any` after writing a tombstone.
    Add { key_bytes: Vec<u8>, seq_id: u64 },

    /// A version was removed inline — cancels any prior `Add` for the same
    /// `(key, seq_id)`.  Emitted from `MvccGcFilter::check_key` in the `Remove` branch.
    Removed { key_bytes: Vec<u8>, seq_id: u64 },
}

// ============================================================================
// GcQueue
// ============================================================================

/// Lock-free event queue for GC events.
///
/// `push` is called inside the btree write lock (hot path) and must be lock-free.
/// `swap` is called only by the GC background task.
pub struct GcQueue {
    current: ArcSwap<SegQueue<GcEvent>>,
}

impl GcQueue {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            current: ArcSwap::new(Arc::new(SegQueue::new())),
        })
    }

    /// Push a GC event — fully lock-free; safe inside the btree write lock.
    pub fn push(&self, event: GcEvent) {
        self.current.load().push(event);
    }

    /// Atomically swap out the current queue and return it for processing.
    /// Only the GC background task should call this.
    pub fn swap(&self) -> Arc<SegQueue<GcEvent>> {
        self.current.swap(Arc::new(SegQueue::new()))
    }
}

// ============================================================================
// MvccGcRemoveFilter
// ============================================================================

/// Per-key `RemoveFilter` used inside `run_cycle` to discover and remove stale
/// MVCC versions in a **single** `remove_range` btree pass.
///
/// Versions are visited newest-first (ascending `MvccKey` order):
///
/// - `seq_id ≥ min_snap` → `Skip`           (still visible to some active snapshot)
/// - First `seq_id < min_snap` → `NeedValue` (the GC anchor; value decides fate)
///   - Tombstone → `Remove`;  `live_anchor_seq` stays `None`
///   - Live value → `Skip`;   `live_anchor_seq` set to `Some(seq_id)`
/// - All `seq_id < anchor_seq` → `Remove`    (below anchor; always stale)
///
/// `anchor_seq` (always set in `check_kv`) acts as the boundary: on btree retry
/// (triggered by a node merge), re-visiting the anchor entry returns `NeedValue`
/// again (seq == anchor_seq) rather than `Remove`, so the anchor is never deleted
/// by stale boolean state.
///
/// After `remove_range`, call `live_anchor_seq()`:
/// - `None`      → tombstone removed; key fully gone (`all_cleaned_up()` = true).
/// - `Some(seq)` → live anchor at `seq` was kept; everything below it was removed.
struct MvccGcRemoveFilter {
    min_snap: u64,
    /// seq_id of the GC anchor (first version below min_snap).  Set in `check_kv`.
    /// Used in `check_key` so that re-visiting the anchor on btree retry returns
    /// `NeedValue` (re-evaluate) instead of `Remove`.
    anchor_seq: std::cell::Cell<Option<u64>>,
    /// seq_id of the live anchor, set only when `check_kv` finds a non-tombstone.
    /// `None` means either no anchor yet or the anchor was a tombstone (fully cleaned up).
    live_anchor_seq: std::cell::Cell<Option<u64>>,
}

unsafe impl Sync for MvccGcRemoveFilter {}

impl MvccGcRemoveFilter {
    fn new(min_snap: u64) -> Self {
        Self {
            min_snap,
            anchor_seq: std::cell::Cell::new(None),
            live_anchor_seq: std::cell::Cell::new(None),
        }
    }

    /// Returns `Some(seq)` if a live anchor at `seq` was kept; `None` if the key
    /// was fully cleaned up (tombstone anchor, all versions removed).
    pub fn live_anchor_seq(&self) -> Option<u64> { self.live_anchor_seq.get() }
}

impl RemoveFilter<MvccKey<DbKey>, MvccValue<DbValue>> for MvccGcRemoveFilter {
    fn check_key(&self, key: &MvccKey<DbKey>) -> RemoveFilterDecision {
        let seq = key.seq_id();
        if seq >= self.min_snap {
            RemoveFilterDecision::Skip
        } else {
            match self.anchor_seq.get() {
                // No anchor yet — this is the first candidate; need value to decide.
                None => RemoveFilterDecision::NeedValue,
                // Strictly below the anchor — always stale.
                Some(x) if seq < x => RemoveFilterDecision::Remove,
                // seq == anchor_seq: re-encountered on btree retry; re-evaluate via check_kv.
                Some(_) => RemoveFilterDecision::NeedValue,
            }
        }
    }

    fn check_kv(&self, key: &MvccKey<DbKey>, val: &MvccValue<DbValue>) -> RemoveFilterDecision {
        let seq = key.seq_id();
        // Record the anchor boundary regardless of tombstone/live so that check_key
        // can use it as a watermark on btree retry.
        self.anchor_seq.set(Some(seq));
        if val.is_tombstone() {
            RemoveFilterDecision::Remove
        } else {
            self.live_anchor_seq.set(Some(seq));
            RemoveFilterDecision::Skip
        }
    }
}

// ============================================================================
// MvccGc
// ============================================================================

/// Background MVCC garbage collector.
///
/// Holds the lock-free event queue plus the btree / registry references needed
/// to issue `remove_range` calls.  The `pending` work map lives inside the
/// background task (or the caller in tests) — `MvccGc` itself is stateless
/// beyond what it shares with writers.
pub struct MvccGc {
    /// Lock-free event queue populated by writers.
    pub queue: Arc<GcQueue>,

    btree: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>>,
    registry: Arc<SnapshotRegistry>,
    key_spec: KeySpec,

    /// Signals the background task to stop (set by `MvccOps` on drop).
    pub shutdown: Arc<AtomicBool>,

    /// When set, the background task skips `run_cycle` (used by tests to freeze GC state).
    pub paused: Arc<AtomicBool>,
}

impl MvccGc {
    pub fn new(
        btree: Arc<dyn BtreeIndex<MvccKey<DbKey>, MvccValue<DbValue>>>,
        registry: Arc<SnapshotRegistry>,
        key_spec: KeySpec,
    ) -> Arc<Self> {
        Arc::new(Self {
            queue: GcQueue::new(),
            btree,
            registry,
            key_spec,
            shutdown: Arc::new(AtomicBool::new(false)),
            paused: Arc::new(AtomicBool::new(false)),
        })
    }

    /// Signal the background task to stop.
    pub fn shutdown(&self) {
        self.shutdown.store(true, Ordering::Relaxed);
    }

    /// Freeze GC: the background task will skip `run_cycle` until unpaused.
    /// Used in tests to prevent GC from running during diagnostic queries.
    pub fn pause(&self) {
        self.paused.store(true, Ordering::SeqCst);
    }

    /// Spawn a single long-running background task that wakes every 100 ms and
    /// calls `run_cycle`.  The `pending` map lives entirely inside this task.
    ///
    /// In `async_backend` mode the task is dispatched to the IOManager reactor pool
    /// using `iomgr::spawn`.  In `sync_backend` mode a plain OS thread is used.
    pub fn spawn_background(self: Arc<Self>) {
        cfg_if::cfg_if! {
            if #[cfg(feature = "async_backend")] {
                iomgr::spawn(async move {
                    let mut pending: HashMap<Vec<u8>, BTreeSet<u64>> = HashMap::new();
                    while !self.shutdown.load(Ordering::Relaxed) {
                        iomgr::sleep(Duration::from_millis(100)).await;
                        self.run_cycle(&mut pending).await;
                    }
                });
            } else {
                std::thread::spawn(move || {
                    let mut pending: HashMap<Vec<u8>, BTreeSet<u64>> = HashMap::new();
                    while !self.shutdown.load(Ordering::Relaxed) {
                        std::thread::sleep(Duration::from_millis(100));
                        if !self.paused.load(Ordering::Relaxed) {
                            self.run_cycle(&mut pending);
                        }
                    }
                });
            }
        }
    }

    /// Run one GC cycle.
    ///
    /// `pending` is owned by the caller (background task or test) and carries
    /// deferred work across cycles — entries accumulate until all their versions
    /// are eligible for removal.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn run_cycle(&self, pending: &mut HashMap<Vec<u8>, BTreeSet<u64>>) {
        // ── Phase 1: drain event queue and merge into pending ──────────────────
        let batch = self.queue.swap();

        while let Some(event) = batch.pop() {
            match event {
                GcEvent::Add { key_bytes, seq_id } => {
                    pending.entry(key_bytes).or_default().insert(seq_id);
                }
                GcEvent::Removed { key_bytes, seq_id } => {
                    if let Some(set) = pending.get_mut(&key_bytes) {
                        set.remove(&seq_id);
                        if set.is_empty() {
                            pending.remove(&key_bytes);
                        }
                    }
                }
            }
        }

        let min_snap = self.registry.min_active_snapshot_ts();

        // Collect key_bytes of entries whose max seq_id < min_snap.
        // Key-bytes only (no BTreeSet copy) — cheap.  The collect is required
        // because Rust does not allow mutating `pending` while iterating over it.
        let work_keys: Vec<Vec<u8>> = pending
            .iter()
            .filter_map(|(k, seq_ids)| {
                let max_seq = *seq_ids.iter().next_back()?;
                if max_seq < min_snap { Some(k.clone()) } else { None }
            })
            .collect();

        // ── Phase 2: single remove_range per key, pending updated inline ───────
        for key_bytes in work_keys {
            let filter = Arc::new(MvccGcRemoveFilter::new(min_snap));
            let range = MvccKey::all_versions_for(key_bytes.clone(), &self.key_spec);
            let filter_dyn: Arc<dyn RemoveFilter<MvccKey<DbKey>, MvccValue<DbValue>>> = filter.clone();
            let _ = self.btree.remove_range(range, Some(filter_dyn)).await;

            match filter.live_anchor_seq() {
                None => {
                    // Tombstone (and all history) removed — key fully gone.
                    pending.remove(&key_bytes);
                }
                Some(anchor_seq) => {
                    // Live anchor kept; discard tracked seq_ids below it.
                    if let Some(set) = pending.get_mut(&key_bytes) {
                        set.retain(|&s| s >= anchor_seq);
                        if set.is_empty() { pending.remove(&key_bytes); }
                    }
                }
            }
        }
    }
}
