//! Comprehensive MemDB correctness tests with shadow-map verification.
//!
//! Tests variable-size key/value pairs across all three compilation modes.
//! Sequential and random operations are fully verified against a reference BTreeMap.
//! Concurrent tests exercise ShardedBtree partition routing with many partitions.
//!
//! Key sizing is chosen to maximise shard spread in ShardedBtree:
//!   MIN_KEY_SIZE = 4, MAX_KEY_SIZE = 16
//!   PARTITION_KEY_SIZE = MIN_KEY_SIZE - 1 = 3  ← forces many distinct partitions
//!
//! Run with:
//!   cargo test --package mem_db --test test_mem_db --no-default-features --features sync_code
//!   cargo test --package mem_db --test test_mem_db --no-default-features --features async_code
//!   cargo test --package mem_db --test test_mem_db --no-default-features --features sync_over_async_code

use mem_db::{MemoryDB, TableSpec, KeySpec, ValueSpec};
use std::collections::BTreeMap;
use std::sync::Arc;
use rand::{rngs::StdRng, Rng, SeedableRng, seq::SliceRandom};

// ─────────────────────────── Parameters ───────────────────────────────────

/// Minimum key size in bytes.  First 4 bytes encode the item id (big-endian).
const MIN_KEY_SIZE: usize = 4;
/// Maximum key size in bytes.
const MAX_KEY_SIZE: usize = 16;
/// Number of leading bytes used to identify a ShardedBtree partition.
/// 1 byte = 256 possible prefixes — enough to spread keys across shards while
/// keeping range scan iteration bounded (at most 256 active partitions per pass).
const PARTITION_KEY_SIZE: usize = 1;

const MIN_VALUE_SIZE: usize = 4;
const MAX_VALUE_SIZE: usize = 32;

/// Entries used in single-threaded correctness tests.
const NUM_ENTRIES: usize = 2000;
/// Workers for concurrent tests.
const NUM_WORKERS: usize = 4;
/// Operations per worker in concurrent tests.
const NUM_OPS_PER_WORKER: usize = 500;
/// Key space shared across all concurrent workers (forces contention).
const CONCURRENT_KEY_SPACE: u32 = 300;

// ─────────────────────────── Key/value helpers ────────────────────────────

/// Variable-size key for id `id`.
/// First 4 bytes = big-endian `id` (ensures lex order == numeric order).
/// Extra bytes = deterministic padding from `rng`.
fn gen_key(id: u32, rng: &mut StdRng) -> Vec<u8> {
    let size = rng.gen_range(MIN_KEY_SIZE..=MAX_KEY_SIZE);
    let mut key = vec![0u8; size];
    key[0..4].copy_from_slice(&id.to_be_bytes());
    for b in key[4..].iter_mut() { *b = rng.gen(); }
    key
}

/// Variable-size value for id `id`.
/// First 4 bytes = big-endian `id` (used for verification).
fn gen_value(id: u32, rng: &mut StdRng) -> Vec<u8> {
    let size = rng.gen_range(MIN_VALUE_SIZE..=MAX_VALUE_SIZE);
    let mut val = vec![0u8; size];
    val[0..4].copy_from_slice(&id.to_be_bytes());
    for b in val[4..].iter_mut() { *b = rng.gen(); }
    val
}

/// Deterministic variable-size key for a given `key_id` in concurrent tests.
/// The same `key_id` always produces the same byte sequence, so concurrent
/// workers share a consistent key space for put/get/remove contention.
fn worker_key(key_id: u32) -> Vec<u8> {
    let extra = (key_id as usize) % (MAX_KEY_SIZE - MIN_KEY_SIZE + 1);
    let mut key = key_id.to_be_bytes().to_vec();
    for i in 0..extra {
        key.push(key_id.wrapping_add(i as u32 * 31) as u8);
    }
    key
}

/// Deterministic variable-size value for a given `key_id` in concurrent tests.
fn worker_value(key_id: u32, worker_id: usize) -> Vec<u8> {
    let extra = (key_id as usize + worker_id) % (MAX_VALUE_SIZE - MIN_VALUE_SIZE + 1);
    let mut val = key_id.to_be_bytes().to_vec();
    for i in 0..extra {
        val.push(key_id.wrapping_add(worker_id as u32).wrapping_add(i as u32 * 17) as u8);
    }
    val
}

/// Inclusive lower bound for a full-range scan.
fn min_key() -> Vec<u8> { vec![0u8; MIN_KEY_SIZE] }

/// Exclusive upper bound for a full-range scan — larger than any key we insert.
fn max_key() -> Vec<u8> { vec![0xffu8; MAX_KEY_SIZE] }

// ─────────────────────────── TableSpec factory ────────────────────────────

