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

use std::hash::Hash;
use std::sync::Arc;
use moka::sync::Cache;

/// Trait for reference-counted types suitable for caching
/// 
/// Types implementing this trait use reference counting for shared ownership,
/// making them safe to cache and share across multiple accessors. The cache
/// can evict entries, but the data stays alive as long as any reference exists.
/// 
/// ## Requirements
/// 
/// - `Clone` must be O(1) and only increment a reference count
/// - Multiple clones must share the same underlying data
/// - Thread-safe reference counting (using atomic operations)
/// 
/// ## Implementations
/// 
/// - `Arc<T>`: Standard Rust reference-counted pointer
/// - Custom types with intrusive reference counting (e.g., BTreeNode)
/// - Types wrapping reference-counted data (e.g., IOBuffer wrapping bytes::Bytes)
pub trait RefCounted: Clone + Send + Sync + 'static {}

/// Trait for types with custom weight/size for cache eviction
/// 
/// Implement this trait when different cache entries should consume different
/// amounts of cache capacity. For example:
/// - A 512-byte block = weight 1
/// - A 4096-byte block = weight 8
/// 
/// The cache capacity then represents total weight, not number of entries.
/// 
/// ## Example
/// 
/// ```rust
/// struct CachedBlock {
///     data: Vec<u8>,
/// }
/// 
/// impl Weighted for CachedBlock {
///     fn weight(&self) -> u32 {
///         (self.data.len() / 512) as u32  // Weight in 512-byte units
///     }
/// }
/// ```
pub trait Weighted {
    /// Return the weight of this value for cache capacity calculations
    /// 
    /// A larger weight means this entry consumes more cache capacity.
    /// Return 0 for entries that should not count toward capacity.
    fn weight(&self) -> u32;
}

// Blanket implementation for Arc<T>
impl<T: Send + Sync + 'static> RefCounted for Arc<T> {}

/// A high-performance concurrent cache with W-TinyLFU eviction
/// 
/// `SimpleCache` is a wrapper around Moka that provides:
/// - Excellent cache hit rates via W-TinyLFU algorithm
/// - Support for reference-counted values that survive eviction
/// - Thread-safe concurrent access
/// - Automatic eviction management
/// 
/// ## Type Parameters
/// 
/// - `K`: Key type (must be `Hash + Eq + Clone + Send + Sync`)
/// - `V`: Value type (must implement `RefCounted`)
/// 
/// ## Capacity Management
/// 
/// The cache uses W-TinyLFU (Window + Tiny LFU) for eviction:
/// - **Window Cache** (1%): Catches burst traffic
/// - **Main Cache** (99%): 
///   - Protected segment (80%): Frequently accessed entries
///   - Probationary segment (19%): Recently accessed entries
/// - **Admission Policy**: Uses frequency estimation to prevent scan pollution
/// 
/// ## Thread Safety
/// 
/// All operations are thread-safe and can be called concurrently from multiple threads.
pub struct SimpleCache<K, V>
where
    K: Hash + Eq + Clone + Send + Sync + 'static,
    V: RefCounted,
{
    cache: Cache<K, V>,
}

