//! MVCC snapshot isolation tests for MemDB.
//!
//! Covers:
//!   1. Basic MVCC put / get / remove round-trip (non-snapshot path)
//!   2. Snapshot reads — snapshot sees exactly the version visible at creation time
//!   3. Tombstone isolation — snapshot before delete keeps old value; after delete sees None
//!   4. Range scan at snapshot — forward and reverse
//!   5. get_snapshot on a non-MVCC table → InvalidOperation error
//!   6. Snapshot deregisters on Drop — SnapshotRegistry is empty afterwards
//!   7. Multiple concurrent snapshots with interleaved writes
//!   8. Inline GC — many writes to same key with no active snapshots; no data corruption
//!
//! Run with:
//!   cargo test --package mem_db --test test_mvcc --no-default-features --features sync_code
//!   cargo test --package mem_db --test test_mvcc --no-default-features --features async_code

use mem_db::{
    MemoryDB, KeySpec, TableSpec, ValueSpec, TableIndex,
    MvccGc, SnapshotRegistry, GLOBAL_SEQ,
};
use std::sync::Arc;
use std::sync::atomic::Ordering;

// ─────────────────────────── Spec / key/value helpers ─────────────────────

const KEY_SIZE: usize = 8;
const VALUE_SIZE: usize = 16;

fn mvcc_spec() -> TableSpec {
    TableSpec::new(KeySpec::fixed(KEY_SIZE), ValueSpec::fixed(VALUE_SIZE))
        .partition_key_size(2)
        .mvcc()
}

fn plain_spec() -> TableSpec {
    TableSpec::fixed_kv(KEY_SIZE, VALUE_SIZE)
}

fn key(id: u64) -> Vec<u8> {
    id.to_be_bytes().to_vec()
}

fn value(id: u64, version: u8) -> Vec<u8> {
    let mut v = id.to_be_bytes().to_vec();
    v.resize(VALUE_SIZE, version);
    v
}

fn min_key() -> Vec<u8> { vec![0u8; KEY_SIZE] }
fn max_key() -> Vec<u8> { vec![0xffu8; KEY_SIZE] }

// ─────────────────────────── Test implementations ─────────────────────────

mod test_impls {
    use super::*;

