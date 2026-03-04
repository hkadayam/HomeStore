//! MVCC snapshot isolation stress test.
//!
//! Runs concurrent writers and snapshot readers for a fixed duration, verifying
//! that every key returned by a range scan matches the value returned by a point-get
//! on the same snapshot (point-get consistency invariant).
//!
//! Writers continuously put/remove random keys. Readers take a snapshot, do a full
//! range scan, then spot-check individual keys via point-get. Since writers run
//! concurrently, two scans on the same snapshot may legitimately differ (new versions
//! may have been GC'd between scans), but point-get on a live snapshot must always
//! agree with what the range scan returned.
//!
//! Run with:
//!   cargo test --release --package mem_db --test test_mvcc_stress \
//!     --no-default-features --features sync_code -- --nocapture
//!   cargo test --release --package mem_db --test test_mvcc_stress \
//!     --no-default-features --features async_code -- --nocapture

use mem_db::{MemoryDB, KeySpec, TableSpec, ValueSpec, TableIndex, Snapshot};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};
use rand::{rngs::StdRng, Rng, SeedableRng};

fn key_id(k: &[u8]) -> u64 { u64::from_le_bytes(k[..8].try_into().unwrap()) }
fn val_id(v: &[u8]) -> u64 { u64::from_le_bytes(v[..8].try_into().unwrap()) }

// ─────────────────────────── Configuration ────────────────────────────────────

const KEY_SIZE: usize = 32;
const VALUE_SIZE: usize = 128;
const KEY_RANGE: u64 = 50_000;
const PRELOAD_KEYS: u64 = 100_000;
const WRITER_WORKERS: usize = 16;
const READER_WORKERS: usize = 8;
const STRESS_SECS: u64 = 30;
const VERIFY_KEYS_PER_SNAP: usize = 100;

fn mvcc_spec() -> TableSpec {
    TableSpec::new(KeySpec::variable(KEY_SIZE), ValueSpec::variable(VALUE_SIZE))
        .partition_key_size(2)
        .mvcc()
    //.inline_gc()
}

fn make_key(id: u64) -> Vec<u8> {
    let mut k = vec![0u8; KEY_SIZE];
    k[0..8].copy_from_slice(&id.to_le_bytes());
    for i in 8..KEY_SIZE {
        k[i] = ((id.wrapping_mul(31).wrapping_add(i as u64)) % 256) as u8;
    }
    k
}

fn make_value(id: u64) -> Vec<u8> {
    let mut v = vec![0u8; VALUE_SIZE];
    v[0..8].copy_from_slice(&id.to_le_bytes());
    for i in 8..VALUE_SIZE {
        v[i] = ((id.wrapping_mul(37).wrapping_add(i as u64)) % 256) as u8;
    }
    v
}

// ─────────────────────────── Sync BackgroundTasks ─────────────────────────────

#[cfg(feature = "sync_frontend")]
mod sync_tasks {
    use std::thread::JoinHandle;

    pub struct BackgroundTasks {
        handles: Vec<JoinHandle<()>>,
    }

    pub struct ReactorTarget(pub usize);
    impl ReactorTarget {
        #[allow(non_snake_case)]
        pub fn Reactor(id: usize) -> Self { Self(id) }
    }

    impl BackgroundTasks {
        pub fn new() -> Self { Self { handles: Vec::new() } }
        pub fn spawn<F: FnOnce() + Send + 'static>(&mut self, _: ReactorTarget, f: F) {
            self.handles.push(std::thread::spawn(f));
        }
        pub fn join_all(self) {
            for h in self.handles {
                h.join().unwrap();
            }
        }
    }
}

cfg_if::cfg_if! {
    if #[cfg(feature = "sync_frontend")] {
        use sync_tasks::{BackgroundTasks, ReactorTarget};
    } else if #[cfg(feature = "async_frontend")] {
        use iomgr::{BackgroundTasks, ReactorTarget};
    }
}

// ─────────────────────────── Test implementations ─────────────────────────────

mod test_impls {
    use super::*;

