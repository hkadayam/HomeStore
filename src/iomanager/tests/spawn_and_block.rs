#![cfg(feature = "tokio")]

use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc,
};
use std::thread;

use iomgr::{init_iomgr, spawn_and_block, ReactorTarget};

#[test]
fn test_spawn_and_block_returns_result() {
    let _ = init_iomgr(4);
    
    // Run in separate thread to ensure it's not a reactor thread
    let handle = thread::spawn(|| {
        spawn_and_block(ReactorTarget::Any, async {
            42
        })
    });
    
    let result = handle.join().unwrap();
    assert_eq!(result, 42);
}

#[test]
fn test_spawn_and_block_specific_reactor() {
    let _ = init_iomgr(4);
    
    let handle = thread::spawn(|| {
        spawn_and_block(ReactorTarget::Reactor(2), async {
            "hello from reactor 2"
        })
    });
    
    let result = handle.join().unwrap();
    assert_eq!(result, "hello from reactor 2");
}

#[test]
fn test_spawn_and_block_multiple_calls() {
    let _ = init_iomgr(4);
    
    let handle = thread::spawn(|| {
        // Multiple calls from same thread - should reuse channel
        let r1 = spawn_and_block(ReactorTarget::Any, async { 1 });
        let r2 = spawn_and_block(ReactorTarget::Any, async { 2 });
        let r3 = spawn_and_block(ReactorTarget::Any, async { 3 });
        r1 + r2 + r3
    });
    
    let sum = handle.join().unwrap();
    assert_eq!(sum, 6);
}

#[test]
fn test_spawn_and_block_from_multiple_threads() {
    let _ = init_iomgr(4);
    
    let counter = Arc::new(AtomicUsize::new(0));
    let mut handles = vec![];
    
    // Spawn multiple threads, each calling spawn_and_block
    for i in 0..10 {
        let c = counter.clone();
        let handle = thread::spawn(move || {
            let result = spawn_and_block(ReactorTarget::Any, async move {
                i * 2
            });
            c.fetch_add(result, Ordering::Relaxed);
            result
        });
        handles.push(handle);
    }
    
    // Wait for all threads
    for handle in handles {
        let _ = handle.join();
    }
    
    // Sum: 0*2 + 1*2 + 2*2 + ... + 9*2 = 2*(0+1+2+...+9) = 2*45 = 90
    assert_eq!(counter.load(Ordering::Relaxed), 90);
}

// Note: Testing panic from reactor thread is complex without run_test
// The panic behavior is validated by the reactor thread detection logic

#[test]
fn test_spawn_and_block_panics_with_current() {
    let _ = init_iomgr(4);
    
    let handle = thread::spawn(|| {
        // ReactorTarget::Current is invalid from non-reactor thread
        spawn_and_block(ReactorTarget::Current, async { 42 })
    });
    
    let result = handle.join();
    assert!(result.is_err());  // Should panic
}

#[test]
fn test_spawn_and_block_panics_with_all() {
    let _ = init_iomgr(4);
    
    let handle = thread::spawn(|| {
        // ReactorTarget::All is not supported
        spawn_and_block(ReactorTarget::All, async { 42 })
    });
    
    let result = handle.join();
    assert!(result.is_err());  // Should panic
}

#[test]
fn test_spawn_and_block_with_complex_type() {
    let _ = init_iomgr(4);
    
    #[derive(Debug, PartialEq)]
    struct MyData {
        x: i32,
        y: String,
    }
    
    let handle = thread::spawn(|| {
        spawn_and_block(ReactorTarget::Any, async {
            MyData {
                x: 100,
                y: "test".to_string(),
            }
        })
    });
    
    let result = handle.join().unwrap();
    assert_eq!(result.x, 100);
    assert_eq!(result.y, "test");
}

#[test]
fn test_spawn_and_block_interleaved_with_async() {
    let _ = init_iomgr(4);
    
    // Test multiple blocking calls in sequence
    let handle = thread::spawn(|| {
        let r1 = spawn_and_block(ReactorTarget::Any, async { 10 });
        let r2 = spawn_and_block(ReactorTarget::Any, async { 20 });
        let r3 = spawn_and_block(ReactorTarget::Any, async { 30 });
        r1 + r2 + r3
    });
    
    let sum = handle.join().unwrap();
    assert_eq!(sum, 60);
}

