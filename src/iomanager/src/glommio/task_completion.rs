//! Glommio-specific task completion tracker.
//!
//! Used by BackgroundTasks to track when all spawned tasks have completed.

use std::sync::atomic::{AtomicUsize, Ordering};

/// Task completion tracker for glommio backend.
/// Tracks the number of pending background tasks and provides notification when all complete.
pub struct TaskCompletion {
    pending_count: AtomicUsize,
    semaphore: glommio::sync::Semaphore,
}

impl TaskCompletion {
    /// Create a new task completion tracker.
    pub fn new() -> Self {
        Self {
            pending_count: AtomicUsize::new(0),
            semaphore: glommio::sync::Semaphore::new(0),
        }
    }

    /// Mark that a task has started.
    /// Must be called before spawning the task.
    pub fn task_started(&self) {
        self.pending_count.fetch_add(1, Ordering::SeqCst);
    }

    /// Mark that a task has completed.
    /// Should be called at the end of the task's execution.
    pub fn task_completed(&self) {
        let prev = self.pending_count.fetch_sub(1, Ordering::SeqCst);
        if prev == 1 {
            // Last task completed - signal all waiters
            // Use signal with large value to wake all potential waiters
            self.semaphore.signal(usize::MAX);
        }
    }

    /// Wait for all pending tasks to complete.
    /// Returns immediately if no tasks are pending.
    pub async fn wait_all(&self) {
        loop {
            // Check if all tasks completed
            if self.pending_count.load(Ordering::SeqCst) == 0 {
                return;
            }
            
            // Wait for signal that a task completed
            // acquire() will block until signal() is called
            let _ = self.semaphore.acquire(1).await;
        }
    }
}

impl Default for TaskCompletion {
    fn default() -> Self {
        Self::new()
    }
}