fn make_spec() -> TableSpec {
    TableSpec::new(
        KeySpec::variable(MAX_KEY_SIZE),
        ValueSpec::variable(MAX_VALUE_SIZE),
    )
    .partition_key_size(PARTITION_KEY_SIZE)
}

// ─────────────────────────── Shadow map type ──────────────────────────────

type ShadowMap = BTreeMap<Vec<u8>, Vec<u8>>;

// ─────────────────────────── Test implementations ─────────────────────────

mod test_impls {
    use super::*;
    use mem_db::TableIndex;

    // ── Helpers ─────────────────────────────────────────────────────────────

    /// Verify every entry in the shadow map is present in the btree with the correct value.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn verify_all(index: &TableIndex, shadow: &ShadowMap) {
        for (key, expected) in shadow.iter() {
            let got = index.get(key.clone()).await
                .unwrap_or_else(|e| panic!("get({:?}) failed: {:?}", key, e));
            assert_eq!(
                got.as_deref(),
                Some(expected.as_slice()),
                "value mismatch for key {:?}", key
            );
        }
    }

    /// Verify every key NOT in the shadow map returns None from the btree.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn verify_absent(index: &TableIndex, keys: &[Vec<u8>]) {
        for key in keys {
            let got = index.get(key.clone()).await
                .unwrap_or_else(|e| panic!("get({:?}) failed: {:?}", key, e));
            assert!(got.is_none(), "expected key {:?} to be absent but found a value", key);
        }
    }

    /// Collect all entries from the btree via a paginated range scan.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn collect_range(index: &TableIndex) -> Vec<(Vec<u8>, Vec<u8>)> {
        let mut iter = index.get_range(min_key(), max_key(), 64)
            .await.expect("get_range failed");
        let mut out = Vec::new();
        loop {
            match iter.next().await.expect("iter.next() failed") {
                Some(kv) => out.push(kv),
                None => break,
            }
        }
        out
    }

    /// Assert the paginated range scan returns exactly the shadow map contents in sorted order.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn verify_query_all(index: &TableIndex, shadow: &ShadowMap) {
        let scanned = collect_range(index).await;
        let expected: Vec<(Vec<u8>, Vec<u8>)> =
            shadow.iter().map(|(k, v)| (k.clone(), v.clone())).collect();

        assert_eq!(
            scanned.len(), expected.len(),
            "range scan returned {} entries; shadow has {}", scanned.len(), expected.len()
        );
        for (i, ((sk, sv), (ek, ev))) in scanned.iter().zip(expected.iter()).enumerate() {
            assert_eq!(sk, ek, "key mismatch at scan position {}", i);
            assert_eq!(sv, ev, "value mismatch at scan position {} (key {:?})", i, sk);
        }
    }

    // ── Sequential insert ───────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_sequential_insert() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();
        let mut shadow = ShadowMap::new();
        let mut rng = StdRng::seed_from_u64(1);

        let entries: Vec<(Vec<u8>, Vec<u8>)> = (0..NUM_ENTRIES as u32)
            .map(|id| (gen_key(id, &mut rng), gen_value(id, &mut rng)))
            .collect();

        for (key, value) in &entries {
            index.put(key.clone(), value.clone()).await.unwrap();
            shadow.insert(key.clone(), value.clone());
        }

        verify_all(&index, &shadow).await;
        verify_query_all(&index, &shadow).await;
    }

    // ── Random insert ───────────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_random_insert() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();
        let mut shadow = ShadowMap::new();
        let mut rng = StdRng::seed_from_u64(2);

        let mut entries: Vec<(Vec<u8>, Vec<u8>)> = (0..NUM_ENTRIES as u32)
            .map(|id| (gen_key(id, &mut rng), gen_value(id, &mut rng)))
            .collect();
        entries.shuffle(&mut rng);

        for (key, value) in &entries {
            index.put(key.clone(), value.clone()).await.unwrap();
            shadow.insert(key.clone(), value.clone());
        }

        verify_all(&index, &shadow).await;
        verify_query_all(&index, &shadow).await;
    }

    // ── Sequential remove ───────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_sequential_remove() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();
        let mut shadow = ShadowMap::new();
        let mut rng = StdRng::seed_from_u64(3);

        let entries: Vec<(Vec<u8>, Vec<u8>)> = (0..NUM_ENTRIES as u32)
            .map(|id| (gen_key(id, &mut rng), gen_value(id, &mut rng)))
            .collect();

        for (key, value) in &entries {
            index.put(key.clone(), value.clone()).await.unwrap();
            shadow.insert(key.clone(), value.clone());
        }

        // Remove first half in sequential order; verify returned value matches shadow.
        let midpoint = NUM_ENTRIES / 2;
        let mut removed_keys = Vec::with_capacity(midpoint);
        for (key, _) in &entries[..midpoint] {
            let removed = index.remove(key.clone()).await.unwrap();
            let shadow_val = shadow.remove(key);
            assert_eq!(removed, shadow_val, "remove value mismatch for key {:?}", key);
            removed_keys.push(key.clone());
        }

        // Remaining entries are still correct; removed entries are gone.
        verify_all(&index, &shadow).await;
        verify_absent(&index, &removed_keys).await;
        verify_query_all(&index, &shadow).await;

        // Remove the second half.
        for (key, _) in &entries[midpoint..] {
            let removed = index.remove(key.clone()).await.unwrap();
            assert!(removed.is_some(), "expected key {:?} to exist during cleanup", key);
            shadow.remove(key);
        }

        // Btree must now be empty.
        let remaining = collect_range(&index).await;
        assert!(remaining.is_empty(),
            "expected empty btree after removing all entries, found {}", remaining.len());
    }

    // ── Random remove ───────────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_random_remove() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();
        let mut shadow = ShadowMap::new();
        let mut rng = StdRng::seed_from_u64(4);

        let entries: Vec<(Vec<u8>, Vec<u8>)> = (0..NUM_ENTRIES as u32)
            .map(|id| (gen_key(id, &mut rng), gen_value(id, &mut rng)))
            .collect();

        for (key, value) in &entries {
            index.put(key.clone(), value.clone()).await.unwrap();
            shadow.insert(key.clone(), value.clone());
        }

        // Shuffle the removal order; verify each removed value matches shadow.
        let mut keys: Vec<Vec<u8>> = entries.iter().map(|(k, _)| k.clone()).collect();
        keys.shuffle(&mut rng);

        for key in &keys {
            let removed = index.remove(key.clone()).await.unwrap();
            let shadow_val = shadow.remove(key);
            assert_eq!(removed, shadow_val, "remove value mismatch for key {:?}", key);
        }

        let remaining = collect_range(&index).await;
        assert!(remaining.is_empty(),
            "expected empty btree after removing all entries, found {}", remaining.len());
    }

    // ── Update (overwrite) ──────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_update_overwrites() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();
        let mut shadow = ShadowMap::new();
        let mut rng = StdRng::seed_from_u64(5);

        // Insert initial values.
        let entries: Vec<(Vec<u8>, Vec<u8>)> = (0..500u32)
            .map(|id| (gen_key(id, &mut rng), gen_value(id, &mut rng)))
            .collect();
        for (key, value) in &entries {
            index.put(key.clone(), value.clone()).await.unwrap();
            shadow.insert(key.clone(), value.clone());
        }

        // Overwrite every entry with a new (larger) value; verify old values are replaced.
        let mut rng2 = StdRng::seed_from_u64(55);
        for (key, _) in &entries {
            let new_val = gen_value(0xDEAD_BEEF, &mut rng2);
            index.put(key.clone(), new_val.clone()).await.unwrap();
            shadow.insert(key.clone(), new_val);
        }

        verify_all(&index, &shadow).await;
        verify_query_all(&index, &shadow).await;
    }

    // ── Mixed insert + remove interleaved ──────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_mixed_insert_remove() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();
        let mut shadow = ShadowMap::new();
        let mut rng = StdRng::seed_from_u64(6);

        let entries: Vec<(Vec<u8>, Vec<u8>)> = (0..NUM_ENTRIES as u32)
            .map(|id| (gen_key(id, &mut rng), gen_value(id, &mut rng)))
            .collect();

        // Pass 1: insert all.
        for (key, value) in &entries {
            index.put(key.clone(), value.clone()).await.unwrap();
            shadow.insert(key.clone(), value.clone());
        }

        // Pass 2: remove every 3rd entry, verify all others.
        for (i, (key, _)) in entries.iter().enumerate() {
            if i % 3 == 0 {
                index.remove(key.clone()).await.unwrap();
                shadow.remove(key);
            }
        }
        verify_all(&index, &shadow).await;

        // Pass 3: re-insert the removed entries with fresh values.
        let mut rng2 = StdRng::seed_from_u64(66);
        for (i, (key, _)) in entries.iter().enumerate() {
            if i % 3 == 0 {
                let new_val = gen_value(i as u32, &mut rng2);
                index.put(key.clone(), new_val.clone()).await.unwrap();
                shadow.insert(key.clone(), new_val);
            }
        }
        verify_all(&index, &shadow).await;
        verify_query_all(&index, &shadow).await;
    }

    // ── Concurrent worker ─────────────────────────────────────────────────

    /// Single worker: performs random puts, gets, and removes against a shared index.
    /// Uses `worker_key(key_id)` so all workers contend on the same key space.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn run_worker(index: Arc<TableIndex>, worker_id: usize, num_ops: usize) {
        let mut rng = StdRng::seed_from_u64(100 + worker_id as u64);
        for _ in 0..num_ops {
            let key_id = rng.gen_range(0..CONCURRENT_KEY_SPACE);
            let op = rng.gen_range(0u32..3);
            match op {
                0 => {
                    let _ = index.put(worker_key(key_id), worker_value(key_id, worker_id)).await;
                }
                1 => {
                    let _ = index.get(worker_key(key_id)).await;
                }
                _ => {
                    let _ = index.remove(worker_key(key_id)).await;
                }
            }
        }
    }

    // ── Concurrent multi-op ────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_concurrent_multiop() {
        let db = MemoryDB::new(NUM_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();

        // Pre-load a subset of the concurrent key space.
        for id in 0..CONCURRENT_KEY_SPACE {
            index.put(worker_key(id), worker_value(id, 0)).await.unwrap();
        }

        // Spawn NUM_WORKERS workers; each performs NUM_OPS_PER_WORKER mixed operations.
        cfg_if::cfg_if! {
            if #[cfg(feature = "sync_frontend")] {
                let mut handles = Vec::new();
                for w in 0..NUM_WORKERS {
                    let idx = Arc::clone(&index);
                    handles.push(std::thread::spawn(move || {
                        run_worker(idx, w, NUM_OPS_PER_WORKER);
                    }));
                }
                for h in handles { h.join().expect("worker thread panicked"); }
            } else if #[cfg(feature = "async_frontend")] {
                let bg = iomgr::BackgroundTasks::new();
                let num_reactors = iomgr::iomgr().num_reactors();
                for w in 0..NUM_WORKERS {
                    let idx = Arc::clone(&index);
                    bg.spawn(iomgr::ReactorTarget::Reactor(w % num_reactors), async move {
                        run_worker(idx, w, NUM_OPS_PER_WORKER).await;
                    });
                }
                bg.join_all().await;
            }
        }
        // Success = no panics during concurrent execution.
    }

    // ── Concurrent stress ──────────────────────────────────────────────────

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_concurrent_stress() {
        const STRESS_WORKERS: usize = 8;
        const STRESS_OPS: usize = 2000;
        const STRESS_KEY_SPACE: u32 = 500;

        let db = MemoryDB::new(STRESS_WORKERS).unwrap();
        let table = db.create_table("t", make_spec()).await.unwrap();
        let index = table.primary_index();

        // Pre-load the full stress key space.
        for id in 0..STRESS_KEY_SPACE {
            index.put(worker_key(id), worker_value(id, 0)).await.unwrap();
        }

        cfg_if::cfg_if! {
            if #[cfg(feature = "sync_frontend")] {
                let mut handles = Vec::new();
                for w in 0..STRESS_WORKERS {
                    let idx = Arc::clone(&index);
                    handles.push(std::thread::spawn(move || {
                        let mut rng = StdRng::seed_from_u64(200 + w as u64);
                        for _ in 0..STRESS_OPS {
                            let key_id = rng.gen_range(0..STRESS_KEY_SPACE);
                            match rng.gen_range(0u32..3) {
                                0 => { let _ = idx.put(worker_key(key_id), worker_value(key_id, w)); }
                                1 => { let _ = idx.get(worker_key(key_id)); }
                                _ => { let _ = idx.remove(worker_key(key_id)); }
                            }
                        }
                    }));
                }
                for h in handles { h.join().expect("stress worker panicked"); }
            } else if #[cfg(feature = "async_frontend")] {
                let bg = iomgr::BackgroundTasks::new();
                let num_reactors = iomgr::iomgr().num_reactors();
                for w in 0..STRESS_WORKERS {
                    let idx = Arc::clone(&index);
                    bg.spawn(iomgr::ReactorTarget::Reactor(w % num_reactors), async move {
                        let mut rng = StdRng::seed_from_u64(200 + w as u64);
                        for _ in 0..STRESS_OPS {
                            let key_id = rng.gen_range(0..STRESS_KEY_SPACE);
                            match rng.gen_range(0u32..3) {
                                0 => { let _ = idx.put(worker_key(key_id), worker_value(key_id, w)).await; }
                                1 => { let _ = idx.get(worker_key(key_id)).await; }
                                _ => { let _ = idx.remove(worker_key(key_id)).await; }
                            }
                        }
                    });
                }
                bg.join_all().await;
            }
        }
        // Success = no panics during concurrent stress.
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
    test_sequential_insert,
    test_random_insert,
    test_sequential_remove,
    test_random_remove,
    test_update_overwrites,
    test_mixed_insert_remove,
    test_concurrent_multiop,
    test_concurrent_stress,
);
