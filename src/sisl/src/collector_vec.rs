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

use std::{cell::UnsafeCell, sync::Arc};
use iomgr::iomgr;

/// Inner data structure holding the per-reactor Vecs.
struct CollectorVecInner<T>
where
    T: Clone + Send + 'static,
{
    /// Per-reactor Vecs, indexed by reactor_id.
    /// UnsafeCell allows interior mutability; safe because each reactor_id is
    /// accessed only by its corresponding reactor thread (single-threaded
    /// access per slot).
    reactor_vecs: Vec<UnsafeCell<Vec<T>>>,
}

// Safety: CollectorVecInner is Send because each reactor accesses only its own
// slot. The UnsafeCell is safe because of reactor thread isolation guarantees.
unsafe impl<T> Send for CollectorVecInner<T> where T: Clone + Send + 'static {}

// Safety: CollectorVecInner is Sync because reactors never access each other's
// slots.
unsafe impl<T> Sync for CollectorVecInner<T> where T: Clone + Send + 'static {}

/// A distributed Vec with per-reactor storage and global collection.
///
/// Each reactor maintains its own `Vec<T>` for fast, lock-free local
/// operations. The vecs are stored in a fixed-size array indexed by reactor_id,
/// allocated once at construction. The `collect()` method iterates through all
/// reactors, extracts their local vecs, and concatenates them to produce a
/// global snapshot.
///
/// # Type Parameters
///
/// - `T`: Element type (must implement `Clone + Send + 'static`)
///
/// # Thread Safety
///
/// - Local operations (`push`, `pop`, etc.) are lock-free within a single reactor.
/// - Each reactor operates on its own dedicated Vec (no contention).
/// - `collect()` spawns async tasks on all reactors and aggregates results; safe to call from any
///   thread.
/// - UnsafeCell is used for interior mutability; safety guaranteed by reactor thread isolation.
/// - CollectorVec is Clone (cheap Arc clone) and can be shared across async tasks.
#[derive(Clone)]
pub struct CollectorVec<T>
where
    T: Clone + Send + 'static,
{
    inner: Arc<CollectorVecInner<T>>,
}

