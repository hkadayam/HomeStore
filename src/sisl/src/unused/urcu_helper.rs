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

use crossbeam_epoch::{self as epoch, Atomic, Guard, Owned, Shared};
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::{Arc, Mutex};
use std::collections::HashSet;
use std::marker::PhantomData;

/// RCU control for managing epoch-based reclamation
pub struct RcuCtl;

impl RcuCtl {
    /// Register current thread for RCU operations
    /// In crossbeam-epoch, this is handled automatically, but we provide
    /// this for API compatibility with C++ version
    pub fn register_rcu() {
        // crossbeam-epoch handles thread registration automatically
        // This is a no-op for API compatibility
    }
    
    /// Unregister current thread from RCU operations
    pub fn unregister_rcu() {
        // crossbeam-epoch handles thread unregistration automatically
        // This is a no-op for API compatibility
    }
    
    /// Synchronize RCU - wait for all readers to finish
    pub fn sync_rcu() {
        // In crossbeam-epoch, we can trigger garbage collection
        epoch::default_collector().collect();
    }
}

/// RAII wrapper for RCU read access
/// Equivalent to C++ `_urcu_access_ptr<T>`
pub struct RcuAccessPtr<'guard, T> {
    ptr: Shared<'guard, T>,
    _guard: &'guard Guard,
}

impl<'guard, T> RcuAccessPtr<'guard, T> {
    fn new(ptr: Shared<'guard, T>, guard: &'guard Guard) -> Self {
        Self { ptr, _guard: guard }
    }
    
    pub fn get(&self) -> Option<&'guard T> {
        unsafe { self.ptr.as_ref() }
    }
}

impl<'guard, T> std::ops::Deref for RcuAccessPtr<'guard, T> {
    type Target = T;
    
    fn deref(&self) -> &Self::Target {
        unsafe { self.ptr.as_ref().unwrap() }
    }
}

/// RCU-protected data container
/// Equivalent to C++ `urcu_data<T>`
pub struct RcuData<T> {
    atomic_ptr: Atomic<T>,
}

impl<T> RcuData<T> {
    /// Create new RCU data with initial value
    pub fn new(value: T) -> Self {
        Self {
            atomic_ptr: Atomic::new(value),
        }
    }
    
    /// Get read access to the data
    pub fn get(&self) -> RcuAccessPtr<'_, T> {
        let guard = epoch::pin();
        let ptr = self.atomic_ptr.load(Ordering::Acquire, &guard);
        RcuAccessPtr::new(ptr, &guard)
    }
    
    /// Create new value and exchange with old one
    /// Returns the old value
    pub fn make_and_exchange(&self, new_value: T) -> Option<T> {
        let guard = epoch::pin();
        let new_ptr = Owned::new(new_value);
        let old_ptr = self.atomic_ptr.swap(new_ptr, Ordering::AcqRel, &guard);
        
        unsafe {
            if !old_ptr.is_null() {
                // Defer deallocation until all readers are done
                guard.defer_destroy(old_ptr);
                // We can't return the old value since it might be deallocated
                // This is a limitation compared to C++ version
                None
            } else {
                None
            }
        }
    }
    
    /// Read the data with a callback
    pub fn read<F, R>(&self, callback: F) -> R
    where
        F: FnOnce(&T) -> R,
    {
        let guard = epoch::pin();
        let ptr = self.atomic_ptr.load(Ordering::Acquire, &guard);
        unsafe {
            callback(ptr.as_ref().unwrap())
        }
    }
}

/// RCU scoped pointer - simplified version
/// Equivalent to C++ `urcu_scoped_ptr<T>`
pub struct RcuScopedPtr<T> {
    atomic_ptr: Atomic<T>,
    update_mutex: Mutex<()>,
}

impl<T: Clone> RcuScopedPtr<T> {
    /// Create new RCU scoped pointer
    pub fn new(value: T) -> Self {
        Self {
            atomic_ptr: Atomic::new(value),
            update_mutex: Mutex::new(()),
        }
    }
    
    /// Read access with callback
    pub fn read<F, R>(&self, callback: F) -> R
    where
        F: FnOnce(&T) -> R,
    {
        let guard = epoch::pin();
        let ptr = self.atomic_ptr.load(Ordering::Acquire, &guard);
        unsafe {
            callback(ptr.as_ref().unwrap())
        }
    }
    
    /// Get access pointer for reading
    pub fn access(&self) -> RcuAccessPtr<'_, T> {
        let guard = epoch::pin();
        let ptr = self.atomic_ptr.load(Ordering::Acquire, &guard);
        RcuAccessPtr::new(ptr, &guard)
    }
    
    /// Update with callback that modifies a copy
    pub fn update<F>(&self, edit_callback: F)
    where
        F: FnOnce(&mut T),
    {
        let _lock = self.update_mutex.lock().unwrap();
        let guard = epoch::pin();
        
        // Load current value
        let current_ptr = self.atomic_ptr.load(Ordering::Acquire, &guard);
        let current_value = unsafe { current_ptr.as_ref().unwrap() };
        
        // Create new value by cloning and modifying
        let mut new_value = current_value.clone();
        edit_callback(&mut new_value);
        
        // Exchange with new value
        let new_ptr = Owned::new(new_value);
        let old_ptr = self.atomic_ptr.swap(new_ptr, Ordering::AcqRel, &guard);
        
        unsafe {
            if !old_ptr.is_null() {
                guard.defer_destroy(old_ptr);
            }
        }
    }
    
    /// Create new instance and exchange
    pub fn make_and_exchange(&self, new_value: T) {
        let _lock = self.update_mutex.lock().unwrap();
        let guard = epoch::pin();
        
        let new_ptr = Owned::new(new_value);
        let old_ptr = self.atomic_ptr.swap(new_ptr, Ordering::AcqRel, &guard);
        
        unsafe {
            if !old_ptr.is_null() {
                guard.defer_destroy(old_ptr);
            }
        }
    }
}

