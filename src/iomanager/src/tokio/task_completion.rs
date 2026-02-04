//! Tokio-specific task completion tracker.
//!
//! Used by BackgroundTasks to track when all spawned tasks have completed.

use std::sync::atomic::{AtomicUsize, Ordering};

/// Task completion tracker for tokio backend.
/// Tracks the number of pending background tasks and provides notification when all complete.
pub struct TaskCompletion {
    pending_count: AtomicUsize,
    notify: tokio::sync::Notify,
}

impl TaskCompletion {
    /// Create a new task completion tracker.
    pub fn new() -> Self {
        Self {
            pending_count: AtomicUsize::new(0),
            notify: tokio::sync::Notify::new(),
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
            // Last task completed - notify all waiters
            self.notify.notify_waiters();
        }
    }

    /// Wait for all pending tasks to complete.
    /// Returns immediately if no tasks are pending.
    ///
    /// Uses the correct Notify pattern to avoid race conditions.
    /// The key is to register for notification BEFORE checking the count.
    pub async fn wait_all(&self) {
        loop {
            // CRITICAL: Register for notification BEFORE checking count
            // This ensures we don't miss notifications that happen between check and wait
            let notified = self.notify.notified();
            
            // Now check if all tasks completed
            if self.pending_count.load(Ordering::SeqCst) == 0 {
                return;
            }
            
            // Wait for notification (we're guaranteed to receive it if tasks complete after our check)
            notified.await;
        }
    }
}

impl Default for TaskCompletion {
    fn default() -> Self {
        Self::new()
    }
}

