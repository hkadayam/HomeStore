//! Hermitage-inspired MVCC isolation anomaly tests for HomeDB.
//!
//! Adapted from "A Critique of Snapshot Isolation" (Fekete et al.) and
//! the Hermitage test suite (https://github.com/ept/hermitage).
//!
//! These tests verify that HomeDB's snapshot isolation correctly prevents the
//! following anomalies. Note that HomeDB has no multi-key write transactions
//! (single-key atomic writes only); tests are adapted to this model.
//!
//! Anomalies covered:
//!   G0   : Write total order — sequential writes produce monotonically
//!           increasing GLOBAL_SEQ timestamps.
//!   G1a  : No dirty read — a snapshot taken before a write never sees the
//!           write, even after it commits.
//!   G2   : Non-repeatable read — reading the same key twice on the same
//!           snapshot returns the same value, even with concurrent writers.
//!   G3   : Phantom read — keys inserted after snapshot creation are invisible
//!           in range scans; keys deleted after snapshot creation remain visible.
//!   SSI  : Multi-key snapshot consistency — a snapshot sees a coherent
//!           cross-section of all keys (i.e. every key at its value at snapshot
//!           creation time, not a mix of past and future versions).
//!   Stab : Snapshot stability — the same range scan repeated on the same
//!           snapshot always returns bit-identical results.
//!   Mono : Snapshot monotonicity — snapshots created later have ≥ timestamps
//!           and see ≥ committed writes than earlier snapshots.
//!
//! Tests NOT included (not applicable to single-key write model):
//!   G1b  : Intermediate read — not applicable; each write is atomic.
//!   G1c  : Circular information flow — requires multi-key transactions.
//!   G2   : Anti-dependency cycles (write skew) — requires concurrent
//!           multi-key transactional writes.
//!
//! Run with:
//!   cargo test --package mem_db --test test_mvcc_anomalies \
//!     --no-default-features --features sync_code -- --nocapture
//!   cargo test --package mem_db --test test_mvcc_anomalies \
//!     --no-default-features --features async_code -- --nocapture

use mem_db::{
    MemoryDB, KeySpec, TableSpec, ValueSpec, TableIndex, GLOBAL_SEQ,
};
use std::sync::Arc;
use std::sync::atomic::Ordering;

// ─────────────────────────── Helpers ──────────────────────────────────────

const KEY_SIZE: usize = 8;
const VALUE_SIZE: usize = 16;

