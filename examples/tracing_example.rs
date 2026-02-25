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

//! Tracing Example for Homestore Btree
//!
//! This example demonstrates how to use tracing with the btree module.
//!
//! Run with different log levels:
//! ```bash
//! # Info level (minimal output)
//! RUST_LOG=homestore::index::btree=info cargo run --example tracing_example
//!
//! # Debug level (detailed output)
//! RUST_LOG=homestore::index::btree=debug cargo run --example tracing_example
//!
//! # Trace level (very detailed)
//! RUST_LOG=homestore::index::btree=trace cargo run --example tracing_example
//!
//! # Specific operations only
//! RUST_LOG=homestore::index::btree[remove_one]=trace cargo run --example tracing_example
//! ```

use homestore::index::btree::{
    Btree, BtreeConfig,
    tests::btree_test_kvs::{FixedSizeTestKey, FixedSizeTestValue},
    underlying::mem::MemBtree,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize tracing - this sets up the logging infrastructure
    homestore::init_tracing();
    
    println!("=== Homestore Tracing Example ===\n");
    println!("Watch the logs below to see tracing in action!");
    println!("Each operation has a sequential op_id for tracking.\n");
    
    // Create a simple in-memory btree
    let config = BtreeConfig::new(4096, "example_btree".to_string());
    let storage = Box::new(MemBtree::new(&config));
    let btree: Btree<FixedSizeTestKey, FixedSizeTestValue> = 
        Btree::new(config, storage, None).await?;
    
    println!("--- Inserting keys (watch for op_id=1, 2, 3) ---");
    
    // Insert some keys - each will get a sequential op_id
    btree.put_one(&FixedSizeTestKey::from_u64(10), &FixedSizeTestValue::from_u64(100), None).await?;
    btree.put_one(&FixedSizeTestKey::from_u64(20), &FixedSizeTestValue::from_u64(200), None).await?;
    btree.put_one(&FixedSizeTestKey::from_u64(30), &FixedSizeTestValue::from_u64(300), None).await?;
    
    println!("\n--- Looking up keys (watch for op_id=4, 5) ---");
    
    // Get some keys
    if let Some(value) = btree.get(&FixedSizeTestKey::from_u64(20)).await? {
        println!("Found value: {:?}", value);
    }
    
    if let Some(value) = btree.get(&FixedSizeTestKey::from_u64(99)).await? {
        println!("Found value: {:?}", value);
    } else {
        println!("Key 99 not found (expected)");
    }
    
    println!("\n--- Removing a key (watch for op_id=6) ---");
    
    // Remove a key
    if let Some(value) = btree.remove_one(&FixedSizeTestKey::from_u64(20)).await? {
        println!("Removed value: {:?}", value);
    }
    
    println!("\n--- Verifying removal (watch for op_id=7) ---");
    
    // Verify it's gone
    if btree.get(&FixedSizeTestKey::from_u64(20)).await?.is_none() {
        println!("Key 20 successfully removed");
    }
    
    println!("\n=== Example Complete ===");
    println!("\nKey observations in the logs:");
    println!("1. Each operation has a unique, sequential op_id");
    println!("2. Keys are shown in concise format: K(10), K(20), etc.");
    println!("3. Timestamps show exact timing of each operation");
    println!("4. Span context (put_one, get, remove_one) groups related logs");
    println!("\nTry running with different RUST_LOG levels to see more/less detail!");
    
    Ok(())
}