    /// Saved on first violation; diagnostics run after all worker threads have exited.
    struct ViolationDetails {
        snap: Arc<Snapshot>,
        key: Vec<u8>,
        snap_ts: u64,
        range_val_id: u64,
        got_vid: Option<u64>,
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub(super) async fn test_snapshot_stress() {
        let db = MemoryDB::new(WRITER_WORKERS).unwrap();
        let index = db.create_table("stress", mvcc_spec()).await.unwrap().primary_index();

        // Preload
        for i in 0..PRELOAD_KEYS {
            index.put(make_key(i % KEY_RANGE), make_value(i % KEY_RANGE)).await.unwrap();
        }

        let min_key = vec![0u8; KEY_SIZE];
        let max_key = vec![0xffu8; KEY_SIZE];
        let violations: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let first_violation: Arc<AtomicBool> = Arc::new(AtomicBool::new(false));
        // Stores snap + key from the first violating reader; no other threads run during diagnostics.
        let diag: Arc<Mutex<Option<ViolationDetails>>> = Arc::new(Mutex::new(None));
        let duration = Duration::from_secs(STRESS_SECS);
        let start = Instant::now();

        cfg_if::cfg_if! {
            if #[cfg(feature = "sync_frontend")] { let mut bg = BackgroundTasks::new(); }
            else if #[cfg(feature = "async_frontend")] { let bg = BackgroundTasks::new(); }
        }

        // Writer workers
        for writer_id in 0..WRITER_WORKERS {
            let idx = Arc::clone(&index);
            let stop = Arc::clone(&first_violation);
            #[cfg(feature = "async_frontend")]
            bg.spawn(ReactorTarget::Reactor(writer_id), async move {
                let mut rng = StdRng::seed_from_u64(200 + writer_id as u64);
                while start.elapsed() < duration && !stop.load(Ordering::Relaxed) {
                    let key_id = rng.gen_range(0..KEY_RANGE);
                    if rng.gen_bool(0.7) {
                        let _ = idx.put(make_key(key_id), make_value(key_id)).await;
                    } else {
                        let _ = idx.remove(make_key(key_id)).await;
                    }
                }
            });
            #[cfg(feature = "sync_frontend")]
            bg.spawn(ReactorTarget::Reactor(writer_id), move || {
                let mut rng = StdRng::seed_from_u64(200 + writer_id as u64);
                while start.elapsed() < duration && !stop.load(Ordering::Relaxed) {
                    let key_id = rng.gen_range(0..KEY_RANGE);
                    if rng.gen_bool(0.7) {
                        let _ = idx.put(make_key(key_id), make_value(key_id));
                    } else {
                        let _ = idx.remove(make_key(key_id));
                    }
                }
            });
        }

        // Reader workers: snapshot + point-get consistency check.
        // On first violation: store snap+key in `diag`, set stop flag, break out of loop.
        // Diagnostics run AFTER bg.join_all() — zero other threads running, no output pollution.
        for reader_id in 0..READER_WORKERS {
            let idx = Arc::clone(&index);
            let viols = Arc::clone(&violations);
            let stop = Arc::clone(&first_violation);
            let diag = Arc::clone(&diag);
            let min = min_key.clone();
            let max = max_key.clone();
            let total_id = WRITER_WORKERS + reader_id;

            #[cfg(feature = "async_frontend")]
            bg.spawn(ReactorTarget::Reactor(total_id), async move {
                let mut iters = 0u64;
                'outer: while start.elapsed() < duration && !stop.load(Ordering::Relaxed) {
                    let snap = Arc::new(TableIndex::get_snapshot(Arc::clone(&idx)).unwrap());
                    let mut results = Vec::new();
                    let mut iter = snap.get_range(min.clone(), max.clone(), 256).await.unwrap();
                    while let Some(kv) = iter.next().await.unwrap() { results.push(kv); }
                    drop(iter);

                    if !results.is_empty() {
                        let n = VERIFY_KEYS_PER_SNAP.min(results.len());
                        let step = (results.len() / n).max(1);
                        for i in (0..results.len()).step_by(step).take(n) {
                            let (k, v) = &results[i];
                            match snap.get(k.clone()).await {
                                Ok(got) if got.as_ref() == Some(v) => {}
                                Ok(got) => {
                                    if stop.compare_exchange(false, true, Ordering::SeqCst, Ordering::Relaxed).is_ok() {
                                        let kid = key_id(k);
                                        let vid = val_id(v);
                                        let got_vid = got.as_ref().map(|g| val_id(g));
                                        eprintln!("=== VIOLATION: snap_ts={} key_id={:#018x} range_val_id={} point_get={:?} ===",
                                            snap.ts(), kid, vid, got_vid);
                                        eprintln!("    stopping all threads — diagnostics will follow");
                                        *diag.lock().unwrap() = Some(ViolationDetails {
                                            snap: Arc::clone(&snap),
                                            key: k.clone(),
                                            snap_ts: snap.ts(),
                                            range_val_id: vid,
                                            got_vid,
                                        });
                                        viols.lock().unwrap().push(format!(
                                            "snap_ts={} key_id={:#018x} range_val_id={} point_get={:?}",
                                            snap.ts(), kid, vid, got_vid
                                        ));
                                    }
                                    // Both the winner and any other reader that lost the race stop here.
                                    break 'outer;
                                }
                                Err(e) => viols.lock().unwrap().push(format!(
                                    "reader {} iter {}: point-get error: {}", reader_id, iters, e
                                )),
                            }
                        }
                    }
                    iters += 1;
                }
                println!("  reader {}: {} snapshot iterations", reader_id, iters);
            });

            #[cfg(feature = "sync_frontend")]
            bg.spawn(ReactorTarget::Reactor(total_id), move || {
                let mut iters = 0u64;
                'outer: while start.elapsed() < duration && !stop.load(Ordering::Relaxed) {
                    let snap = Arc::new(TableIndex::get_snapshot(Arc::clone(&idx)).unwrap());
                    let mut results = Vec::new();
                    let mut iter = snap.get_range(min.clone(), max.clone(), 256).unwrap();
                    while let Some(kv) = iter.next().unwrap() { results.push(kv); }
                    drop(iter);

                    if !results.is_empty() {
                        let n = VERIFY_KEYS_PER_SNAP.min(results.len());
                        let step = (results.len() / n).max(1);
                        for i in (0..results.len()).step_by(step).take(n) {
                            let (k, v) = &results[i];
                            match snap.get(k.clone()) {
                                Ok(got) if got.as_ref() == Some(v) => {}
                                Ok(got) => {
                                    if stop.compare_exchange(false, true, Ordering::SeqCst, Ordering::Relaxed).is_ok() {
                                        let kid = key_id(k);
                                        let vid = val_id(v);
                                        let got_vid = got.as_ref().map(|g| val_id(g));
                                        eprintln!("=== VIOLATION: snap_ts={} key_id={:#018x} range_val_id={} point_get={:?} ===",
                                            snap.ts(), kid, vid, got_vid);
                                        eprintln!("    stopping all threads — diagnostics will follow");
                                        *diag.lock().unwrap() = Some(ViolationDetails {
                                            snap: Arc::clone(&snap),
                                            key: k.clone(),
                                            snap_ts: snap.ts(),
                                            range_val_id: vid,
                                            got_vid,
                                        });
                                        viols.lock().unwrap().push(format!(
                                            "snap_ts={} key_id={:#018x} range_val_id={} point_get={:?}",
                                            snap.ts(), kid, vid, got_vid
                                        ));
                                    }
                                    break 'outer;
                                }
                                Err(e) => viols.lock().unwrap().push(format!(
                                    "reader {} iter {}: point-get error: {}", reader_id, iters, e
                                )),
                            }
                        }
                    }
                    iters += 1;
                }
                println!("  reader {}: {} snapshot iterations", reader_id, iters);
            });
        }