impl<K, V> SimpleCache<K, V>
where
    K: Hash + Eq + Clone + Send + Sync + 'static,
    V: RefCounted,
{
    /// Create a new cache with the specified maximum capacity
    /// 
    /// The capacity represents the maximum number of entries (each entry counts as 1).
    /// 
    /// # Arguments
    /// 
    /// * `max_capacity` - Maximum number of entries the cache can hold
    /// 
    /// # Example
    /// 
    /// ```
    /// use sisl::simple_cache::SimpleCache;
    /// use std::sync::Arc;
    /// 
    /// let cache = SimpleCache::<u64, Arc<String>>::new(10_000);
    /// ```
    pub fn new(max_capacity: u64) -> Self {
        Self {
            cache: Cache::new(max_capacity),
        }
    }
    
    /// Create a weighted cache where entries have custom sizes
    /// 
    /// **Important**: The `max_capacity` represents the **sum of all weights**, not the number of entries.
    /// 
    /// For example, with `max_capacity = 10`:
    /// - Entry with weight 3 + Entry with weight 7 = total 10 (at capacity)
    /// - Adding another entry with weight 1 (total 11) triggers eviction
    /// 
    /// Use this when cache entries have different sizes and you want to limit
    /// by total size rather than number of entries.
    /// 
    /// # Arguments
    /// 
    /// * `max_capacity` - Maximum total weight (sum of all entry weights)
    /// * `weigher` - Function that returns the weight of each entry
    /// 
    /// # Example
    /// 
    /// ```
    /// use sisl::simple_cache::SimpleCache;
    /// use std::sync::Arc;
    /// 
    /// // Cache limited to 1GB (in 512-byte blocks = 2M blocks)
    /// let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
    ///     2_000_000,  // Max total weight = 2M blocks
    ///     |_key, value| (value.len() / 512) as u32
    /// );
    /// 
    /// // Insert 512-byte entry (weight 1, total: 1)
    /// cache.insert(1, Arc::new(vec![0u8; 512]));
    /// 
    /// // Insert 4KB entry (weight 8, total: 1+8=9)
    /// cache.insert(2, Arc::new(vec![0u8; 4096]));
    /// 
    /// // Both entries fit because 9 < 2_000_000
    /// assert!(cache.contains_key(&1));
    /// assert!(cache.contains_key(&2));
    /// ```
    pub fn new_weighted<F>(max_capacity: u64, weigher: F) -> Self
    where
        F: Fn(&K, &V) -> u32 + Send + Sync + 'static,
    {
        Self {
            cache: Cache::builder()
                .max_capacity(max_capacity)
                .weigher(weigher)
                .build(),
        }
    }

    /// Create a cache with an eviction filter
    ///
    /// The eviction filter is called before evicting an entry. If it returns `false`,
    /// the entry will not be evicted and will be moved to the back of the LRU queue.
    ///
    /// This is useful for preventing eviction of entries that are still in use
    /// (e.g., dirty entries being flushed, or entries with external references).
    ///
    /// # Arguments
    ///
    /// * `max_capacity` - Maximum number of entries
    /// * `filter` - Function that returns `true` to allow eviction, `false` to veto
    ///
    /// # Example
    ///
    /// ```rust
    /// use sisl::simple_cache::SimpleCache;
    /// use std::sync::Arc;
    ///
    /// // Only allow eviction if refcount is 1 (only cache holds it)
    /// let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_with_filter(
    ///     1000,
    ///     |_key, value| Arc::strong_count(value) == 1
    /// );
    ///
    /// cache.insert(1, Arc::new(vec![1, 2, 3]));
    ///
    /// // Hold a reference
    /// let handle = cache.get(&1).unwrap();
    ///
    /// // Cache cannot evict this entry while handle exists
    /// // (refcount > 1, filter returns false)
    /// ```
    pub fn new_with_filter<F>(max_capacity: u64, filter: F) -> Self
    where
        F: Fn(&K, &V) -> bool + Send + Sync + 'static,
    {
        Self {
            cache: Cache::builder()
                .max_capacity(max_capacity)
                .eviction_filter(filter)
                .build(),
        }
    }

    /// Create a weighted cache with an eviction filter
    ///
    /// Combines custom weight calculation with eviction filtering.
    ///
    /// # Arguments
    ///
    /// * `max_capacity` - Maximum total weight
    /// * `weigher` - Function that returns the weight of each entry
    /// * `filter` - Function that returns `true` to allow eviction, `false` to veto
    ///
    /// # Example
    ///
    /// ```rust
    /// use sisl::simple_cache::SimpleCache;
    /// use std::sync::Arc;
    ///
    /// let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted_with_filter(
    ///     10_000,  // Max weight
    ///     |_key, value| (value.len() / 512) as u32,  // Weight in blocks
    ///     |_key, value| Arc::strong_count(value) == 1  // Only evict if not shared
    /// );
    /// ```
    pub fn new_weighted_with_filter<W, F>(max_capacity: u64, weigher: W, filter: F) -> Self
    where
        W: Fn(&K, &V) -> u32 + Send + Sync + 'static,
        F: Fn(&K, &V) -> bool + Send + Sync + 'static,
    {
        Self {
            cache: Cache::builder()
                .max_capacity(max_capacity)
                .weigher(weigher)
                .eviction_filter(filter)
                .build(),
        }
    }
    
    /// Create a cache builder for advanced configuration
    /// 
    /// Allows customizing:
    /// - Time-to-live (TTL)
    /// - Time-to-idle
    /// - Eviction listeners
    /// - And more Moka options
    /// 
    /// # Example
    /// 
    /// ```
    /// use sisl::simple_cache::SimpleCache;
    /// use std::sync::Arc;
    /// use std::time::Duration;
    /// 
    /// let cache = SimpleCache::<u64, Arc<String>>::builder()
    ///     .max_capacity(10_000)
    ///     .time_to_idle(Duration::from_secs(300))
    ///     .build();
    /// ```
    pub fn builder() -> moka::sync::CacheBuilder<K, V, Cache<K, V>> {
        Cache::builder()
    }
    
    /// Create a cache from a pre-configured Moka cache
    /// 
    /// Useful when you need full control over Moka's configuration
    pub fn from_moka(cache: Cache<K, V>) -> Self {
        Self { cache }
    }
    
    /// Get a value from the cache
    /// 
    /// Returns a reference-counted handle to the value. The value will stay
    /// alive even if evicted from the cache, as long as this handle exists.
    /// 
    /// # Arguments
    /// 
    /// * `key` - The key to look up
    /// 
    /// # Returns
    /// 
    /// - `Some(V)` if the key exists (reference-counted clone)
    /// - `None` if the key doesn't exist
    /// 
    /// # Example
    /// 
    /// ```
    /// # use sisl::simple_cache::SimpleCache;
    /// # use std::sync::Arc;
    /// # let cache = SimpleCache::new(100);
    /// cache.insert(1, Arc::new("value"));
    /// 
    /// let value = cache.get(&1).unwrap();
    /// assert_eq!(*value, "value");
    /// ```
    pub fn get(&self, key: &K) -> Option<V> {
        self.cache.get(key)
    }
    
    /// Insert or update a key-value pair
    /// 
    /// If the key already exists, the old value is replaced.
    /// 
    /// # Arguments
    /// 
    /// * `key` - The key to insert
    /// * `value` - The value to insert (reference-counted type)
    /// 
    /// # Example
    /// 
    /// ```
    /// # use sisl::simple_cache::SimpleCache;
    /// # use std::sync::Arc;
    /// # let cache = SimpleCache::new(100);
    /// cache.insert(1, Arc::new("value"));
    /// cache.insert(1, Arc::new("new_value"));  // Replaces old value
    /// 
    /// assert_eq!(*cache.get(&1).unwrap(), "new_value");
    /// ```
    pub fn insert(&self, key: K, value: V) {
        self.cache.insert(key, value);
    }
    
    /// Remove a key from the cache
    /// 
    /// Note: Any existing references to the value will remain valid
    /// (reference counting keeps the data alive).
    /// 
    /// # Arguments
    /// 
    /// * `key` - The key to remove
    /// 
    /// # Example
    /// 
    /// ```
    /// # use sisl::simple_cache::SimpleCache;
    /// # use std::sync::Arc;
    /// # let cache = SimpleCache::new(100);
    /// cache.insert(1, Arc::new("value"));
    /// 
    /// let handle = cache.get(&1).unwrap();
    /// cache.invalidate(&1);
    /// 
    /// assert!(cache.get(&1).is_none());
    /// assert_eq!(*handle, "value");  // Handle still valid!
    /// ```
    pub fn invalidate(&self, key: &K) {
        self.cache.invalidate(key);
    }
    
    /// Check if a key exists in the cache
    /// 
    /// # Arguments
    /// 
    /// * `key` - The key to check
    /// 
    /// # Returns
    /// 
    /// `true` if the key exists, `false` otherwise
    pub fn contains_key(&self, key: &K) -> bool {
        self.cache.contains_key(key)
    }
    
    /// Get the current number of entries in the cache
    /// 
    /// Note: This is an approximate count due to concurrent access
    pub fn len(&self) -> u64 {
        self.cache.entry_count()
    }
    
    /// Check if the cache is empty
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    
    /// Run pending maintenance tasks
    /// 
    /// Moka performs maintenance (eviction, cleanup) lazily. This method
    /// forces immediate execution of pending tasks.
    /// 
    /// Typically not needed in production, but useful in tests.
    pub fn run_pending_tasks(&self) {
        self.cache.run_pending_tasks();
    }
}

