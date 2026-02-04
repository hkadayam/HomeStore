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

use std::{cell::UnsafeCell, collections::HashMap, hash::Hash, sync::Arc};
use iomgr::iomgr;

/// Trait for values that can be merged during collection.
/// Implement this for any type you want to aggregate across reactors.
pub trait Mergeable: Clone {
    /// Merge `other` into `self`. Typically used for aggregation (sum, max,
    /// union, etc.).
    fn merge(&mut self, other: Self);
}

/// Inner data structure holding the per-reactor HashMaps.
struct CollectorHashMapInner<K, V>
where
    K: Eq + Hash + Clone + Send + 'static,
    V: Mergeable + Send + 'static,
{
    /// Per-reactor HashMaps, indexed by reactor_id.
    /// UnsafeCell allows interior mutability; safe because each reactor_id is
    /// accessed only by its corresponding reactor thread (single-threaded
    /// access per slot).
    reactor_maps: Vec<UnsafeCell<HashMap<K, V>>>,
}

// Safety: CollectorHashMapInner is Send because each reactor accesses only its
// own slot. The UnsafeCell is safe because of reactor thread isolation
// guarantees.
unsafe impl<K, V> Send for CollectorHashMapInner<K, V>
where
    K: Eq + Hash + Clone + Send + 'static,
    V: Mergeable + Send + 'static,
{
}

// Safety: CollectorHashMapInner is Sync because reactors never access each
// other's slots.
unsafe impl<K, V> Sync for CollectorHashMapInner<K, V>
where
    K: Eq + Hash + Clone + Send + 'static,
    V: Mergeable + Send + 'static,
{
}

/// A distributed HashMap with per-reactor storage and global collection.
///
/// Each reactor maintains its own `HashMap<K, V>` for fast, lock-free local
/// operations. The hashmaps are stored in a fixed-size array indexed by
/// reactor_id, allocated once at construction. The `collect()` method iterates
/// through all reactors, extracts their local maps, and merges values using
/// `V::merge()` to produce a global snapshot.
///
/// # Type Parameters
///
/// - `K`: Key type (must implement `Eq + Hash + Clone + Send + 'static`)
/// - `V`: Value type (must implement `Mergeable + Send + 'static`)
///
/// # Thread Safety
///
/// - Local operations (`insert`, `get`, `remove`) are lock-free within a single reactor.
/// - Each reactor operates on its own dedicated HashMap (no contention).
/// - `collect()` spawns async tasks on all reactors and aggregates results; safe to call from any
///   thread.
/// - UnsafeCell is used for interior mutability; safety guaranteed by reactor thread isolation.
/// - CollectorHashMap is Clone (cheap Arc clone) and can be shared across async tasks.
#[derive(Clone)]
pub struct CollectorHashMap<K, V>
where
    K: Eq + Hash + Clone + Send + 'static,
    V: Mergeable + Send + 'static,
{
    inner: Arc<CollectorHashMapInner<K, V>>,
}

