//! Tests for iterator seek functionality

use mem_db::{MemoryDB, TableSpec, KeySpec, ValueSpec};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

static TABLE_COUNTER: AtomicUsize = AtomicUsize::new(0);

/// Helper to create test index with unique table name
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
async fn create_test_index() -> (MemoryDB, Arc<mem_db::TableIndex>) {
    let db = MemoryDB::new().unwrap();
    let spec = TableSpec::new(KeySpec::variable(1024), ValueSpec::variable(1024));
    let table_name = format!("test_table_{}", TABLE_COUNTER.fetch_add(1, Ordering::SeqCst));
    let table = db.create_table(&table_name, spec).await.unwrap();
    let index = table.primary_index();
    (db, index)
}

mod test_impls {
    use super::*;
    
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub(super) async fn test_seek_before_iteration() {
    let (_db, index) = create_test_index().await;

    // Insert keys 000-099
    for i in 0..100 {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range(b"key000".to_vec(), b"key099".to_vec(), 10).await.unwrap();

    // Seek to middle before reading anything
    assert!(iter.seek(b"key050").await.unwrap());

    let (k, v) = iter.next().await.unwrap().unwrap();
    assert_eq!(k, b"key050");
    assert_eq!(v, b"v50");
    
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
pub(super) async fn test_seek_during_iteration() {
    let (_db, index) = create_test_index().await;

    for i in 0..100 {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range(b"key000".to_vec(), b"key100".to_vec(), 10).await.unwrap();

    // Read first few keys
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key000");
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key001");
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key002");

    // Jump to key050
    assert!(iter.seek(b"key050").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key050");
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key051");

    // Jump back to key025
    assert!(iter.seek(b"key025").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key025");
    
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
pub(super) async fn test_seek_to_missing_key() {
    let (_db, index) = create_test_index().await;

    // Insert only even keys: 000, 002, 004, ...
    for i in (0..100).step_by(2) {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range(b"key000".to_vec(), b"key100".to_vec(), 10).await.unwrap();

    // Seek to odd key (doesn't exist)
    assert!(iter.seek(b"key025").await.unwrap());

    // Should position at next available key (key026)
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key026");
    
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
pub(super) async fn test_seek_beyond_end() {
    let (_db, index) = create_test_index().await;

    for i in 0..50 {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range(b"key000".to_vec(), b"key050".to_vec(), 10).await.unwrap();

    // Seek beyond range
    assert!(!iter.seek(b"key999").await.unwrap());

    // Next should return None
    assert!(iter.next().await.unwrap().is_none());
    
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
pub(super) async fn test_reverse_seek() {
    let (_db, index) = create_test_index().await;

    for i in 0..100 {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range_reverse(b"key000".to_vec(), b"key100".to_vec(), 10).await.unwrap();

    // Start at end
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key099");

    // Seek to key050
    assert!(iter.seek_for_prev(b"key050").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key050");
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key049");
    
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
pub(super) async fn test_multiple_seeks() {
    let (_db, index) = create_test_index().await;

    for i in 0..100 {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range(b"key000".to_vec(), b"key100".to_vec(), 10).await.unwrap();

    // Multiple seeks
    assert!(iter.seek(b"key010").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key010");

    assert!(iter.seek(b"key050").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key050");

    assert!(iter.seek(b"key075").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key075");

    assert!(iter.seek(b"key020").await.unwrap());
    assert_eq!(iter.next().await.unwrap().unwrap().0, b"key020");
    
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
pub(super) async fn test_seek_for_prev_on_forward_iterator_fails() {
    let (_db, index) = create_test_index().await;

    for i in 0..10 {
        let key = format!("key{:03}", i);
        let value = format!("v{}", i);
        index.put(key.as_bytes().to_vec(), value.as_bytes().to_vec()).await.unwrap();
    }

    let mut iter = index.get_range(b"key000".to_vec(), b"key010".to_vec(), 10).await.unwrap();

    // seek_for_prev should fail on forward iterator
    let result = iter.seek_for_prev(b"key005").await;
    assert!(result.is_err());
    assert!(result.unwrap_err().to_string().contains("only valid for reverse iterators"));
    
}
} // end test_impls module

// Macro to generate test wrappers for both sync and async modes
macro_rules! generate_tests {
    ($($test_fn:ident),* $(,)?) => {
        $(
            #[cfg(feature = "async_mode")]
            #[iomgr::iomanager_test]
            async fn $test_fn() {
                test_impls::$test_fn().await;
            }
            
            #[cfg(feature = "sync_mode")]
            #[test]
            fn $test_fn() {
                test_impls::$test_fn();
            }
        )*
    };
}

// Generate all test wrappers
generate_tests!(
    test_seek_before_iteration,
    test_seek_during_iteration,
    test_seek_to_missing_key,
    test_seek_beyond_end,
    test_reverse_seek,
    test_multiple_seeks,
    test_seek_for_prev_on_forward_iterator_fails,
);