    // ── 1. Basic MVCC CRUD (non-snapshot path) ─────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_mvcc_basic_crud() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Insert key 1 → value(1, 1)
        index.put(key(1), value(1, 1)).await.unwrap();
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 1)));

        // Overwrite → value(1, 2) — non-snapshot get returns the latest version
        index.put(key(1), value(1, 2)).await.unwrap();
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 2)));

        // Remove → tombstone; non-snapshot get returns None
        index.remove(key(1)).await.unwrap();
        assert_eq!(index.get(key(1)).await.unwrap(), None);

        // Key 2 was never inserted → None
        assert_eq!(index.get(key(2)).await.unwrap(), None);
    }

    // ── 2. Snapshot sees version at creation time ─────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_point_in_time() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Write v1 before any snapshot
        index.put(key(1), value(1, 1)).await.unwrap();

        // Snapshot A captures v1
        let snap_a = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();
        assert_eq!(snap_a.get(key(1)).await.unwrap(), Some(value(1, 1)));

        // Write v2 after snap_a
        index.put(key(1), value(1, 2)).await.unwrap();

        // snap_a still sees v1; non-snapshot get sees v2
        assert_eq!(snap_a.get(key(1)).await.unwrap(), Some(value(1, 1)));
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 2)));

        // Snapshot B captures v2
        let snap_b = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();
        assert_eq!(snap_b.get(key(1)).await.unwrap(), Some(value(1, 2)));
        assert_eq!(snap_a.get(key(1)).await.unwrap(), Some(value(1, 1)));

        // Write v3 after snap_b
        index.put(key(1), value(1, 3)).await.unwrap();

        // snap_a still v1, snap_b still v2, current is v3
        assert_eq!(snap_a.get(key(1)).await.unwrap(), Some(value(1, 1)));
        assert_eq!(snap_b.get(key(1)).await.unwrap(), Some(value(1, 2)));
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 3)));
    }

    // ── 3. Snapshot does not see a key that didn't exist yet ──────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_absent_key() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Snapshot before any writes
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Writes happen after the snapshot
        index.put(key(1), value(1, 1)).await.unwrap();
        index.put(key(2), value(2, 1)).await.unwrap();

        // Snapshot sees neither key
        assert_eq!(snap.get(key(1)).await.unwrap(), None);
        assert_eq!(snap.get(key(2)).await.unwrap(), None);

        // Non-snapshot get sees both
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 1)));
        assert_eq!(index.get(key(2)).await.unwrap(), Some(value(2, 1)));
    }

    // ── 4. Tombstone isolation ────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_tombstone_isolation() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        index.put(key(1), value(1, 1)).await.unwrap();

        // Snapshot before the delete — should still see the value
        let snap_before = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        index.remove(key(1)).await.unwrap();

        // Snapshot after the delete — should see None (tombstone)
        let snap_after = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        assert_eq!(snap_before.get(key(1)).await.unwrap(), Some(value(1, 1)),
            "snap_before should see the pre-delete value");
        assert_eq!(snap_after.get(key(1)).await.unwrap(), None,
            "snap_after should see the tombstone (key deleted)");
        assert_eq!(index.get(key(1)).await.unwrap(), None,
            "non-snapshot get should return None after delete");
    }

    // ── 5. Snapshot range scan (forward) ─────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_range_scan() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Insert keys 1..=5 v1
        for id in 1u64..=5 {
            index.put(key(id), value(id, 1)).await.unwrap();
        }

        // Snapshot captures keys 1–5 v1
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Overwrite keys 3 and 5 with v2; add key 6
        index.put(key(3), value(3, 2)).await.unwrap();
        index.put(key(5), value(5, 2)).await.unwrap();
        index.put(key(6), value(6, 1)).await.unwrap();
        // Delete key 2
        index.remove(key(2)).await.unwrap();

        // Snapshot range scan [key(1), key(7)) should see the original 5 keys at v1
        let mut iter = snap.get_range(key(1), key(7), 64).await.unwrap();
        let mut results = Vec::new();
        while let Some(kv) = iter.next().await.unwrap() {
            results.push(kv);
        }

        assert_eq!(results.len(), 5, "snapshot range should return exactly 5 entries");
        for (i, id) in (1u64..=5).enumerate() {
            assert_eq!(results[i].0, key(id), "key mismatch at position {}", i);
            assert_eq!(results[i].1, value(id, 1), "snapshot should see v1 for key {}", id);
        }

        // Current (non-snapshot) range scan should see: keys 1, 3(v2), 4, 5(v2), 6; not key 2
        let mut cur_iter = index.get_range(key(1), key(7), 64).await.unwrap();
        let mut cur_results = Vec::new();
        while let Some(kv) = cur_iter.next().await.unwrap() {
            cur_results.push(kv);
        }
        assert_eq!(cur_results.len(), 5);
        assert_eq!(cur_results[1].0, key(3));
        assert_eq!(cur_results[1].1, value(3, 2), "current should see v2 for key 3");
    }

    // ── 6. Snapshot reverse range scan ───────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_range_reverse() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        for id in 1u64..=4 {
            index.put(key(id), value(id, 1)).await.unwrap();
        }

        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Write more after snapshot
        index.put(key(5), value(5, 1)).await.unwrap();

        let mut iter = snap.get_range_reverse(key(1), key(5), 64).await.unwrap();
        let mut results = Vec::new();
        while let Some(kv) = iter.next().await.unwrap() {
            results.push(kv);
        }

        // Snapshot sees keys 1..=4 in reverse order (4, 3, 2, 1)
        assert_eq!(results.len(), 4);
        assert_eq!(results[0].0, key(4));
        assert_eq!(results[1].0, key(3));
        assert_eq!(results[2].0, key(2));
        assert_eq!(results[3].0, key(1));
    }

    // ── 7. get_snapshot on plain table → error ────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_on_plain_table_errors() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", plain_spec()).await.unwrap();
        let index = table.primary_index();

        let result = TableIndex::get_snapshot(Arc::clone(&index));
        assert!(result.is_err(), "get_snapshot on a plain table must return an error");
    }

    // ── 8. Snapshot deregisters on Drop ──────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_drop_deregisters() {
        let registry = SnapshotRegistry::new();

        assert!(registry.is_empty(), "registry should be empty before any snapshots");

        let ts1 = registry.register();
        let ts2 = registry.register();
        assert!(ts1 < ts2, "each registration must produce a strictly larger ts");

        assert!(!registry.is_empty());
        assert_eq!(registry.min_active_snapshot_ts(), ts1);

        registry.release(ts1);
        assert_eq!(registry.min_active_snapshot_ts(), ts2);

        registry.release(ts2);
        assert!(registry.is_empty(), "registry should be empty after all snapshots dropped");
        // When empty, min_active_snapshot_ts() returns GLOBAL_SEQ (>= ts2 + 1).
        assert!(registry.min_active_snapshot_ts() > ts2);
    }

    // ── 9. Snapshot RAII — Snapshot::drop unregisters via TableIndex ──────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_raii_drop() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        index.put(key(1), value(1, 1)).await.unwrap();

        let ts_before = GLOBAL_SEQ.load(Ordering::Acquire);

        {
            let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();
            let ts = snap.ts();
            assert!(ts >= ts_before, "snapshot ts should be >= ts_before");

            // Can read through the snapshot while it is live
            assert_eq!(snap.get(key(1)).await.unwrap(), Some(value(1, 1)));
            // snap is dropped here
        }

        // After drop, write proceeds normally — no hanging snapshot preventing GC
        index.put(key(1), value(1, 2)).await.unwrap();
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 2)));
    }

    // ── 10. Multiple concurrent snapshots — min reflects oldest ──────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_multiple_snapshots_min_tracking() {
        let registry = SnapshotRegistry::new();

        let ts1 = registry.register();
        let ts2 = registry.register();
        let ts3 = registry.register();
        assert!(ts1 < ts2 && ts2 < ts3);

        assert_eq!(registry.min_active_snapshot_ts(), ts1);

        // Release oldest two — min advances to ts3
        registry.release(ts1);
        assert_eq!(registry.min_active_snapshot_ts(), ts2);
        registry.release(ts2);
        assert_eq!(registry.min_active_snapshot_ts(), ts3);

        // Release last
        registry.release(ts3);
        assert!(registry.is_empty());
    }

    // ── 11. Inline GC — many writes, no active snapshot, no corruption ────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_inline_gc_no_active_snapshot() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Write 20 versions of key 1 with no active snapshots — inline GC
        // should prune old versions to prevent unbounded growth.
        for v in 1u8..=20 {
            index.put(key(1), value(1, v)).await.unwrap();
        }

        // Non-snapshot get returns the latest version (v=20)
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 20)));

        // Verify other keys are unaffected
        index.put(key(2), value(2, 1)).await.unwrap();
        assert_eq!(index.get(key(2)).await.unwrap(), Some(value(2, 1)));
    }

    // ── 12. Inline GC respects active snapshot ────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_inline_gc_respects_snapshot() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // v1
        index.put(key(1), value(1, 1)).await.unwrap();
        // Snapshot pins v1
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Write many more versions — inline GC must NOT remove v1 while snap is live
        for v in 2u8..=10 {
            index.put(key(1), value(1, v)).await.unwrap();
        }

        // snap must still see v1
        assert_eq!(snap.get(key(1)).await.unwrap(), Some(value(1, 1)));
        // Current must see v10
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 10)));
    }

    // ── 13. Deferred GC — tombstone cleanup ───────────────────────────────
    //
    // Write a key, delete it (tombstone), release all snapshots, run GC cycle.
    // After GC the btree should hold no versions of the key.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_deferred_gc_tombstone_cleanup() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        index.put(key(1), value(1, 1)).await.unwrap();
        index.remove(key(1)).await.unwrap();

        // No active snapshots — both live value and tombstone are eligible for GC.
        let gc = index.mvcc_gc().unwrap();
        let mut pending = std::collections::HashMap::new();
        gc.run_cycle(&mut pending).await;

        // After GC: the key no longer exists in the btree at all.
        assert_eq!(index.get(key(1)).await.unwrap(), None);
        // No leftover versions (both tombstone and live value removed).
        assert!(pending.is_empty(), "pending should be empty after full tombstone GC");
    }

    // ── 14. Deferred GC — old versions pruned, live anchor kept ──────────
    //
    // Write 5 versions of the same key with no active snapshots.
    // After one GC cycle the btree holds exactly 1 version (the latest).

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_deferred_gc_old_versions() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        for v in 1u8..=5 {
            index.put(key(1), value(1, v)).await.unwrap();
        }

        // Verify latest value before GC.
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 5)));

        let gc = index.mvcc_gc().unwrap();
        let mut pending = std::collections::HashMap::new();
        gc.run_cycle(&mut pending).await;

        // After GC the latest value is still correct.
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 5)));
    }

    // ── 15. Deferred GC — carry-forward across cycles ─────────────────────
    //
    // V1 written → snapshot S taken → V2 written → run_cycle():
    //   S pins V1, so V1 must NOT be removed yet.
    // Drop S → run_cycle():
    //   now V1 is eligible and should be removed.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_deferred_gc_carry_forward() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Write V1.
        index.put(key(1), value(1, 1)).await.unwrap();

        // Snapshot S pins V1.
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();
        let snap_ts = snap.ts();

        // Write V2 (V1 becomes an old version).
        index.put(key(1), value(1, 2)).await.unwrap();

        // First GC cycle — S is still alive; V1 must not be removed.
        let gc = index.mvcc_gc().unwrap();
        let mut pending = std::collections::HashMap::new();
        gc.run_cycle(&mut pending).await;

        // V1 must still be visible through snapshot S.
        assert_eq!(snap.get(key(1)).await.unwrap(), Some(value(1, 1)),
            "snapshot at ts={} must still see V1 after first GC cycle", snap_ts);
        // Current non-snapshot get must see V2.
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 2)));

        // Drop snapshot S — V1 is now fully eligible.
        drop(snap);

        // Second GC cycle — now V1 should be removed.
        gc.run_cycle(&mut pending).await;

        // Current non-snapshot get must still see V2 (live anchor preserved).
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 2)),
            "V2 (live anchor) must survive the second GC cycle");
    }
}

// ─────────────────────────── Test wrappers ────────────────────────────────

macro_rules! generate_tests {
    ($($test_fn:ident),* $(,)?) => {
        $(
            cfg_if::cfg_if! {
                if #[cfg(feature = "async_frontend")] {
                    #[iomgr::iomanager_test]
                    async fn $test_fn() {
                        test_impls::$test_fn().await;
                    }
                } else if #[cfg(feature = "sync_frontend")] {
                    #[test]
                    fn $test_fn() {
                        test_impls::$test_fn();
                    }
                }
            }
        )*
    };
}

generate_tests!(
    test_mvcc_basic_crud,
    test_snapshot_point_in_time,
    test_snapshot_absent_key,
    test_snapshot_tombstone_isolation,
    test_snapshot_range_scan,
    test_snapshot_range_reverse,
    test_snapshot_on_plain_table_errors,
    test_snapshot_drop_deregisters,
    test_snapshot_raii_drop,
    test_multiple_snapshots_min_tracking,
    test_inline_gc_no_active_snapshot,
    test_inline_gc_respects_snapshot,
    test_deferred_gc_tombstone_cleanup,
    test_deferred_gc_old_versions,
    test_deferred_gc_carry_forward,
);
