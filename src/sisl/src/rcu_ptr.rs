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

use left_right::{self, Absorb, ReadHandle, WriteHandle};

/// RcuPtr - A Read-Copy-Update pointer similar to C++ urcu_scoped_ptr
///
/// Provides lock-free reads with atomic updates. Readers always see
/// a consistent snapshot without blocking writers.
///
/// # Example
/// ```
/// use sisl::RcuPtr;
///
/// #[derive(Clone, PartialEq, Debug, Default)]
/// enum State { #[default] Init, Active, Done }
///
/// let mut rcu = RcuPtr::new(State::Init);
///
/// // Lock-free read with guard (limited scope to release borrow)
/// {
///     let guard = rcu.read();
///     assert_eq!(*guard, State::Init);
/// }
///
/// // Or read with closure
/// rcu.read_and_then(|state| {
///     assert_eq!(*state, State::Init);
/// });
///
/// // Update (creates new copy)
/// rcu.update(State::Active);
///
/// // Readers see new value
/// {
///     let guard = rcu.read();
///     assert_eq!(*guard, State::Active);
/// }
/// ```
/// Wrapper for the value to implement Absorb trait
/// This is necessary because we can't implement foreign traits on generic types
#[derive(Clone)]
struct RcuValue<T: Clone>(T);

impl<T: Clone + Default> Default for RcuValue<T> {
    fn default() -> Self { RcuValue(T::default()) }
}

impl<T: Clone> Absorb<T> for RcuValue<T> {
    fn absorb_first(&mut self, operation: &mut T, _other: &Self) { self.0 = operation.clone(); }

    fn sync_with(&mut self, first: &Self) { self.0 = first.0.clone(); }

    fn drop_first(self: Box<Self>) {}
}

pub struct RcuPtr<T>
where
    T: Clone,
{
    reader: ReadHandle<RcuValue<T>>,
    writer: std::cell::UnsafeCell<WriteHandle<RcuValue<T>, T>>,
}

/// Guard that provides access to the RCU-protected value
///
/// This wraps the internal ReadGuard and provides clean access to T.
/// Similar to C++ urcu_scoped_ptr's dereferencing behavior.
pub struct RcuGuard<'a, T: Clone> {
    guard: left_right::ReadGuard<'a, RcuValue<T>>,
}

impl<'a, T: Clone> std::ops::Deref for RcuGuard<'a, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target { &self.guard.0 }
}

// Safety: RcuPtr is safe to send between threads since:
// - ReadHandle allows concurrent lock-free reads
// - WriteHandle is only accessed mutably (one writer at a time)
unsafe impl<T: Clone + Send> Send for RcuPtr<T> {}
unsafe impl<T: Clone + Send + Sync> Sync for RcuPtr<T> {}

impl<T> RcuPtr<T>
where
    T: Clone + Default,
{
    /// Create a new RcuPtr with initial value
    pub fn new(initial: T) -> Self {
        let (writer, reader) = left_right::new::<RcuValue<T>, T>();
        let mut rcu = Self { reader, writer: std::cell::UnsafeCell::new(writer) };
        rcu.update(initial);
        rcu
    }

    /// Read the current value with a closure
    ///
    /// The closure receives a reference to the current value.
    /// This is wait-free and never blocks.
    pub fn read_and_then<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&T) -> R,
    {
        self.reader.enter().map(|guard| f(&guard.0)).unwrap()
    }

    /// Get a guard to the current value (similar to C++ urcu_scoped_ptr)
    ///
    /// Returns a guard that provides access to the current snapshot.
    /// The guard must be dereferenced to access the value.
    ///
    /// # Example
    /// ```
    /// use sisl::RcuPtr;
    ///
    /// let rcu = RcuPtr::new(42);
    /// let guard = rcu.read();
    /// assert_eq!(*guard, 42);  // Dereference to get the value
    /// ```
    pub fn read(&self) -> RcuGuard<'_, T> { RcuGuard { guard: self.reader.enter().unwrap() } }

    /// Update the value atomically
    ///
    /// This passes T directly as the operation parameter to publish()
    /// The Absorb trait handles replacing the value
    ///
    /// # Safety
    /// Uses interior mutability via UnsafeCell. The caller must ensure that
    /// only one thread calls update() at a time for correct behavior.
    pub fn update(&self, new_value: T) {
        unsafe {
            let writer = &mut *self.writer.get();
            writer.append(new_value);
            writer.publish();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, PartialEq, Debug, Default)]
    enum TestState {
        #[default]
        Init,
        Running,
        Done,
    }

    #[test]
    fn test_basic_read_write() {
        let mut rcu = RcuPtr::new(TestState::Init);

        rcu.read_and_then(|state| {
            assert_eq!(*state, TestState::Init);
        });

        rcu.update(TestState::Running);

        rcu.read_and_then(|state| {
            assert_eq!(*state, TestState::Running);
        });
    }

    #[test]
    fn test_multiple_updates() {
        #[derive(Clone, PartialEq, Debug, Default)]
        struct Counter(u32);

        let mut rcu = RcuPtr::new(Counter(0));

        rcu.update(Counter(1));
        rcu.read_and_then(|counter| {
            assert_eq!(counter.0, 1);
        });

        rcu.update(Counter(2));
        rcu.read_and_then(|counter| {
            assert_eq!(counter.0, 2);
        });
    }

    #[test]
    fn test_concurrent_reads() {
        use std::{sync::Arc, thread};

        #[derive(Clone, PartialEq, Debug, Default)]
        struct Value(i32);

        let rcu = Arc::new(RcuPtr::new(Value(42)));
        let mut handles = vec![];

        // Spawn multiple readers
        for _ in 0..10 {
            let rcu_clone = Arc::clone(&rcu);
            handles.push(thread::spawn(move || {
                rcu_clone.read_and_then(|value| {
                    // Should read 42 since we're not updating
                    assert_eq!(value.0, 42);
                });
            }));
        }

        // Wait for all readers
        for handle in handles {
            handle.join().unwrap();
        }
    }

    #[test]
    fn test_load_guard() {
        let rcu = RcuPtr::new(42);

        // Access value through guard
        let guard = rcu.read();
        assert_eq!(*guard, 42);
    }

    #[test]
    fn test_load_guard_with_complex_type() {
        #[derive(Clone, PartialEq, Debug, Default)]
        struct Data {
            id: u32,
            name: String,
        }

        let rcu = RcuPtr::new(Data { id: 1, name: "test".to_string() });

        // Access fields through guard
        let guard = rcu.read();
        assert_eq!(guard.id, 1);
        assert_eq!(guard.name, "test");
    }
}