        bg.join_all().await;
        let elapsed = start.elapsed();

        // ── All worker threads have fully exited. Zero concurrent activity. ──────
        if let Some(info) = diag.lock().unwrap().take() {
            eprintln!(
                "=== DIAGNOSTICS (all threads stopped) snap_ts={} key_id={:#018x} range_val_id={} point_get={:?} ===",
                info.snap_ts, key_id(&info.key), info.range_val_id, info.got_vid
            );
            eprintln!("--- SNAPSHOT RANGE re-run (from violating key to max_key) ---");
            match info.snap.get_range(info.key.clone(), max_key.clone(), 4).await {
                Ok(mut it) => loop {
                    match it.next().await {
                        Ok(Some((k2, v2))) => eprintln!("  key_id={:#018x} val_id={}", key_id(&k2), val_id(&v2)),
                        Ok(None) => break,
                        Err(e) => { eprintln!("  iter error: {}", e); break; }
                    }
                },
                Err(e) => eprintln!("  get_range error: {}", e),
            }
            eprintln!("--- SNAPSHOT POINT GET re-run ---");
            match info.snap.get(info.key.clone()).await {
                Ok(got) => eprintln!("  got: {:?}", got.as_ref().map(|g| val_id(g))),
                Err(e) => eprintln!("  error: {}", e),
            }
            eprintln!("=== END DIAGNOSTICS ===");
        }

        let viols = violations.lock().unwrap();
        if viols.is_empty() {
            println!("=== SNAPSHOT STRESS PASSED in {:?} — zero violations ===", elapsed);
        } else {
            for v in viols.iter() { eprintln!("VIOLATION: {}", v); }
            panic!("SNAPSHOT STRESS FAILED: {} violations in {:?}", viols.len(), elapsed);
        }
    }
}

// ─────────────────────────── Test entry points ────────────────────────────────

macro_rules! generate_tests {
    ($($name:ident),+) => {
        $(
            #[cfg(feature = "sync_frontend")]
            #[test]
            fn $name() { test_impls::$name(); }

            #[cfg(feature = "async_frontend")]
            #[tokio::test]
            async fn $name() { test_impls::$name().await; }
        )+
    };
}

generate_tests!(test_snapshot_stress);
