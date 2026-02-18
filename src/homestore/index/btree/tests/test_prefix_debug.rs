//! Btree Prefix Compress Node Debug Test

// This test uses iomgr::iomanager_test which requires Send.
// parking_lot locks are not Send, so this test is async-only.
#![cfg(feature = "async_code")]

use triomphe::Arc as TArc;
use crate::btree_node::{Node, NodeCore, LockType, InternalLockGuard, NodeOps, PREFIX_COMPRESS_NODE_OPS};
use crate::btree_kvs::ValueOrOverflow;

const NODE_SIZE: usize = 4096;

#[iomgr::iomanager_test]
async fn test_prefix_compress_debug() {
    let node_core = NodeCore::new(/*node_id=*/1, /*is_leaf=*/true, NODE_SIZE as u32);
    
    // Set the node variant to PREFIX_COMPRESS (4)
    {
        let header = node_core.get_persistent_header_mut();
        header.node_variant = 4;
    }
    
    // Initialize node with explicit type parameters
    <dyn NodeOps<u32, u64>>::init_new_node(&PREFIX_COMPRESS_NODE_OPS, &node_core);
    
    let node_core = TArc::new(node_core);
    let node = unsafe {
        let write_guard = node_core.lock.write().await;
        let write_guard = std::mem::transmute(write_guard);
        Node {
            core: node_core,
            lock_type: LockType::Write,
            _guard: InternalLockGuard::Write(write_guard),
        }
    };
    
    // Try to insert a single key
    let key: u32 = 1;
    let value: u64 = 1000;
    
    println!("Before insert: nentries = {}", node.total_entries());
    
    let val_ref = ValueOrOverflow::Inline(value);
    let result = node.insert::<u32, u64>(0, &key, &val_ref);
    println!("Insert result: {:?}", result);
    
    if let Err(e) = result {
        println!("Insert failed with error: {:?}", e);
        panic!("Insert failed");
    }
    
    println!("After insert: nentries = {}", node.total_entries());
    
    // Try to find it
    let (found, idx) = node.find::<u32, u64>(&key);
    println!("Find result: found={}, idx={}", found, idx);
    
    if !found {
        println!("Failed to find key that was just inserted!");
        
        // Get all kvs to see what's actually in the node
        let kvs = node.get_all_kvs::<u32, u64>();
        println!("All KVs in node:");
        for (k, val_ref) in &kvs {
            let v = val_ref.clone().expect_inline("Unexpected overflow in test");
            println!("  key={}, value={}", k, v);
        }
        
        panic!("Key not found after insert");
    }
    
    let read_value_ref = node.get_nth_value::<u32, u64>(idx, true);
    let read_value = read_value_ref.expect_inline("Unexpected overflow in test");
    println!("Retrieved value: {}", read_value);
    
    assert_eq!(read_value, value, "Value mismatch");
    println!("Test passed!");
}

