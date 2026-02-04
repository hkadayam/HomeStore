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

/// Inner data structure holding the per-reactor values.
struct ReactorLocalInner<T>
where
    T: Send + 'static,
{
    /// Per-reactor values, indexed by reactor_id.
    /// UnsafeCell allows interior mutability; safe because each reactor_id is
    /// accessed only by its corresponding reactor thread (single-threaded
    /// access per slot).
    reactor_values: Vec<UnsafeCell<T>>,
}

// Safety: ReactorLocalInner is Send because each reactor accesses only its own
// slot. The UnsafeCell is safe because of reactor thread isolation guarantees.
unsafe impl<T> Send for ReactorLocalInner<T> where T: Send + 'static {}

// Safety: ReactorLocalInner is Sync because reactors never access each other's
// slots.
unsafe impl<T> Sync for ReactorLocalInner<T> where T: Send + 'static {}

/// A per-reactor storage container for reactor-local data.
///
/// Each reactor maintains its own `T` for fast, lock-free local access.
/// The values are stored in a fixed-size array indexed by reactor_id, allocated
/// once at construction. Access to the current reactor's value is provided via
/// `get()` or `with()`.
///
/// # Type Parameters
///
/// - `T`: Value type (must implement `Send + 'static`)
///
/// # Thread Safety
///
/// - Local operations (`get`, `with`) are lock-free within a single reactor.
/// - Each reactor operates on its own dedicated slot (no contention).
/// - UnsafeCell is used for interior mutability; safety guaranteed by reactor thread isolation.
/// - ReactorLocal is Clone (cheap Arc clone) and can be shared across async tasks.
///
/// # Example
///
/// ```rust,no_run
/// use sisl::reactor_local::ReactorLocal;
///
/// // Create with an initializer function
/// let counter = ReactorLocal::new(|| 0u64);
///
/// // Access and modify the current reactor's value
/// *counter.get() += 1;
///
/// // Or use a closure
/// counter.with(|val| {
///     *val += 10;
///     println!("Counter: {}", val);
/// });
/// ```
#[derive(Clone)]
pub struct ReactorLocal<T>
where
    T: Send + 'static,
{
    inner: Arc<ReactorLocalInner<T>>,
}

