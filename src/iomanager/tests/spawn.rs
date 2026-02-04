#![cfg(feature = "tokio")]
use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc,
};

use iomgr::{iomgr, spawn_detached, spawn_waitable, spawn_waitable_all, spawn, ReactorTarget};

#[iomgr::iomanager_test]
async fn test_spawn_detached_any() {
    let counter = Arc::new(AtomicUsize::new(0));
    
    for _ in 0..10 {
        let c = counter.clone();
        spawn_detached(ReactorTarget::Any, async move {
            c.fetch_add(1, Ordering::Relaxed);
        });
    }
    
    // Yield to let tasks run
    for _ in 0..50 {
        tokio::task::yield_now().await;
    }
    
    assert_eq!(counter.load(Ordering::Relaxed), 10);
}

#[iomgr::iomanager_test]
async fn test_spawn_detached_current() {
    let counter = Arc::new(AtomicUsize::new(0));
    let c = counter.clone();
    
    spawn_detached(ReactorTarget::Current, async move {
        c.fetch_add(1, Ordering::Relaxed);
    });
    
    // Yield to let task run
    for _ in 0..50 {
        tokio::task::yield_now().await;
    }
    
    assert_eq!(counter.load(Ordering::Relaxed), 1);
}

#[iomgr::iomanager_test]
async fn test_spawn_waitable_returns_result() {
    let result = spawn_waitable(ReactorTarget::Any, async {
        42
    }).await;
    
    assert_eq!(result, 42);
}

#[iomgr::iomanager_test]
async fn test_spawn_waitable_current_reactor() {
    let result = spawn_waitable(ReactorTarget::Current, async {
        "hello".to_string()
    }).await;
    
    assert_eq!(result, "hello");
}

#[iomgr::iomanager_test]
async fn test_spawn_returns_result() {
    let handle = spawn(async {
        100 + 23
    });
    
    let result = handle.await;
    assert_eq!(result, 123);
}

#[iomgr::iomanager_test]
async fn test_multiple_spawn_waitable() {
    let h1 = spawn_waitable(ReactorTarget::Any, async { 10 });
    let h2 = spawn_waitable(ReactorTarget::Any, async { 20 });
    let h3 = spawn_waitable(ReactorTarget::Any, async { 30 });
    
    let r1 = h1.await;
    let r2 = h2.await;
    let r3 = h3.await;
    
    assert_eq!(r1 + r2 + r3, 60);
}

#[iomgr::iomanager_test]
async fn test_joinhandle_reactor_id() {
    let handle = spawn(async { 42 });
    
    // Check reactor_id is valid
    let reactor_id = handle.reactor_id();
    assert!(reactor_id < iomgr().num_reactors());
    
    let result = handle.await;
    assert_eq!(result, 42);
}

#[iomgr::iomanager_test]
async fn test_joinhandle_is_finished() {
    let handle = spawn(async {
        // Quick task
        1 + 1
    });
    
    // Give it a moment to complete
    for _ in 0..10 {
        tokio::task::yield_now().await;
        if handle.is_finished() {
            break;
        }
    }
    
    // Should be finished by now
    assert!(handle.is_finished());
    
    let result = handle.await;
    assert_eq!(result, 2);
}

#[iomgr::iomanager_test]
async fn test_joinhandle_detach() {
    let counter = Arc::new(AtomicUsize::new(0));
    let c = counter.clone();
    
    let handle = spawn(async move {
        c.fetch_add(1, Ordering::Relaxed);
        42
    });
    
    // Detach without waiting
    handle.detach();
    
    // Yield to let task run
    for _ in 0..50 {
        tokio::task::yield_now().await;
    }
    
    // Task should have run
    assert_eq!(counter.load(Ordering::Relaxed), 1);
}

#[iomgr::iomanager_test]
async fn test_spawn_waitable_all() {
    let results = spawn_waitable_all(|reactor_id| async move {
        reactor_id * 2
    }).await;
    
    let num_reactors = iomgr().num_reactors();
    assert_eq!(results.len(), num_reactors);
    
    for (i, &result) in results.iter().enumerate() {
        assert_eq!(result, i * 2);
    }
}