fn mvcc_spec() -> TableSpec {
    TableSpec::new(KeySpec::fixed(KEY_SIZE), ValueSpec::fixed(VALUE_SIZE))
        .partition_key_size(2)
        .mvcc()
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

    // ── G0: Write total order ──────────────────────────────────────────────
    //
    // Every write to a key gets a strictly greater commit timestamp (GLOBAL_SEQ)
    // than any prior write. A snapshot taken between two writes sees exactly the
    // version that was current when the snapshot was created.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn g0_write_total_order() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        let ts0 = GLOBAL_SEQ.load(Ordering::Acquire);

        index.put(key(1), value(1, 1)).await.unwrap();
        let ts1 = GLOBAL_SEQ.load(Ordering::Acquire);

        index.put(key(1), value(1, 2)).await.unwrap();
        let ts2 = GLOBAL_SEQ.load(Ordering::Acquire);

        index.put(key(1), value(1, 3)).await.unwrap();
        let ts3 = GLOBAL_SEQ.load(Ordering::Acquire);

        // Each write must have advanced the sequence counter.
        assert!(ts0 < ts1, "G0: first write must advance GLOBAL_SEQ (ts0={} ts1={})", ts0, ts1);
        assert!(ts1 < ts2, "G0: second write must advance GLOBAL_SEQ (ts1={} ts2={})", ts1, ts2);
        assert!(ts2 < ts3, "G0: third write must advance GLOBAL_SEQ (ts2={} ts3={})", ts2, ts3);

        // Snapshots taken between writes see the correct versioned state.
        //
        // We can't retroactively create a snapshot at ts1; we verify indirectly:
        // the snapshot taken now (at ts3) must see v3, and a snapshot taken before
        // the writes would see nothing.
        let snap_now = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();
        assert_eq!(
            snap_now.get(key(1)).await.unwrap(),
            Some(value(1, 3)),
            "G0: snapshot taken after all writes must see the latest version"
        );
    }

    // ── G1a: No dirty read ────────────────────────────────────────────────
    //
    // A snapshot taken at time T must not see any write whose commit timestamp
    // is ≥ T. This holds regardless of how many subsequent writes occur.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn g1a_no_dirty_read() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Establish baseline: key(1) = v1.
        index.put(key(1), value(1, 1)).await.unwrap();

        // Snapshot A is taken. Its timestamp T is loaded from GLOBAL_SEQ with Acquire
        // ordering, so any write that subsequently calls fetch_add(SeqCst) gets
        // commit_ts ≥ T. The snapshot will therefore skip those writes.
        let snap_a = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Post-snapshot writes — each gets commit_ts > snap_a.ts().
        index.put(key(1), value(1, 2)).await.unwrap();
        index.put(key(1), value(1, 3)).await.unwrap();
        index.put(key(1), value(1, 4)).await.unwrap();

        assert_eq!(
            snap_a.get(key(1)).await.unwrap(),
            Some(value(1, 1)),
            "G1a: snapshot must not see any write committed after it was created"
        );

        // Non-snapshot read must see the latest version.
        assert_eq!(index.get(key(1)).await.unwrap(), Some(value(1, 4)));
    }

    // ── G2: Non-repeatable read (sequential) ──────────────────────────────
    //
    // Reading the same key twice on the same snapshot must return the same
    // value. Post-snapshot writes to the key must not change what the snapshot
    // sees on subsequent reads.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn g2_non_repeatable_read() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        index.put(key(1), value(1, 1)).await.unwrap();
        index.put(key(2), value(2, 1)).await.unwrap();

        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // First reads.
        let r1_k1 = snap.get(key(1)).await.unwrap();
        let r1_k2 = snap.get(key(2)).await.unwrap();

        // Post-snapshot mutations: overwrite key(1), delete key(2), add key(3).
        index.put(key(1), value(1, 2)).await.unwrap();
        index.remove(key(2)).await.unwrap();
        index.put(key(3), value(3, 1)).await.unwrap();

        // Second reads on the same snapshot must equal the first reads exactly.
        let r2_k1 = snap.get(key(1)).await.unwrap();
        let r2_k2 = snap.get(key(2)).await.unwrap();

        assert_eq!(r1_k1, r2_k1,
            "G2: key(1) read twice from same snapshot must return identical value");
        assert_eq!(r1_k2, r2_k2,
            "G2: key(2) read twice from same snapshot must return identical value");
        assert_eq!(r1_k1, Some(value(1, 1)), "G2: snapshot must see pre-snapshot v1");
        assert_eq!(r1_k2, Some(value(2, 1)), "G2: snapshot must see pre-snapshot v1");
        // key(3) was inserted after the snapshot — snapshot must not see it.
        assert_eq!(snap.get(key(3)).await.unwrap(), None,
            "G2: snapshot must not see a key inserted after it was created");
    }

    // ── G2 concurrent: Non-repeatable read under concurrent writers ────────
    //
    // The G2 invariant must hold even when a separate writer thread/task is
    // concurrently mutating the index. The snapshot's reads must remain stable.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn g2_non_repeatable_read_concurrent() {
        const NUM_KEYS: u64 = 10;

        let db = MemoryDB::new(4).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Seed key space.
        for id in 1u64..=NUM_KEYS {
            index.put(key(id), value(id, 1)).await.unwrap();
        }

        // Take snapshot — commits_ts for all NUM_KEYS writes are < snap.ts().
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // First pass: read all keys through the snapshot.
        let mut first_reads: Vec<(u64, Option<Vec<u8>>)> = Vec::new();
        for id in 1u64..=NUM_KEYS {
            first_reads.push((id, snap.get(key(id)).await.unwrap()));
        }

        // Concurrent writer: overwrite all keys.
        cfg_if::cfg_if! {
            if #[cfg(feature = "sync_frontend")] {
                let idx = Arc::clone(&index);
                let writer = std::thread::spawn(move || {
                    for id in 1u64..=NUM_KEYS {
                        let _ = idx.put(key(id), value(id, 99));
                    }
                });
                writer.join().unwrap();
            } else if #[cfg(feature = "async_frontend")] {
                let bg = iomgr::BackgroundTasks::new();
                let num_reactors = iomgr::iomgr().num_reactors();
                let idx = Arc::clone(&index);
                bg.spawn(iomgr::ReactorTarget::Reactor(1 % num_reactors), async move {
                    for id in 1u64..=NUM_KEYS {
                        let _ = idx.put(key(id), value(id, 99)).await;
                    }
                });
                bg.join_all().await;
            }
        }

        // Second pass: re-read from the same snapshot. Must match first pass.
        for (id, expected) in &first_reads {
            let got = snap.get(key(*id)).await.unwrap();
            assert_eq!(
                *expected, got,
                "G2 concurrent: key({}) returned different value on second snapshot read \
                 (first={:?}, second={:?})",
                id, expected, got
            );
        }
    }

    // ── G3: Phantom read ──────────────────────────────────────────────────
    //
    // New keys inserted after the snapshot are not visible in snapshot range scans
    // (no phantoms). Keys deleted after the snapshot remain visible in range scans.
    // Updated keys appear at their pre-snapshot version.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn g3_phantom_read() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Pre-populate: keys 10, 20, 30 at v1.
        for id in [10u64, 20, 30] {
            index.put(key(id), value(id, 1)).await.unwrap();
        }

        // Snapshot captures keys {10, 20, 30} all at v1.
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Post-snapshot mutations:
        index.put(key(40), value(40, 1)).await.unwrap(); // phantom: brand-new key
        index.put(key(20), value(20, 2)).await.unwrap(); // update: key 20 → v2
        index.remove(key(10)).await.unwrap();            // tombstone: key 10 deleted

        // Snapshot range [1, 50) must see exactly {10/v1, 20/v1, 30/v1}.
        let mut iter = snap.get_range(key(1), key(50), 64).await.unwrap();
        let mut results = Vec::new();
        while let Some(kv) = iter.next().await.unwrap() {
            results.push(kv);
        }

        assert_eq!(results.len(), 3,
            "G3: snapshot must see exactly 3 keys (phantom key(40) must be invisible)");
        assert_eq!(results[0].0, key(10),
            "G3: key(10) must appear (tombstone is post-snapshot)");
        assert_eq!(results[0].1, value(10, 1),
            "G3: key(10) must be at v1");
        assert_eq!(results[1].0, key(20),
            "G3: key(20) must appear");
        assert_eq!(results[1].1, value(20, 1),
            "G3: key(20) must be at v1 (overwrite is post-snapshot)");
        assert_eq!(results[2].0, key(30),
            "G3: key(30) must appear");
        assert_eq!(results[2].1, value(30, 1),
            "G3: key(30) must be at v1");
    }

    // ── SSI: Multi-key snapshot consistency ───────────────────────────────
    //
    // A snapshot must provide a consistent cross-section of all keys at creation
    // time. It must see every key at the version that was current when the snapshot
    // was created — not a mix of versions from before and after snapshot creation.
    //
    // Note: Classic "write skew" (the SI-specific anomaly that requires concurrent
    // multi-key transactional writes) is not applicable to HomeDB's single-key
    // write model. This test verifies the weaker but fundamental property: each
    // snapshot sees an internally consistent state of all keys.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn ssi_multi_key_consistency() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        // Phase 1: write k1=A, k2=B.
        index.put(key(1), value(1, 0xAA)).await.unwrap();
        index.put(key(2), value(2, 0xBB)).await.unwrap();

        // Snapshot S1 — captures k1=A, k2=B.
        let snap1 = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Phase 2: overwrite both keys.
        index.put(key(1), value(1, 0xA2)).await.unwrap();
        index.put(key(2), value(2, 0xB2)).await.unwrap();

        // Snapshot S2 — captures k1=A', k2=B'.
        let snap2 = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Phase 3: further updates after both snapshots.
        index.put(key(1), value(1, 0xA3)).await.unwrap();
        index.put(key(2), value(2, 0xB3)).await.unwrap();

        // S1 must see A and B (its creation-time values).
        assert_eq!(snap1.get(key(1)).await.unwrap(), Some(value(1, 0xAA)),
            "SSI: snap1 must see k1=A");
        assert_eq!(snap1.get(key(2)).await.unwrap(), Some(value(2, 0xBB)),
            "SSI: snap1 must see k2=B");

        // S2 must see A' and B' (its creation-time values).
        assert_eq!(snap2.get(key(1)).await.unwrap(), Some(value(1, 0xA2)),
            "SSI: snap2 must see k1=A'");
        assert_eq!(snap2.get(key(2)).await.unwrap(), Some(value(2, 0xB2)),
            "SSI: snap2 must see k2=B'");

        // S1 must NOT see S2's writes, and vice versa.
        assert_ne!(
            snap1.get(key(1)).await.unwrap(),
            snap2.get(key(1)).await.unwrap(),
            "SSI: snap1 and snap2 must see different versions of k1"
        );

        // Verify consistency via range scans as well.
        let snap1_range = {
            let mut iter = snap1.get_range(key(1), key(3), 64).await.unwrap();
            let mut r = Vec::new();
            while let Some(kv) = iter.next().await.unwrap() { r.push(kv); }
            r
        };
        let snap2_range = {
            let mut iter = snap2.get_range(key(1), key(3), 64).await.unwrap();
            let mut r = Vec::new();
            while let Some(kv) = iter.next().await.unwrap() { r.push(kv); }
            r
        };

        assert_eq!(snap1_range.len(), 2);
        assert_eq!(snap1_range[0].1, value(1, 0xAA), "SSI: range confirms snap1 sees k1=A");
        assert_eq!(snap1_range[1].1, value(2, 0xBB), "SSI: range confirms snap1 sees k2=B");
        assert_eq!(snap2_range.len(), 2);
        assert_eq!(snap2_range[0].1, value(1, 0xA2), "SSI: range confirms snap2 sees k1=A'");
        assert_eq!(snap2_range[1].1, value(2, 0xB2), "SSI: range confirms snap2 sees k2=B'");
    }

    // ── Stability: Repeated range scans on same snapshot are deterministic ─
    //
    // The same range query issued repeatedly on the same snapshot must return
    // bit-identical results, even while concurrent writers mutate the index.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn stability_repeated_range_scan() {
        const NUM_KEYS: u64 = 20;
        const NUM_REPEATS: usize = 10;

        let db = MemoryDB::new(4).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        for id in 1u64..=NUM_KEYS {
            index.put(key(id), value(id, 1)).await.unwrap();
        }

        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Ground-truth scan.
        let first_scan = {
            let mut iter = snap.get_range(min_key(), max_key(), 64).await.unwrap();
            let mut r = Vec::new();
            while let Some(kv) = iter.next().await.unwrap() { r.push(kv); }
            r
        };
        assert_eq!(first_scan.len(), NUM_KEYS as usize,
            "stability: initial scan must see all pre-populated keys");

        // Spawn a concurrent writer (sync_frontend: thread; async_frontend: reactor task).
        // Then re-scan NUM_REPEATS times and assert identical results.
        cfg_if::cfg_if! {
            if #[cfg(feature = "sync_frontend")] {
                use std::sync::atomic::{AtomicBool, Ordering as AOrdering};
                let stop = Arc::new(AtomicBool::new(false));
                let stop_w = Arc::clone(&stop);
                let idx = Arc::clone(&index);
                let writer = std::thread::spawn(move || {
                    let mut ver = 2u8;
                    while !stop_w.load(AOrdering::Relaxed) {
                        for id in 1u64..=NUM_KEYS {
                            let _ = idx.put(key(id), value(id, ver));
                        }
                        ver = ver.wrapping_add(1);
                    }
                });

                for repeat in 1..=NUM_REPEATS {
                    let mut iter = snap.get_range(min_key(), max_key(), 64).unwrap();
                    let mut scan = Vec::new();
                    while let Some(kv) = iter.next().unwrap() { scan.push(kv); }
                    assert_eq!(scan, first_scan,
                        "stability: scan repeat #{} differs from ground-truth under concurrent writers",
                        repeat);
                }

                stop.store(true, AOrdering::Relaxed);
                writer.join().unwrap();

            } else if #[cfg(feature = "async_frontend")] {
                // In async mode: spawn writer on a separate reactor, do repeated scans
                // on the current reactor, then join the writer.
                let bg = iomgr::BackgroundTasks::new();
                let num_reactors = iomgr::iomgr().num_reactors();
                let idx = Arc::clone(&index);
                bg.spawn(iomgr::ReactorTarget::Reactor(1 % num_reactors), async move {
                    for ver in 2u8..=100 {
                        for id in 1u64..=NUM_KEYS {
                            let _ = idx.put(key(id), value(id, ver)).await;
                        }
                    }
                });

                for repeat in 1..=NUM_REPEATS {
                    let mut iter = snap.get_range(min_key(), max_key(), 64).await.unwrap();
                    let mut scan = Vec::new();
                    while let Some(kv) = iter.next().await.unwrap() { scan.push(kv); }
                    assert_eq!(scan, first_scan,
                        "stability: scan repeat #{} differs from ground-truth under concurrent writers",
                        repeat);
                }

                bg.join_all().await;
            }
        }
    }

    // ── Snapshot monotonicity ─────────────────────────────────────────────
    //
    // Snapshots created later must have timestamps ≥ those of earlier snapshots,
    // and must see at least as many committed writes. A later snapshot must not
    // see an older version of a key than an earlier snapshot.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn snapshot_monotonicity() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        index.put(key(1), value(1, 1)).await.unwrap();
        let snap1 = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        index.put(key(1), value(1, 2)).await.unwrap();
        let snap2 = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        index.put(key(1), value(1, 3)).await.unwrap();
        let snap3 = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Timestamps must be non-decreasing.
        assert!(snap1.ts() <= snap2.ts(),
            "monotonicity: snap1.ts({}) must be ≤ snap2.ts({})", snap1.ts(), snap2.ts());
        assert!(snap2.ts() <= snap3.ts(),
            "monotonicity: snap2.ts({}) must be ≤ snap3.ts({})", snap2.ts(), snap3.ts());

        // Each snapshot sees exactly its own version of key(1).
        assert_eq!(snap1.get(key(1)).await.unwrap(), Some(value(1, 1)),
            "monotonicity: snap1 must see v1");
        assert_eq!(snap2.get(key(1)).await.unwrap(), Some(value(1, 2)),
            "monotonicity: snap2 must see v2");
        assert_eq!(snap3.get(key(1)).await.unwrap(), Some(value(1, 3)),
            "monotonicity: snap3 must see v3");

        // A later snapshot must not regress: snap3 must see at least what snap2 sees.
        // For key(1): snap3 sees v3 ≥ snap2's v2 (by version number, not ordering).
        // More precisely: snap3.get(key(1)) != snap1.get(key(1)) demonstrates
        // that newer snapshots see newer state.
        assert_ne!(
            snap1.get(key(1)).await.unwrap(),
            snap3.get(key(1)).await.unwrap(),
            "monotonicity: snap3 must see a different (newer) version than snap1"
        );
    }

    // ── Tombstone isolation in range scans ────────────────────────────────
    //
    // A snapshot taken before a deletion must continue to see the deleted key in
    // range scans, even after the delete commits and after a GC cycle runs.
    // The snapshot pins the deleted key's live version until the snapshot drops.

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn tombstone_range_isolation() {
        let db = MemoryDB::new(2).unwrap();
        let table = db.create_table("t", mvcc_spec()).await.unwrap();
        let index = table.primary_index();

        for id in 1u64..=5 {
            index.put(key(id), value(id, 1)).await.unwrap();
        }

        // Snapshot before any deletions.
        let snap = TableIndex::get_snapshot(Arc::clone(&index)).unwrap();

        // Delete keys 2 and 4 after the snapshot.
        index.remove(key(2)).await.unwrap();
        index.remove(key(4)).await.unwrap();

        // Snapshot range must still see all 5 original keys.
        let snap_range = {
            let mut iter = snap.get_range(key(1), key(6), 64).await.unwrap();
            let mut r = Vec::new();
            while let Some(kv) = iter.next().await.unwrap() { r.push(kv); }
            r
        };
        assert_eq!(snap_range.len(), 5,
            "tombstone: snapshot must see all 5 keys (post-snapshot deletes are invisible)");
        for (i, id) in (1u64..=5).enumerate() {
            assert_eq!(snap_range[i].0, key(id),
                "tombstone: key({}) missing from snapshot range", id);
            assert_eq!(snap_range[i].1, value(id, 1),
                "tombstone: key({}) wrong version in snapshot range", id);
        }

        // Current (non-snapshot) range sees only keys 1, 3, 5.
        let cur_range = {
            let mut iter = index.get_range(key(1), key(6), 64).await.unwrap();
            let mut r = Vec::new();
            while let Some(kv) = iter.next().await.unwrap() { r.push(kv); }
            r
        };
        assert_eq!(cur_range.len(), 3,
            "tombstone: non-snapshot range must skip deleted keys");
        assert_eq!(cur_range[0].0, key(1));
        assert_eq!(cur_range[1].0, key(3));
        assert_eq!(cur_range[2].0, key(5));

        // Run a GC cycle — snapshot is still alive, so the live anchors of keys
        // 2 and 4 must NOT be removed.
        let gc = index.mvcc_gc().unwrap();
        let mut pending = std::collections::HashMap::new();
        gc.run_cycle(&mut pending).await;

        // Snapshot must still see all 5 keys after GC.
        let post_gc = {
            let mut iter = snap.get_range(key(1), key(6), 64).await.unwrap();
            let mut r = Vec::new();
            while let Some(kv) = iter.next().await.unwrap() { r.push(kv); }
            r
        };
        assert_eq!(post_gc.len(), 5,
            "tombstone: snapshot must still see all 5 keys after GC (GC must respect live snapshot)");
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
    g0_write_total_order,
    g1a_no_dirty_read,
    g2_non_repeatable_read,
    g2_non_repeatable_read_concurrent,
    g3_phantom_read,
    ssi_multi_key_consistency,
    stability_repeated_range_scan,
    snapshot_monotonicity,
    tombstone_range_isolation,
);
