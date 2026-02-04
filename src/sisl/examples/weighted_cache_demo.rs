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

use std::sync::Arc;
use std::hash::Hash;

// Simplified version for demo
trait RefCounted: Clone + Send + Sync + 'static {}
impl<T: Send + Sync + 'static> RefCounted for Arc<T> {}

trait Weighted {
    fn weight(&self) -> u32;
}

struct SimpleCache<K, V>
where
    K: Hash + Eq + Clone + Send + Sync + 'static,
    V: RefCounted,
{
    cache: moka::sync::Cache<K, V>,
}

impl<K, V> SimpleCache<K, V>
where
    K: Hash + Eq + Clone + Send + Sync + 'static,
    V: RefCounted,
{
    fn new_weighted<F>(max_capacity: u64, weigher: F) -> Self
    where
        F: Fn(&K, &V) -> u32 + Send + Sync + 'static,
    {
        Self {
            cache: moka::sync::Cache::builder()
                .max_capacity(max_capacity)
                .weigher(weigher)
                .build(),
        }
    }
    
    fn get(&self, key: &K) -> Option<V> {
        self.cache.get(key)
    }
    
    fn insert(&self, key: K, value: V) {
        self.cache.insert(key, value);
    }
    
    fn contains_key(&self, key: &K) -> bool {
        self.cache.contains_key(key)
    }
    
    fn run_pending_tasks(&self) {
        self.cache.run_pending_tasks();
    }
}

fn main() {
    println!("=== Weighted Cache Demo ===\n");
    
    // Example 1: Cache with byte-level weighting
    println!("1. Byte-weighted cache (capacity = 10 bytes)");
    let cache = SimpleCache::<&str, Arc<Vec<u8>>>::new_weighted(
        10,
        |_key, value| value.len() as u32
    );
    
    cache.insert("small", Arc::new(vec![0u8; 3]));  // 3 bytes
    cache.insert("medium", Arc::new(vec![0u8; 5])); // 5 bytes
    println!("   Inserted 'small' (3 bytes) and 'medium' (5 bytes)");
    println!("   Total: 8 bytes (under capacity)");
    println!("   Both in cache: {}", 
        cache.contains_key(&"small") && cache.contains_key(&"medium"));
    
    cache.insert("large", Arc::new(vec![0u8; 4]));  // 4 bytes (exceeds capacity)
    cache.run_pending_tasks();
    
    println!("\n   Inserted 'large' (4 bytes) - exceeds capacity!");
    println!("   Cache evicted entries to stay under 10 bytes\n");
    
    // Example 2: Block-based weighting (512-byte blocks)
    println!("2. Block-weighted cache (capacity = 20 blocks = ~10KB)");
    let block_cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
        20,
        |_key, value| {
            // Round up to 512-byte blocks
            ((value.len() + 511) / 512) as u32
        }
    );
    
    // Insert various sized entries
    let entries = vec![
        (1, 512, "512 bytes = 1 block"),
        (2, 1024, "1KB = 2 blocks"),
        (3, 2048, "2KB = 4 blocks"),
        (4, 4096, "4KB = 8 blocks"),
    ];
    
    for (id, size, desc) in &entries {
        block_cache.insert(*id, Arc::new(vec![0u8; *size]));
        println!("   Inserted entry {}: {}", id, desc);
    }
    
    println!("\n   Total weight: 1+2+4+8 = 15 blocks (under 20)");
    println!("   All entries in cache: {}", 
        (1..=4).all(|i| block_cache.contains_key(&i)));
    
    // Insert large entry that exceeds capacity
    println!("\n   Inserting entry 5: 8KB = 16 blocks");
    block_cache.insert(5, Arc::new(vec![0u8; 8192]));
    block_cache.run_pending_tasks();
    
    println!("   Total would be 31 blocks (exceeds 20)");
    println!("   Cache evicted older entries to make room\n");
    
    // Example 3: Mixed weight entries with handles
    println!("3. Reference survival with weighted cache");
    let survival_cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
        100,
        |_key, value| value.len() as u32
    );
    
    survival_cache.insert(1, Arc::new(vec![1u8; 50]));
    
    println!("   Inserted entry 1 (50 bytes)");
    let handle = survival_cache.get(&1).unwrap();
    println!("   Got handle to entry 1");
    
    // Insert large entry to trigger eviction
    survival_cache.insert(2, Arc::new(vec![2u8; 80]));
    survival_cache.run_pending_tasks();
    
    println!("   Inserted entry 2 (80 bytes) - total 130 bytes, exceeds capacity");
    println!("   Entry 1 might be evicted from cache");
    
    // But handle still works!
    println!("   Handle still valid: {} bytes", handle.len());
    println!("   Data accessible: first byte = {}", handle[0]);
    
    println!("\n=== Demo complete ===");
    println!("\nKey features:");
    println!("• Different entries can have different weights");
    println!("• Capacity is sum of weights, not count of entries");
    println!("• Reference counting keeps evicted data alive");
    println!("• Useful for caching variable-sized data (blocks, buffers, nodes)");
}
