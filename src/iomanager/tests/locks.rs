// Unit tests for AsyncMutex and AsyncRwLock under the tokio backend.
// Glommio tests are omitted due to executor bootstrap differences on macOS.

#![cfg(feature = "tokio")]

use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

use iomgr::{init_iomgr, AsyncMutex, AsyncRwLock};

// Helper to ensure IOManager initialized exactly once across tests.
fn ensure_iomanager() {
    let _ = init_iomgr(4); // ignore error if already initialized
}

#[tokio::test]
async fn async_mutex_mutual_exclusion() {
    ensure_iomanager();
    let mu = Arc::new(AsyncMutex::new(0u64));
    let iterations: u64 = 500;
    let tasks = 8u64;
    let mut handles = Vec::new();
    for _ in 0..tasks {
        // spawn tasks incrementing counter
        let mu_cl = mu.clone();
        handles.push(tokio::spawn(async move {
            for _ in 0..iterations {
                let mut guard = mu_cl.lock().await;
                *guard += 1;
            }
        }));
    }
    for h in handles {
        h.await.unwrap();
    }
    let final_val = *mu.lock().await;
    assert_eq!(final_val, iterations * tasks, "counter should equal total increments");
}

#[tokio::test]
async fn async_mutex_guard_drop_unlocks() {
    ensure_iomanager();
    let mu = AsyncMutex::new(10u32);
    {
        let mut g = mu.lock().await;
        *g = 42;
    } // drop guard -> unlock
    let g2 = mu.lock().await;
    assert_eq!(*g2, 42, "value persisted and lock released on guard drop");
}

#[tokio::test]
async fn async_rwlock_readers_can_share() {
    ensure_iomanager();
    let rw = Arc::new(AsyncRwLock::new(0u64));
    let concurrent_readers = Arc::new(AtomicU64::new(0));
    let saw_multi = Arc::new(AtomicU64::new(0));
    let mut handles = Vec::new();

    for _ in 0..6 {
        // spawn readers
        let rw_cl = rw.clone();
        let cr = concurrent_readers.clone();
        let saw = saw_multi.clone();
        handles.push(tokio::spawn(async move {
            let guard = rw_cl.read_lock().await;
            let prev = cr.fetch_add(1, Ordering::SeqCst) + 1;
            if prev >= 2 {
                saw.store(1, Ordering::SeqCst);
            }
            tokio::task::yield_now().await;
            cr.fetch_sub(1, Ordering::SeqCst);
            drop(guard);
        }));
    }
    for h in handles {
        h.await.unwrap();
    }

    assert_eq!(concurrent_readers.load(Ordering::SeqCst), 0, "all readers should have released");
    assert_eq!(saw_multi.load(Ordering::SeqCst), 1, "should observe at least two readers simultaneously");
}

#[tokio::test]
async fn async_rwlock_writer_is_exclusive() {
    ensure_iomanager();
    let rw = Arc::new(AsyncRwLock::new(0u32));

    // Acquire a write lock and hold it while spawning a reader that should block
    // until release.
    let rw_writer = rw.clone();
    let write_handle = tokio::spawn(async move {
        let mut wg = rw_writer.write_lock().await;
        *wg = 99;
        // Simulate holding the write lock with cooperative yields instead of sleeping
        // (tokio time feature not enabled).
        for _ in 0..200 {
            tokio::task::yield_now().await;
        }
        // drop wg -> unlock
    });

    // Reader attempt: should only see updated value after writer done.
    let rw_reader = rw.clone();
    let reader_handle = tokio::spawn(async move {
        // Briefly yield to let writer acquire lock first.
        for _ in 0..10 {
            tokio::task::yield_now().await;
        }
        let rg = rw_reader.read_lock().await;
        *rg // return observed value
    });

    let _ = write_handle.await.unwrap();
    let observed = reader_handle.await.unwrap();
    assert_eq!(observed, 99, "reader should observe writer's update after exclusive period");
}
