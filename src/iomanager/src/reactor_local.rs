//! # ReactorLocal
//!
//! A per-reactor storage container that provides thread-safe access to
//! reactor-local data. Each reactor gets its own dedicated slot in a fixed-size
//! array, indexed by reactor_id.

use std::{cell::UnsafeCell, sync::Arc};

/// Inner data structure holding the per-reactor values.
struct ReactorLocalInner<T>
where
    T: Send + 'static,
{
    /// Per-reactor values, indexed by reactor_id.
    /// UnsafeCell allows interior mutability; safe because each reactor_id is
    /// accessed only by its corresponding reactor thread.
    reactor_values: Vec<UnsafeCell<T>>,
}

// Safety: ReactorLocalInner is Send because each reactor accesses only its own slot.
unsafe impl<T> Send for ReactorLocalInner<T> where T: Send + 'static {}

// Safety: ReactorLocalInner is Sync because reactors never access each other's slots.
unsafe impl<T> Sync for ReactorLocalInner<T> where T: Send + 'static {}

/// A per-reactor storage container for reactor-local data.
///
/// Each reactor maintains its own `T` for fast, lock-free local access.
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
    ///
    /// # Arguments
    ///
    /// * `num_reactors` - The number of reactors
    /// * `init` - A function that returns the initial value for each reactor
    pub fn new<F>(num_reactors: usize, init: F) -> Self
    where
        F: Fn() -> T,
    {
        let mut reactor_values = Vec::with_capacity(num_reactors);
        for _ in 0..num_reactors {
            reactor_values.push(UnsafeCell::new(init()));
        }
        Self { inner: Arc::new(ReactorLocalInner { reactor_values }) }
    }

    /// Create a new ReactorLocal from a pre-allocated vector of values.
    ///
    /// # Arguments
    ///
    /// * `values` - Vector of values, one per reactor
    pub fn from_vec(values: Vec<T>) -> Self {
        let reactor_values = values.into_iter().map(|v| UnsafeCell::new(v)).collect();
        Self { inner: Arc::new(ReactorLocalInner { reactor_values }) }
    }

    /// Get a reference to the current reactor's value.
    ///
    /// # Panics
    ///
    /// Panics if called outside a reactor context or if IOManager is not initialized.
    #[inline]
    pub fn get(&self) -> &T {
        let rid = crate::iomanager::iomgr().current_reactor_id();
        unsafe { &*self.inner.reactor_values[rid].get() }
    }

    /// Get a reference to a specific reactor's value.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the target reactor is not currently
    /// accessing its value.
    ///
    /// # Arguments
    ///
    /// * `reactor_id` - The reactor ID to access
    ///
    /// # Panics
    ///
    /// Panics if reactor_id >= num_reactors.
    #[inline]
    pub fn get_for_reactor(&self, reactor_id: usize) -> &T {
        unsafe { &*self.inner.reactor_values[reactor_id].get() }
    }

    /// Get the number of reactors.
    pub fn num_reactors(&self) -> usize { self.inner.reactor_values.len() }
}