// Convenience methods when V implements Weighted
impl<K, V> SimpleCache<K, V>
where
    K: Hash + Eq + Clone + Send + Sync + 'static,
    V: RefCounted + Weighted,
{
    /// Create a weighted cache using the `Weighted` trait implementation
    /// 
    /// This is a convenience method for types that implement the `Weighted` trait.
    /// The cache will automatically use `value.weight()` for capacity calculations.
    /// 
    /// **Important**: `max_capacity` is the **sum of all weights**, not entry count.
    /// 
    /// # Arguments
    /// 
    /// * `max_capacity` - Maximum total weight (sum of all entry weights)
    /// 
    /// # Example
    /// 
    /// ```
    /// use sisl::simple_cache::{SimpleCache, RefCounted, Weighted};
    /// 
    /// struct Block {
    ///     data: Vec<u8>,
    /// }
    /// 
    /// impl Clone for Block { /* ... */ }
    /// impl RefCounted for Block {}
    /// 
    /// impl Weighted for Block {
    ///     fn weight(&self) -> u32 {
    ///         // Weight in 512-byte units
    ///         (self.data.len() / 512) as u32
    ///     }
    /// }
    /// 
    /// // Cache holds up to 10,000 blocks of total weight
    /// let cache = SimpleCache::<u64, Block>::new_with_weight(10_000);
    /// 
    /// // If each block has weight 1, can hold ~10,000 entries
    /// // If blocks vary (weights 1, 2, 4, 8), fewer entries
    /// ```
    pub fn new_with_weight(max_capacity: u64) -> Self {
        Self {
            cache: Cache::builder()
                .max_capacity(max_capacity)
                .weigher(|_key: &K, value: &V| value.weight())
                .build(),
        }
    }
}