impl<T> ReactorLocal<T>
where
    T: Send + 'static,
{
    /// Create a new ReactorLocal with an initializer function.
    ///
    /// The initializer is called once per reactor to create the initial value.
    /// The number of reactors is obtained directly from the IOManager.
    ///
    /// # Arguments
    ///
    /// * `init` - A function that returns the initial value for each reactor
    ///
    /// # Panics
    ///
    /// Panics if IOManager is not initialized.
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::reactor_local::ReactorLocal;
    /// let counter = ReactorLocal::new(|| 0u64);
    /// let vec = ReactorLocal::new(|| Vec::<String>::new());
    /// ```
    pub fn new<F>(init: F) -> Self
    where
        F: Fn() -> T,
    {
        let num_reactors = iomgr().num_reactors();
        let mut reactor_values = Vec::with_capacity(num_reactors);
        for _ in 0..num_reactors {
            reactor_values.push(UnsafeCell::new(init()));
        }
        Self { inner: Arc::new(ReactorLocalInner { reactor_values }) }
    }

    /// Get a mutable reference to the current reactor's value.
    ///
    /// # Safety
    ///
    /// Safe because each reactor only accesses its own slot (indexed by
    /// reactor_id). IOManager guarantees that reactor threads are pinned
    /// and reactor_id is stable.
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::reactor_local::ReactorLocal;
    /// # let counter = ReactorLocal::new(|| 0u64);
    /// let value = counter.get();
    /// *value += 1;
    /// println!("Value: {}", *value);
    /// ```
    #[inline]
    pub fn get(&self) -> &mut T {
        let rid = iomgr().current_reactor_id();
        unsafe { &mut *self.inner.reactor_values[rid].get() }
    }

    /// Access the current reactor's value via a closure.
    ///
    /// This is a convenience method for operations that don't need to keep the
    /// reference.
    ///
    /// # Arguments
    ///
    /// * `f` - A closure that receives a mutable reference to the current reactor's value
    ///
    /// # Returns
    ///
    /// The return value of the closure.
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::reactor_local::ReactorLocal;
    /// # let counter = ReactorLocal::new(|| 0u64);
    /// counter.with(|val| {
    ///     *val += 1;
    /// });
    ///
    /// let result = counter.with(|val| *val * 2);
    /// ```
    pub fn with<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&mut T) -> R,
    {
        f(self.get())
    }

    /// Get a mutable reference to a specific reactor's value.
    ///
    /// # Safety
    ///
    /// This is unsafe because it allows access to another reactor's data.
    /// The caller must ensure that the target reactor is not currently
    /// accessing its value. This is primarily intended for use in
    /// collection/aggregation scenarios where coordination is externally
    /// guaranteed (e.g., via run_on_all_sequential).
    ///
    /// # Arguments
    ///
    /// * `reactor_id` - The reactor ID to access
    ///
    /// # Panics
    ///
    /// Panics if reactor_id >= num_reactors.
    #[inline]
    pub unsafe fn get_for_reactor(&self, reactor_id: usize) -> &mut T {
        &mut *self.inner.reactor_values[reactor_id].get()
    }

    /// Get the number of reactors (array size).
    pub fn num_reactors(&self) -> usize { self.inner.reactor_values.len() }

    /// Collect all reactor values into a Vec.
    ///
    /// This method uses IOManager's `run_on_all_sequential` to execute
    /// collection on each reactor sequentially, cloning each reactor's
    /// value. The original values remain unchanged.
    ///
    /// This requires `T: Clone`.
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::reactor_local::ReactorLocal;
    /// # async fn example() {
    /// let counter = ReactorLocal::new(|| 0u64);
    /// // ... modify from various reactors ...
    /// let all_values = counter.collect().await;
    /// let sum: u64 = all_values.iter().sum();
    /// # }
    /// ```
    pub async fn collect(&self) -> Vec<T>
    where
        T: Clone,
    {
        let inner = self.inner.clone();
        iomgr()
            .spawn_waitable_all(move |rid| {
                let inner = inner.clone();
                async move {
                    let value_ptr = inner.reactor_values[rid].get();
                    unsafe { (*value_ptr).clone() }
                }
            })
            .await
    }

    /// Collect and aggregate all reactor values using a custom aggregation
    /// function.
    ///
    /// This method collects values from all reactors and reduces them into a
    /// single value using the provided aggregation function.
    ///
    /// # Arguments
    ///
    /// * `init` - Initial value for the accumulator
    /// * `f` - Aggregation function that takes (accumulator, reactor_value) and returns new
    ///   accumulator
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::reactor_local::ReactorLocal;
    /// # async fn example() {
    /// let counter = ReactorLocal::new(|| 0u64);
    /// // ... increment from various reactors ...
    /// let total = counter.collect_aggregate(0u64, |acc, val| acc + *val).await;
    /// # }
    /// ```
    pub async fn collect_aggregate<F, R>(&self, init: R, f: F) -> R
    where
        T: Clone,
        F: Fn(R, &T) -> R,
        R: Send + 'static,
    {
        let values = self.collect().await;
        values.iter().fold(init, |acc, val| f(acc, val))
    }

    /// Reset all reactor values using the provided initializer.
    ///
    /// This uses `run_on_all_sequential` to reset each reactor's value on its
    /// own thread.
    ///
    /// # Arguments
    ///
    /// * `init` - A function that returns the new value for each reactor
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use sisl::reactor_local::ReactorLocal;
    /// # async fn example() {
    /// let counter = ReactorLocal::new(|| 0u64);
    /// // ... use counter ...
    /// counter.reset(|| 0u64).await;
    /// # }
    /// ```
    pub async fn reset<F>(&self, init: F)
    where
        F: Fn() -> T + Send + Sync + 'static,
    {
        let inner = self.inner.clone();
        let init = Arc::new(init);
        iomgr()
            .spawn_waitable_all(move |rid| {
                let inner = inner.clone();
                let init = init.clone();
                async move {
                    let value_ptr = inner.reactor_values[rid].get();
                    unsafe {
                        *value_ptr = init();
                    }
                }
            })
            .await;
    }
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
