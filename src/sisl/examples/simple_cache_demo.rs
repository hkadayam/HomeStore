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

// Simplified version of SimpleCache for demo purposes
use std::hash::Hash;

trait RefCounted: Clone + Send + Sync + 'static {}
impl<T: Send + Sync + 'static> RefCounted for Arc<T> {}

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
    fn new(max_capacity: u64) -> Self {
        Self {
            cache: moka::sync::Cache::new(max_capacity),
        }
    }
    
    fn get(&self, key: &K) -> Option<V> {
        self.cache.get(key)
    }
    
    fn insert(&self, key: K, value: V) {
        self.cache.insert(key, value);
    }
    
    fn invalidate(&self, key: &K) {
        self.cache.invalidate(key);
    }
    
    fn run_pending_tasks(&self) {
        self.cache.run_pending_tasks();
    }
}

fn main() {
    println!("=== SimpleCache Demo ===\n");
    
    // Create a small cache
    let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new(2);
    
    // Insert some entries
    println!("1. Inserting entries...");
    cache.insert(1, Arc::new(vec![1, 1, 1]));
    cache.insert(2, Arc::new(vec![2, 2, 2]));
    println!("   Cache has 2 entries\n");
    
    // Get a handle to entry 1
    println!("2. Getting handle to entry 1...");
    let handle1 = cache.get(&1).unwrap();
    println!("   Handle acquired: Arc::strong_count = {}\n", Arc::strong_count(&handle1));
    
    // Insert entry 3, forcing eviction
    println!("3. Inserting entry 3 (cache is full, will evict)...");
    cache.insert(3, Arc::new(vec![3, 3, 3]));
    cache.run_pending_tasks();
    println!("   Entry 3 inserted\n");
    
    // Entry 1 might be evicted, but handle1 is still valid!
    println!("4. Checking handle1 after potential eviction...");
    println!("   handle1 data: {:?}", *handle1);
    println!("   Arc::strong_count = {}", Arc::strong_count(&handle1));
    println!("   ✓ Data is still accessible!\n");
    
    // Try to get entry 1 from cache
    println!("5. Trying to get entry 1 from cache...");
    match cache.get(&1) {
        Some(data) => println!("   ✓ Found in cache: {:?}", *data),
        None => println!("   ✗ Not in cache (evicted), but handle1 still works!"),
    }
    
    println!("\n=== Demo complete ===");
    println!("\nKey takeaway: Reference-counted values survive cache eviction.");
    println!("The cache can evict entries, but data stays alive while references exist.");
}