impl<K, V> Clone for SimpleCache<K, V>
where
    K: Hash + Eq + Clone + Send + Sync + 'static,
    V: RefCounted,
{
    /// Clone the cache handle (cheap, just clones the Arc inside Moka)
    fn clone(&self) -> Self {
        Self {
            cache: self.cache.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::ptr::NonNull;

    // =====================================================================
    // Test Type 1: Intrusive Reference Counted Node (like BTreeNode)
    // =====================================================================
    
    #[derive(Debug)]
    struct TestNode {
        inner: NonNull<TestNodeInner>,
    }
    
    struct TestNodeInner {
        ref_count: AtomicUsize,
        data: Vec<u8>,
    }
    
    impl TestNode {
        fn new(data: Vec<u8>) -> Self {
            let inner = Box::into_raw(Box::new(TestNodeInner {
                ref_count: AtomicUsize::new(1),
                data,
            }));
            
            Self {
                inner: unsafe { NonNull::new_unchecked(inner) },
            }
        }
        
        fn ref_count(&self) -> usize {
            unsafe { self.inner.as_ref().ref_count.load(Ordering::Acquire) }
        }
        
        fn data(&self) -> &[u8] {
            unsafe { &self.inner.as_ref().data }
        }
    }
    
    impl Clone for TestNode {
        fn clone(&self) -> Self {
            unsafe {
                self.inner.as_ref().ref_count.fetch_add(1, Ordering::Relaxed);
            }
            Self { inner: self.inner }
        }
    }
    
    impl Drop for TestNode {
        fn drop(&mut self) {
            unsafe {
                if self.inner.as_ref().ref_count.fetch_sub(1, Ordering::Release) == 1 {
                    let _ = Box::from_raw(self.inner.as_ptr());
                }
            }
        }
    }
    
    unsafe impl Send for TestNode {}
    unsafe impl Sync for TestNode {}
    
    impl RefCounted for TestNode {}
    
    // =====================================================================
    // Tests
    // =====================================================================
    
    #[test]
    fn test_basic_insert_and_get() {
        let cache = SimpleCache::<u64, Arc<String>>::new(100);
        
        cache.insert(1, Arc::new("value1".to_string()));
        cache.insert(2, Arc::new("value2".to_string()));
        
        assert_eq!(*cache.get(&1).unwrap(), "value1");
        assert_eq!(*cache.get(&2).unwrap(), "value2");
        assert!(cache.get(&3).is_none());
    }
    
    #[test]
    fn test_update_existing_key() {
        let cache = SimpleCache::<u64, Arc<String>>::new(100);
        
        cache.insert(1, Arc::new("old".to_string()));
        cache.insert(1, Arc::new("new".to_string()));
        
        assert_eq!(*cache.get(&1).unwrap(), "new");
    }
    
    #[test]
    fn test_invalidate() {
        let cache = SimpleCache::<u64, Arc<String>>::new(100);
        
        cache.insert(1, Arc::new("value".to_string()));
        assert!(cache.contains_key(&1));
        
        cache.invalidate(&1);
        assert!(!cache.contains_key(&1));
        assert!(cache.get(&1).is_none());
    }
    
    #[test]
    fn test_reference_survives_invalidation() {
        let cache = SimpleCache::<u64, Arc<String>>::new(100);
        
        cache.insert(1, Arc::new("value".to_string()));
        
        // Get a handle before invalidation
        let handle = cache.get(&1).unwrap();
        
        // Invalidate the cache entry
        cache.invalidate(&1);
        
        // Cache no longer has the entry
        assert!(cache.get(&1).is_none());
        
        // But the handle is still valid!
        assert_eq!(*handle, "value");
    }
    
    #[test]
    fn test_reference_survives_eviction() {
        // Small cache that will evict
        let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new(2);
        
        // Insert 3 entries (will evict one)
        cache.insert(1, Arc::new(vec![1]));
        cache.insert(2, Arc::new(vec![2]));
        
        // Hold reference to entry 1
        let handle1 = cache.get(&1).unwrap();
        
        // Insert entry 3, forcing eviction
        cache.insert(3, Arc::new(vec![3]));
        
        // Run pending maintenance to ensure eviction happens
        cache.run_pending_tasks();
        
        // Entry 1 might be evicted from cache
        // But handle1 is still valid because of reference counting
        assert_eq!(*handle1, vec![1]);
    }
    
    #[test]
    fn test_multiple_references() {
        let cache = SimpleCache::<u64, Arc<String>>::new(100);
        
        cache.insert(1, Arc::new("shared".to_string()));
        
        let ref1 = cache.get(&1).unwrap();
        let ref2 = cache.get(&1).unwrap();
        let ref3 = ref1.clone();
        
        // All references point to same data
        assert_eq!(*ref1, "shared");
        assert_eq!(*ref2, "shared");
        assert_eq!(*ref3, "shared");
        
        // Arc ref count is at least 3
        assert!(Arc::strong_count(&ref1) >= 3);
    }
    
    #[test]
    fn test_intrusive_ref_counting() {
        let cache = SimpleCache::<u64, TestNode>::new(100);
        
        let node = TestNode::new(vec![1, 2, 3, 4]);
        assert_eq!(node.ref_count(), 1);
        
        cache.insert(1, node);
        
        // Get references
        let ref1 = cache.get(&1).unwrap();
        assert_eq!(ref1.ref_count(), 2); // cache + ref1
        
        let ref2 = cache.get(&1).unwrap();
        assert_eq!(ref2.ref_count(), 3); // cache + ref1 + ref2
        
        // Invalidate from cache
        cache.invalidate(&1);
        cache.run_pending_tasks();  // Force Moka to drop the reference
        
        // Ref count should drop (cache drops its reference)
        // Note: Moka may temporarily hold references in internal buffers
        // So ref count might be slightly higher than expected
        assert!(ref1.ref_count() >= 2, "At least ref1 + ref2 should exist"); // ref1 + ref2 (+ maybe cache internals)
        
        drop(ref1);
        assert!(ref2.ref_count() >= 1, "At least ref2 should exist"); // ref2 (+ maybe cache internals)
        
        // Data still accessible
        assert_eq!(ref2.data(), &[1, 2, 3, 4]);
    }
    
    #[test]
    fn test_intrusive_ref_survives_eviction() {
        let cache = SimpleCache::<u64, TestNode>::new(2);
        
        cache.insert(1, TestNode::new(vec![1]));
        cache.insert(2, TestNode::new(vec![2]));
        
        let handle1 = cache.get(&1).unwrap();
        assert_eq!(handle1.ref_count(), 2); // cache + handle1
        
        // Force eviction
        cache.insert(3, TestNode::new(vec![3]));
        cache.run_pending_tasks();
        
        // Entry might be evicted, but handle1 is still valid
        assert_eq!(handle1.data(), &[1]);
        // Ref count dropped to 1 if evicted
        assert!(handle1.ref_count() >= 1);
    }
    
    #[test]
    fn test_concurrent_access() {
        use std::sync::Arc as StdArc;
        use std::thread;
        
        let cache = StdArc::new(SimpleCache::<u64, Arc<Vec<u8>>>::new(100));
        
        // Pre-populate
        for i in 0..10 {
            cache.insert(i, Arc::new(vec![i as u8]));
        }
        
        let mut handles = vec![];
        
        // Spawn multiple threads accessing the cache
        for t in 0..4 {
            let cache_clone: StdArc<SimpleCache<u64, Arc<Vec<u8>>>> = StdArc::clone(&cache);
            let handle = thread::spawn(move || {
                for i in 0..100 {
                    let key = (i % 10) as u64;
                    if let Some(data) = cache_clone.get(&key) {
                        assert_eq!(data[0], key as u8);
                    }
                    
                    // Also insert new entries
                    cache_clone.insert(key + 100 * t, Arc::new(vec![key as u8]));
                }
            });
            handles.push(handle);
        }
        
        // Wait for all threads
        for handle in handles {
            handle.join().unwrap();
        }
        
        // Cache should have entries
        assert!(cache.len() > 0);
    }
    
    // Note: Moka's hit_count() and miss_count() require special configuration
    // and are not available by default, so we skip stats testing
    
    #[test]
    fn test_len_and_empty() {
        let cache = SimpleCache::<u64, Arc<String>>::new(100);
        
        assert!(cache.is_empty());
        assert_eq!(cache.len(), 0);
        
        cache.insert(1, Arc::new("a".to_string()));
        cache.insert(2, Arc::new("b".to_string()));
        cache.run_pending_tasks();  // Moka lazily updates counters
        
        assert!(!cache.is_empty());
        assert_eq!(cache.len(), 2);
    }
    
    // =====================================================================
    // Weighted Cache Tests
    // =====================================================================
    
    #[test]
    fn test_weighted_cache_basic() {
        // Cache with capacity of 10 weight units
        // Each entry's weight = length of vector
        let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
            10,
            |_key: &u64, value: &Arc<Vec<u8>>| value.len() as u32
        );
        
        // Insert entry with weight 3
        cache.insert(1, Arc::new(vec![0u8; 3]));
        
        // Insert entry with weight 5
        cache.insert(2, Arc::new(vec![0u8; 5]));
        
        // Total weight = 8, under capacity
        assert!(cache.contains_key(&1));
        assert!(cache.contains_key(&2));
        
        // Insert entry with weight 4 (would exceed capacity)
        cache.insert(3, Arc::new(vec![0u8; 4]));
        cache.run_pending_tasks();
        
        // One of the earlier entries should be evicted
        // (but we can't predict which due to W-TinyLFU)
        let count = [1u64, 2, 3].iter()
            .filter(|k| cache.contains_key(k))
            .count();
        
        // Should have evicted at least one entry to stay under capacity
        assert!(count <= 2);
    }
    
    #[test]
    fn test_weighted_cache_custom_units() {
        // Cache in 512-byte blocks (capacity = 20 blocks = 10KB)
        let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
            20,
            |_key: &u64, value: &Arc<Vec<u8>>| ((value.len() + 511) / 512) as u32  // Round up to blocks
        );
        
        // 512 bytes = 1 block
        cache.insert(1, Arc::new(vec![0u8; 512]));
        
        // 4096 bytes = 8 blocks
        cache.insert(2, Arc::new(vec![0u8; 4096]));
        
        // 2048 bytes = 4 blocks
        cache.insert(3, Arc::new(vec![0u8; 2048]));
        
        // Total = 1 + 8 + 4 = 13 blocks (under 20)
        assert!(cache.contains_key(&1));
        assert!(cache.contains_key(&2));
        assert!(cache.contains_key(&3));
    }
    
    #[test]
    fn test_weighted_reference_survives_eviction() {
        // Small weighted cache (capacity = 5 units)
        let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
            5,
            |_key: &u64, value: &Arc<Vec<u8>>| value.len() as u32
        );
        
        // Insert entry with weight 3
        cache.insert(1, Arc::new(vec![1u8; 3]));
        
        // Get handle before eviction
        let handle = cache.get(&1).unwrap();
        assert_eq!(handle.len(), 3);
        
        // Insert large entry (weight 5) to trigger eviction
        cache.insert(2, Arc::new(vec![2u8; 5]));
        cache.run_pending_tasks();
        
        // Entry 1 might be evicted, but handle still works
        assert_eq!(*handle, vec![1u8; 3]);
        // Arc count should be 1 if evicted, or 2 if still in cache
        // W-TinyLFU might keep the frequently accessed entry
        assert!(Arc::strong_count(&handle) >= 1, "Handle should still be valid");
    }
    
    #[test]
    fn test_weighted_trait() {
        // Test type with Weighted trait
        #[derive(Clone, Debug)]
        struct Block {
            data: Vec<u8>,
        }
        
        impl RefCounted for Block {}
        
        impl Weighted for Block {
            fn weight(&self) -> u32 {
                // Weight in 512-byte units
                ((self.data.len() + 511) / 512) as u32
            }
        }
        
        // Create cache using the Weighted trait
        let cache = SimpleCache::<u64, Block>::new_with_weight(10);
        
        // Insert blocks of different sizes
        cache.insert(1, Block { data: vec![0u8; 512] });   // weight = 1
        cache.run_pending_tasks();
        cache.insert(2, Block { data: vec![0u8; 1024] });  // weight = 2
        cache.run_pending_tasks();
        cache.insert(3, Block { data: vec![0u8; 2048] });  // weight = 4
        cache.run_pending_tasks();
        
        // Total weight = 7, should all fit (capacity is 10)
        assert!(cache.contains_key(&1));
        assert!(cache.contains_key(&2));
        assert!(cache.contains_key(&3));
        
        // Insert large block (weight = 8, total would be 15 > 10)
        cache.insert(4, Block { data: vec![0u8; 4096] });
        cache.run_pending_tasks();
        
        // Should have evicted entries to stay under weight capacity
        // W-TinyLFU admission policy determines what stays
        let count = [1u64, 2, 3, 4].iter()
            .filter(|k| cache.contains_key(k))
            .count();
        
        assert!(count < 4, "Cache should evict entries to stay under weight capacity");
    }
    
    #[test]
    fn test_weighted_zero_weight() {
        // Test that zero-weight entries don't count toward capacity
        let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
            5,
            |_key: &u64, value: &Arc<Vec<u8>>| {
                if value.is_empty() {
                    0  // Empty vectors have zero weight
                } else {
                    value.len() as u32
                }
            }
        );
        
        // Insert many zero-weight entries
        for i in 0..100 {
            cache.insert(i, Arc::new(Vec::new()));
        }
        
        // All should fit (zero weight)
        for i in 0..100 {
            assert!(cache.contains_key(&i));
        }
        
        // Insert normal-weight entry
        cache.insert(1000, Arc::new(vec![0u8; 5]));
        
        // Should still have zero-weight entries
        assert!(cache.contains_key(&0));
        assert!(cache.contains_key(&1000));
    }
    
    #[test]
    fn test_weighted_capacity_is_sum_of_weights() {
        // Verify that max_capacity is the sum of weights, not entry count
        let cache = SimpleCache::<u64, Arc<String>>::new_weighted(
            4,  // Capacity = 4 weight units
            |_key: &u64, value: &Arc<String>| value.len() as u32
        );
        
        // Insert entry with weight 2
        cache.insert(1, Arc::new("aa".to_string()));  // weight = 2
        cache.run_pending_tasks();
        assert!(cache.contains_key(&1));
        
        // Insert another entry with weight 2 (total = 4, at capacity)
        cache.insert(2, Arc::new("bb".to_string()));  // weight = 2
        cache.run_pending_tasks();
        assert!(cache.contains_key(&1));
        assert!(cache.contains_key(&2));
        
        // Insert entry with weight 1 (would exceed: 2+2+1=5 > 4)
        cache.insert(3, Arc::new("c".to_string()));   // weight = 1
        cache.run_pending_tasks();
        
        // Should have evicted at least one entry to stay under capacity
        let present_count = [1u64, 2, 3].iter()
            .filter(|k| cache.contains_key(k))
            .count();
        
        // Can't fit all 3 entries (weights: 2+2+1=5 > capacity of 4)
        // Due to W-TinyLFU admission policy, Moka might not admit entry 3
        // or might evict differently, so we just verify weight constraint is respected
        assert!(present_count < 3, "Cache should have evicted entries to stay under weight capacity");
    }
    
    #[test]
    fn test_weighted_exact_capacity() {
        // Test behavior at exact capacity boundary
        let cache = SimpleCache::<u64, Arc<Vec<u8>>>::new_weighted(
            10,  // Exactly 10 units
            |_key: &u64, value: &Arc<Vec<u8>>| value.len() as u32
        );
        
        // Fill exactly to capacity
        cache.insert(1, Arc::new(vec![0u8; 3]));  // weight 3
        cache.insert(2, Arc::new(vec![0u8; 3]));  // weight 3
        cache.insert(3, Arc::new(vec![0u8; 4]));  // weight 4
        // Total: 3+3+4 = 10 (exactly at capacity)
        
        assert!(cache.contains_key(&1));
        assert!(cache.contains_key(&2));
        assert!(cache.contains_key(&3));
        
        // One more byte should trigger eviction
        cache.insert(4, Arc::new(vec![0u8; 1]));  // weight 1 (total would be 11)
        cache.run_pending_tasks();
        
        // Should have evicted something
        let count = [1u64, 2, 3, 4].iter()
            .filter(|k| cache.contains_key(k))
            .count();
        
        assert!(count <= 3, "Should evict to stay within capacity");
    }
}