impl<K, V> CollectorHashMap<K, V>
where
    K: Eq + Hash + Clone + Send + 'static,
    V: Mergeable + Send + 'static,
{
    /// Create a new CollectorHashMap with storage for all reactors.
    ///
    /// The number of reactors is obtained directly from the IOManager.
    ///
    /// # Panics
    ///
    /// Panics if IOManager is not initialized.
    pub fn new() -> Self {
        let num_reactors = iomgr().num_reactors();
        let mut reactor_maps = Vec::with_capacity(num_reactors);
        for _ in 0..num_reactors {
            reactor_maps.push(UnsafeCell::new(HashMap::new()));
        }
        Self { inner: Arc::new(CollectorHashMapInner { reactor_maps }) }
    }

    /// Get mutable reference to the current reactor's HashMap.
    ///
    /// # Safety
    ///
    /// Safe because each reactor only accesses its own slot (indexed by
    /// reactor_id). IOManager guarantees that reactor threads are pinned
    /// and reactor_id is stable.
    #[inline]
    fn current_map(&self) -> &mut HashMap<K, V> {
        let rid = iomgr().current_reactor_id();
        unsafe { &mut *self.inner.reactor_maps[rid].get() }
    }

    /// Insert a key-value pair into the current reactor's HashMap.
    ///
    /// If the key already exists, the value is replaced (no merge on insert).
    pub fn insert(&self, key: K, value: V) { self.current_map().insert(key, value); }

    /// Get a cloned value from the current reactor's HashMap.
    ///
    /// Returns `None` if the key is not present in this reactor's local map.
    pub fn get(&self, key: &K) -> Option<V> { self.current_map().get(key).cloned() }

    /// Remove a key from the current reactor's HashMap.
    ///
    /// Returns the removed value if it existed.
    pub fn remove(&self, key: &K) -> Option<V> { self.current_map().remove(key) }

    /// Update an existing key's value in the current reactor's HashMap by
    /// applying a closure.
    ///
    /// If the key exists, the closure is called with a mutable reference to the
    /// value. Returns `true` if the key was found and updated, `false`
    /// otherwise.
    pub fn update<F>(&self, key: &K, f: F) -> bool
    where
        F: FnOnce(&mut V),
    {
        if let Some(val) = self.current_map().get_mut(key) {
            f(val);
            true
        } else {
            false
        }
    }

    /// Collect all reactor HashMaps, merge values, and return a global
    /// snapshot.
    ///
    /// This method uses IOManager's `run_on_all_sequential` to execute
    /// collection on each reactor sequentially, extracting and resetting
    /// the reactor's HashMap. The results are then merged using
    /// `V::merge()`. The result is a single `HashMap<K, V>` representing
    /// the aggregated state across all reactors.
    ///
    /// Reactor maps are cleared after collection to avoid double-counting on
    /// subsequent collects.
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::collector_hashmap::{CollectorHashMap, Mergeable};
    /// # use iomanager::iomanager;
    /// # #[derive(Clone)] struct Counter(u64);
    /// # impl Mergeable for Counter { fn merge(&mut self, other: Self) { self.0 += other.0; } }
    /// # async fn example() {
    /// let collector = CollectorHashMap::<String, Counter>::new(iomgr().num_reactors);
    /// // ... insert from various reactors ...
    /// let global_map = collector.collect().await;
    /// for (key, val) in global_map {
    ///     println!("{}: {}", key, val.0);
    /// }
    /// # }
    /// ```
    pub async fn collect(&self) -> HashMap<K, V> {
        let inner = self.inner.clone();
        let local_maps = iomgr()
            .spawn_waitable_all(move |rid| {
                let inner = inner.clone();
                async move {
                    // Extract and reset the reactor's local map
                    let map_ptr = inner.reactor_maps[rid].get();
                    unsafe {
                        let local_map = std::mem::take(&mut *map_ptr);
                        local_map
                    }
                }
            })
            .await;

        Self::merge_maps(local_maps)
    }

    /// Merge a collection of HashMaps using the Mergeable trait.
    ///
    /// This is a utility method for merging multiple hashmaps into one.
    fn merge_maps(maps: Vec<HashMap<K, V>>) -> HashMap<K, V> {
        let mut global_map = HashMap::<K, V>::new();
        for local_map in maps {
            for (key, value) in local_map {
                global_map.entry(key).and_modify(|existing| existing.merge(value.clone())).or_insert(value);
            }
        }
        global_map
    }

    /// Clear the current reactor's HashMap.
    ///
    /// Useful for manual reset without collecting.
    pub fn clear(&self) { self.current_map().clear(); }

    /// Get the number of entries in the current reactor's map.
    pub fn len(&self) -> usize { self.current_map().len() }

    /// Check if the current reactor's map is empty.
    pub fn is_empty(&self) -> bool { self.current_map().is_empty() }

    /// Check if the current reactor's map contains a key.
    pub fn contains_key(&self, key: &K) -> bool { self.current_map().contains_key(key) }

    /// Get an iterator over the current reactor's map entries.
    ///
    /// Note: This returns a temporary reference. Use with caution in async
    /// contexts.
    pub fn iter(&self) -> impl Iterator<Item = (&K, &V)> {
        // This is safe because we're only iterating over the current reactor's map
        self.current_map().iter()
    }

    /// Get the number of reactors (array size).
    pub fn num_reactors(&self) -> usize { self.inner.reactor_maps.len() }
}

// Example Mergeable implementations for common types.

impl Mergeable for u64 {
    fn merge(&mut self, other: Self) { *self += other; }
}

impl Mergeable for i64 {
    fn merge(&mut self, other: Self) { *self += other; }
}

impl Mergeable for f64 {
    fn merge(&mut self, other: Self) { *self += other; }
}

impl Mergeable for String {
    fn merge(&mut self, other: Self) { self.push_str(&other); }
}

impl<T: Clone> Mergeable for Vec<T> {
    fn merge(&mut self, mut other: Self) { self.append(&mut other); }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, Debug, PartialEq)]
    struct Counter(u64);

    impl Mergeable for Counter {
        fn merge(&mut self, other: Self) { self.0 += other.0; }
    }

    // Note: These tests require IOManager to be initialized.
    // Full integration tests should be in a separate test file with proper setup.

    #[test]
    fn test_mergeable_primitives() {
        let mut a: u64 = 10;
        a.merge(20);
        assert_eq!(a, 30);

        let mut s = "Hello".to_string();
        s.merge(", World!".to_string());
        assert_eq!(s, "Hello, World!");

        let mut v = vec![1, 2, 3];
        v.merge(vec![4, 5]);
        assert_eq!(v, vec![1, 2, 3, 4, 5]);
    }

    #[test]
    fn test_counter_merge() {
        let mut c1 = Counter(10);
        c1.merge(Counter(20));
        assert_eq!(c1, Counter(30));
    }
}