impl<T> CollectorVec<T>
where
    T: Clone + Send + 'static,
{
    /// Create a new CollectorVec with storage for all reactors.
    ///
    /// The number of reactors is obtained directly from the IOManager.
    ///
    /// # Panics
    ///
    /// Panics if IOManager is not initialized.
    pub fn new() -> Self {
        let num_reactors = iomgr().num_reactors();
        let mut reactor_vecs = Vec::with_capacity(num_reactors);
        for _ in 0..num_reactors {
            reactor_vecs.push(UnsafeCell::new(Vec::new()));
        }
        Self { inner: Arc::new(CollectorVecInner { reactor_vecs }) }
    }

    /// Create a new CollectorVec with pre-allocated capacity for each reactor.
    ///
    /// # Arguments
    ///
    /// * `capacity` - The initial capacity for each reactor's Vec
    pub fn with_capacity(capacity: usize) -> Self {
        let num_reactors = iomgr().num_reactors();
        let mut reactor_vecs = Vec::with_capacity(num_reactors);
        for _ in 0..num_reactors {
            reactor_vecs.push(UnsafeCell::new(Vec::with_capacity(capacity)));
        }
        Self { inner: Arc::new(CollectorVecInner { reactor_vecs }) }
    }

    /// Get mutable reference to the current reactor's Vec.
    ///
    /// # Safety
    ///
    /// Safe because each reactor only accesses its own slot (indexed by
    /// reactor_id). IOManager guarantees that reactor threads are pinned
    /// and reactor_id is stable.
    #[inline]
    fn current_vec(&self) -> &mut Vec<T> {
        let rid = iomgr().current_reactor_id();
        unsafe { &mut *self.inner.reactor_vecs[rid].get() }
    }

    /// Push a value onto the current reactor's Vec.
    pub fn push(&self, value: T) { self.current_vec().push(value); }

    /// Pop a value from the current reactor's Vec.
    ///
    /// Returns `None` if the current reactor's vec is empty.
    pub fn pop(&self) -> Option<T> { self.current_vec().pop() }

    /// Get a reference to an element at the specified index in the current
    /// reactor's Vec.
    ///
    /// Returns `None` if the index is out of bounds.
    pub fn get(&self, index: usize) -> Option<&T> { self.current_vec().get(index) }

    /// Get a mutable reference to an element at the specified index in the
    /// current reactor's Vec.
    ///
    /// Returns `None` if the index is out of bounds.
    pub fn get_mut(&self, index: usize) -> Option<&mut T> { self.current_vec().get_mut(index) }

    /// Extend the current reactor's Vec with elements from an iterator.
    pub fn extend<I>(&self, iter: I)
    where
        I: IntoIterator<Item = T>,
    {
        self.current_vec().extend(iter);
    }

    /// Collect all reactor Vecs and return a concatenated global Vec.
    ///
    /// This method uses IOManager's `run_on_all_sequential` to execute
    /// collection on each reactor sequentially, extracting and resetting
    /// the reactor's Vec. The results are then concatenated into a single
    /// Vec.
    ///
    /// Reactor vecs are cleared after collection to avoid double-counting on
    /// subsequent collects.
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::collector_vec::CollectorVec;
    /// # use iomanager::iomanager;
    /// # async fn example() {
    /// let collector = CollectorVec::<u64>::new();
    /// // ... push from various reactors ...
    /// let global_vec = collector.collect().await;
    /// println!("Collected {} items", global_vec.len());
    /// # }
    /// ```
    pub async fn collect(&self) -> Vec<T> {
        let inner = self.inner.clone();
        let local_vecs = iomgr()
            .spawn_waitable_all(move |rid| {
                let inner = inner.clone();
                async move {
                    // Extract and reset the reactor's local vec
                    let vec_ptr = inner.reactor_vecs[rid].get();
                    unsafe {
                        let local_vec = std::mem::take(&mut *vec_ptr);
                        local_vec
                    }
                }
            })
            .await;

        Self::merge_vecs(local_vecs)
    }

    /// Merge a collection of Vecs by concatenating them.
    ///
    /// This is a utility method for merging multiple vecs into one.
    fn merge_vecs(vecs: Vec<Vec<T>>) -> Vec<T> {
        let total_len: usize = vecs.iter().map(|v| v.len()).sum();
        let mut global_vec = Vec::with_capacity(total_len);
        for mut local_vec in vecs {
            global_vec.append(&mut local_vec);
        }
        global_vec
    }

    /// Clear the current reactor's Vec.
    ///
    /// Useful for manual reset without collecting.
    pub fn clear(&self) { self.current_vec().clear(); }

    /// Get the number of elements in the current reactor's vec.
    pub fn len(&self) -> usize { self.current_vec().len() }

    /// Check if the current reactor's vec is empty.
    pub fn is_empty(&self) -> bool { self.current_vec().is_empty() }

    /// Get the capacity of the current reactor's vec.
    pub fn capacity(&self) -> usize { self.current_vec().capacity() }

    /// Reserve additional capacity in the current reactor's vec.
    pub fn reserve(&self, additional: usize) { self.current_vec().reserve(additional); }

    /// Shrink the capacity of the current reactor's vec to fit its length.
    pub fn shrink_to_fit(&self) { self.current_vec().shrink_to_fit(); }

    /// Get an iterator over the current reactor's vec elements.
    ///
    /// Note: This returns a temporary reference. Use with caution in async
    /// contexts.
    pub fn iter(&self) -> impl Iterator<Item = &T> {
        // This is safe because we're only iterating over the current reactor's vec
        self.current_vec().iter()
    }

    /// Get a mutable iterator over the current reactor's vec elements.
    pub fn iter_mut(&self) -> impl Iterator<Item = &mut T> { self.current_vec().iter_mut() }

    /// Retain only the elements specified by the predicate in the current
    /// reactor's vec.
    pub fn retain<F>(&self, f: F)
    where
        F: FnMut(&T) -> bool,
    {
        self.current_vec().retain(f);
    }

    /// Insert an element at position index within the current reactor's vec.
    ///
    /// # Panics
    ///
    /// Panics if index > len.
    pub fn insert(&self, index: usize, element: T) { self.current_vec().insert(index, element); }

    /// Remove and return the element at position index within the current
    /// reactor's vec.
    ///
    /// # Panics
    ///
    /// Panics if index is out of bounds.
    pub fn remove(&self, index: usize) -> T { self.current_vec().remove(index) }

    /// Get the number of reactors (array size).
    pub fn num_reactors(&self) -> usize { self.inner.reactor_vecs.len() }
}

impl<T> Default for CollectorVec<T>
where
    T: Clone + Send + 'static,
{
    fn default() -> Self { Self::new() }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Note: These tests require IOManager to be initialized.
    // Full integration tests should be in a separate test file with proper setup.

    #[test]
    fn test_basic_operations() {
        // This test would need IOManager initialized
        // Just testing the module compiles
    }
}
