/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

//! Reverse Iteration / Traversal Query Tests
//!
//! Tests for reverse iteration support using traversal queries.
//! Critical for TiKV integration to avoid deadlocks.

use crate::index::btree::{
    btree::{Btree, UnderlyingBtree},
    BtreeConfig,  // Re-exported from btree_types at btree module level
};
use crate::index::btree::underlying::mem::MemBtree;
use crate::index::btree::detail::btree_req::BtreeKeyRange;

//================================================================================
// Test Implementation Functions (with maybe-async-cfg)
//================================================================================

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_reverse_traversal_query_basic_impl() {
    println!("=== Testing Reverse Traversal Query (Basic) ===");
    
    let mut config = BtreeConfig::new(4096, "test_reverse".to_string());
    config.leaf_node_variant = 4; // PREFIX_COMPRESS
    config.int_node_variant = 4;
    
    let storage: Box<dyn UnderlyingBtree> = Box::new(MemBtree::new(config.node_size));
    let btree = Btree::<u32, u64>::new(config, storage, None).await.expect("Failed to create btree");
    
    // Insert 100 entries
    println!("Inserting 100 entries...");
    for i in 0..100u32 {
        btree.put_one(&i, &((i as u64) * 1000), None).await
            .expect(&format!("Failed to insert key {}", i));
    }
    
    println!("Entries inserted. Testing queries...\n");
    
    // Test 1: Forward traversal query
    println!("Test 1: Forward traversal query (20-29)");
    let range = BtreeKeyRange::new(20, true, 29, true);
    let handle = btree.query_traversal(range, 100, None, false).await
        .expect("Forward query failed");
    
    println!("  Forward results: {} entries", handle.results.len());
    assert_eq!(handle.results.len(), 10, "Expected 10 forward results");
    for (i, (k, v)) in handle.results.iter().enumerate() {
        assert_eq!(*k, 20 + i as u32, "Forward key mismatch at index {}", i);
        assert_eq!(*v, (20 + i as u32) as u64 * 1000, "Forward value mismatch");
        println!("    [{}] key={}, value={}", i, k, v);
    }
    
    // Test 2: Reverse traversal query
    println!("\nTest 2: Reverse traversal query (29-20)");
    let range = BtreeKeyRange::new(20, true, 29, true);
    let handle = btree.query_traversal(range, 100, None, true).await
        .expect("Reverse query failed");
    
    println!("  Reverse results: {} entries", handle.results.len());
    assert_eq!(handle.results.len(), 10, "Expected 10 reverse results");
    for (i, (k, v)) in handle.results.iter().enumerate() {
        assert_eq!(*k, 29 - i as u32, "Reverse key mismatch at index {}", i);
        assert_eq!(*v, (29 - i as u32) as u64 * 1000, "Reverse value mismatch");
        println!("    [{}] key={}, value={}", i, k, v);
    }
    
    println!("\n✅ Basic reverse traversal tests passed!");
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_reverse_traversal_pagination_impl() {
    println!("=== Testing Reverse Traversal with Pagination ===");
    
    let mut config = BtreeConfig::new(4096, "test_reverse_page".to_string());
    config.leaf_node_variant = 4; // PREFIX_COMPRESS
    config.int_node_variant = 4;
    
    let storage: Box<dyn UnderlyingBtree> = Box::new(MemBtree::new(config.node_size));
    let btree = Btree::<u32, u64>::new(config, storage, None).await.expect("Failed to create btree");
    
    // Insert 100 entries
    println!("Inserting 100 entries...");
    for i in 0..100u32 {
        btree.put_one(&i, &((i as u64) * 1000), None).await
            .expect(&format!("Failed to insert key {}", i));
    }
    
    // Test 3: Forward with pagination
    println!("\nTest 3: Forward pagination (0-99, batch_size=20)");
    let range = BtreeKeyRange::new(0, true, 99, true);
    let mut handle = btree.query_traversal(range, 20, None, false).await
        .expect("Forward pagination query failed");
    
    let mut all_forward_keys = Vec::new();
    let mut batch_count = 0;
    loop {
        batch_count += 1;
        println!("  Batch {}: {} keys", batch_count, handle.results.len());
        for (k, _v) in &handle.results {
            all_forward_keys.push(*k);
        }
        if !handle.has_more() {
            break;
        }
        handle = btree.query_next_batch(handle).await.expect("Next batch failed");
    }
    println!("  Collected {} keys via forward pagination in {} batches", 
             all_forward_keys.len(), batch_count);
    assert_eq!(all_forward_keys.len(), 100, "Expected 100 keys from forward pagination");
    for i in 0..100 {
        assert_eq!(all_forward_keys[i], i as u32, "Forward pagination key mismatch at {}", i);
    }
    
    // Test 4: Reverse with pagination
    println!("\nTest 4: Reverse pagination (99-0, batch_size=20)");
    let range = BtreeKeyRange::new(0, true, 99, true);
    let mut handle = btree.query_traversal(range, 20, None, true).await
        .expect("Reverse pagination query failed");
    
    let mut all_reverse_keys = Vec::new();
    batch_count = 0;
    loop {
        batch_count += 1;
        println!("  Batch {}: {} keys", batch_count, handle.results.len());
        for (k, _v) in &handle.results {
            all_reverse_keys.push(*k);
        }
        if !handle.has_more() {
            break;
        }
        handle = btree.query_next_batch(handle).await.expect("Next batch failed");
    }
    println!("  Collected {} keys via reverse pagination in {} batches", 
             all_reverse_keys.len(), batch_count);
    assert_eq!(all_reverse_keys.len(), 100, "Expected 100 keys from reverse pagination");
    for i in 0..100 {
        assert_eq!(all_reverse_keys[i], (99 - i) as u32, "Reverse pagination key mismatch at {}", i);
    }
    
    println!("\n✅ All reverse pagination tests passed!");
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_reverse_traversal_multi_level_tree_impl() {
    println!("=== Testing Reverse Traversal on Multi-Level Tree ===");
    
    let mut config = BtreeConfig::new(4096, "test_reverse_multilevel".to_string());
    config.leaf_node_variant = 4; // PREFIX_COMPRESS
    config.int_node_variant = 4;
    
    let storage: Box<dyn UnderlyingBtree> = Box::new(MemBtree::new(config.node_size));
    let btree = Btree::<u32, u64>::new(config, storage, None).await.expect("Failed to create btree");
    
    // Insert 10,000 entries to force multi-level tree
    println!("Inserting 10,000 entries to create multi-level tree...");
    for i in 0..10000u32 {
        btree.put_one(&i, &((i as u64) * 1000), None).await
            .expect(&format!("Failed to insert key {}", i));
    }
    
    // Note: height() and count_entries() methods may not be publicly exposed
    // Just test that reverse query works on a large tree
    println!("Tree with 10,000 entries created");
    
    // Test reverse iteration on multi-level tree
    println!("\nTesting reverse query on range [5000, 5099]");
    let range = BtreeKeyRange::new(5000, true, 5099, true);
    let handle = btree.query_traversal(range, 100, None, true).await
        .expect("Reverse query failed");
    
    println!("  Reverse results: {} entries", handle.results.len());
    assert_eq!(handle.results.len(), 100, "Expected 100 reverse results");
    
    // Verify results are in reverse order
    for (i, (k, v)) in handle.results.iter().enumerate() {
        let expected_key = 5099 - i as u32;
        assert_eq!(*k, expected_key, "Reverse key mismatch at index {}", i);
        assert_eq!(*v, expected_key as u64 * 1000, "Reverse value mismatch");
    }
    
    println!("\n✅ Multi-level tree reverse traversal tests passed!");
}

//================================================================================
// Test Instantiation Macro
//================================================================================

macro_rules! instantiate_reverse_test {
    ($test_fn:ident) => {
        #[cfg(feature = "async_code")]
        #[iomgr::iomanager_test]
        async fn $test_fn() {
            paste::paste! {
                [<$test_fn _impl>]().await;
            }
        }

        #[cfg(feature = "sync_code")]
        #[test]
        fn $test_fn() {
            paste::paste! {
                [<$test_fn _impl>]();
            }
        }
    };
}

// Instantiate all tests
instantiate_reverse_test!(test_reverse_traversal_query_basic);
instantiate_reverse_test!(test_reverse_traversal_pagination);
instantiate_reverse_test!(test_reverse_traversal_multi_level_tree);