/// Batch operations for multiple RCU data items
/// Equivalent to C++ `urcu_data_batch<T>`
pub struct RcuBatch<T> {
    batch: Mutex<HashSet<*const RcuData<T>>>,
}

impl<T> RcuBatch<T> {
    pub fn new() -> Self {
        Self {
            batch: Mutex::new(HashSet::new()),
        }
    }
    
    /// Add RCU data to batch
    pub fn add(&self, data: &RcuData<T>) {
        let mut batch = self.batch.lock().unwrap();
        batch.insert(data as *const RcuData<T>);
    }
    
    /// Remove RCU data from batch
    pub fn remove(&self, data: &RcuData<T>) {
        let mut batch = self.batch.lock().unwrap();
        batch.remove(&(data as *const RcuData<T>));
    }
    
    /// Exchange all items in batch with new value
    pub fn exchange(&self, new_value: T) 
    where 
        T: Clone,
    {
        let guard = epoch::pin();
        let batch = self.batch.lock().unwrap();
        
        for &data_ptr in batch.iter() {
            unsafe {
                let data = &*data_ptr;
                let new_ptr = Owned::new(new_value.clone());
                let old_ptr = data.atomic_ptr.swap(new_ptr, Ordering::AcqRel, &guard);
                
                if !old_ptr.is_null() {
                    guard.defer_destroy(old_ptr);
                }
            }
        }
    }
}

// Singleton instance for batch operations
impl<T> Default for RcuBatch<T> {
    fn default() -> Self {
        Self::new()
    }
}

/// Thread-local RCU registration flag for compatibility
thread_local! {
    static RCU_REGISTERED: std::cell::Cell<bool> = std::cell::Cell::new(false);
}

/// Initialize RCU registration (compatibility macro equivalent)
pub fn rcu_register_init() {
    RCU_REGISTERED.with(|registered| {
        if !registered.get() {
            RcuCtl::register_rcu();
            registered.set(true);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::thread;
    use std::time::Duration;

    #[test]
    fn test_rcu_data_basic() {
        let rcu_data = RcuData::new(42);
        
        // Test read access
        rcu_data.read(|value| {
            assert_eq!(*value, 42);
        });
        
        // Test exchange
        rcu_data.make_and_exchange(100);
        
        rcu_data.read(|value| {
            assert_eq!(*value, 100);
        });
    }
    
    #[test]
    fn test_rcu_scoped_ptr() {
        let rcu_ptr = RcuScopedPtr::new(String::from("hello"));
        
        // Test read
        rcu_ptr.read(|s| {
            assert_eq!(s, "hello");
        });
        
        // Test update
        rcu_ptr.update(|s| {
            s.push_str(" world");
        });
        
        rcu_ptr.read(|s| {
            assert_eq!(s, "hello world");
        });
    }
    
    #[test]
    fn test_rcu_access_ptr() {
        let rcu_ptr = RcuScopedPtr::new(vec![1, 2, 3]);
        
        {
            let access = rcu_ptr.access();
            assert_eq!(access.len(), 3);
            assert_eq!(access[0], 1);
        }
        
        rcu_ptr.update(|v| v.push(4));
        
        {
            let access = rcu_ptr.access();
            assert_eq!(access.len(), 4);
            assert_eq!(access[3], 4);
        }
    }
    
    #[test]
    fn test_concurrent_access() {
        let rcu_ptr = Arc::new(RcuScopedPtr::new(0));
        let mut handles = vec![];
        
        // Spawn readers
        for _ in 0..4 {
            let rcu_clone = Arc::clone(&rcu_ptr);
            handles.push(thread::spawn(move || {
                for _ in 0..100 {
                    rcu_clone.read(|value| {
                        // Just access the value
                        let _ = *value;
                    });
                    thread::sleep(Duration::from_millis(1));
                }
            }));
        }
        
        // Spawn writer
        let rcu_clone = Arc::clone(&rcu_ptr);
        handles.push(thread::spawn(move || {
            for i in 1..=50 {
                rcu_clone.update(|value| {
                    *value = i;
                });
                thread::sleep(Duration::from_millis(2));
            }
        }));
        
        // Wait for all threads
        for handle in handles {
            handle.join().unwrap();
        }
        
        // Final value should be 50
        rcu_ptr.read(|value| {
            assert_eq!(*value, 50);
        });
    }
    
    #[test]
    fn test_rcu_batch() {
        let batch = RcuBatch::new();
        let data1 = RcuData::new(1);
        let data2 = RcuData::new(2);
        
        batch.add(&data1);
        batch.add(&data2);
        
        // Exchange all to new value
        batch.exchange(100);
        
        data1.read(|value| assert_eq!(*value, 100));
        data2.read(|value| assert_eq!(*value, 100));
    }
}