//! Result channel for efficient cross-reactor communication.
//!
//! Instead of creating a fresh channel for each cross-reactor call, we use
//! a single persistent channel per reactor with message-based multiplexing.
//! This provides O(1) message slot allocation and reuse.
//!
//! ## Simplified Design
//! - ResultChannel owns both sender and receiver (no split ownership)
//! - ResultReceiver implements Future directly (no MessageWaiter wrapper)
//! - Clean API with no unsafe raw pointers in user code
//!
//! ## Usage
//! ```ignore
//! let (result_tx, result_rx) = source_reactor.create_result_handle();
//!
//! target_reactor.spawn_future(Box::pin(async move {
//!     let result = do_work().await;
//!     result_tx.send_result(result).await.unwrap();
//! }));
//!
//! let result: MyType = result_rx.wait().await;
//! ```

use std::future::Future;
use std::pin::Pin;
use std::cell::UnsafeCell;
use std::collections::VecDeque;
use std::task::{Context, Poll, Waker};

/// ID for a message slot in the multiplexed channel.
pub type MessageId = usize;

/// A message slot that holds the result and waker for a pending operation.
#[derive(Default)]
struct Message {
    result: Option<Box<dyn std::any::Any + Send>>,
    waker: Option<Waker>,
}

/// Result channel for cross-reactor communication.
/// Each reactor has one of these, owns both sender and receiver.
/// Uses message multiplexing internally for efficient O(1) allocation.
///
/// **Lock-free design:**
/// Each reactor accesses ONLY its own ResultChannel - single-threaded access!
///
/// Flow:
/// 1. Calling reactor: allocates message from ITS OWN channel
/// 2. Calling reactor: sends ResultSender to target reactor
/// 3. Target reactor: executes work, sends result back via sender (uses atomic channel)
/// 4. Calling reactor's demux loop: receives result via run_receiver_loop(), stores in ITS OWN channel
/// 5. Calling reactor: waits on ITS OWN channel
///
/// Uses UnsafeCell for interior mutability - no locks, no atomics, pure single-threaded access.
/// Message allocation uses Vec<Option<Message>> + VecDeque for O(1) alloc/free with simple code.
/// Safety: Only the owning reactor's thread accesses these fields.
pub struct ResultChannel<S, R> {
    sender: S,
    receiver: UnsafeCell<Option<R>>, // UnsafeCell so receiver loop can access via &self
    messages: UnsafeCell<Vec<Option<Message>>>,
    free_ids: UnsafeCell<VecDeque<MessageId>>,
}

// Safety: ResultChannel is Sync because only the owning reactor accesses receiver/messages/free_ids.
// The sender S and receiver R are already Send+Sync (they're atomic channel endpoints).
unsafe impl<S: Send, R: Send> Sync for ResultChannel<S, R> {}

impl<S, R> ResultChannel<S, R> {
    /// Create a new multiplexed channel with the given sender and receiver.
    pub fn new(sender: S, receiver: R) -> Self {
        Self {
            sender,
            receiver: UnsafeCell::new(Some(receiver)),
            messages: UnsafeCell::new(Vec::new()),
            free_ids: UnsafeCell::new(VecDeque::new()),
        }
    }

    /// Get a reference to the sender (for spawning work on remote reactor).
    pub fn sender(&self) -> &S { &self.sender }

    /// Take the receiver out of the channel (can only be called once).
    /// This is used to start the receiver loop.
    pub fn take_receiver(&self) -> Option<R> {
        // Safety: Only accessed by owning reactor thread during startup
        unsafe { (*self.receiver.get()).take() }
    }

    /// Allocate a message slot from the free pool.
    /// O(1) operation using VecDeque of free IDs.
    pub fn allocate_message(&self) -> MessageId {
        // Safety: Only accessed by owning reactor thread
        let free_ids = unsafe { &mut *self.free_ids.get() };
        let messages = unsafe { &mut *self.messages.get() };

        // Try to reuse a freed ID first
        if let Some(id) = free_ids.pop_front() {
            // Reuse this slot
            messages[id] = Some(Message::default());
            return id;
        }

        // No free IDs, allocate new slot
        let id = messages.len();
        messages.push(Some(Message::default()));
        id
    }

    /// Free a message slot back to the pool.
    /// O(1) operation pushing to VecDeque.
    fn free_message(&self, id: MessageId) {
        // Safety: Only accessed by owning reactor thread
        let messages = unsafe { &mut *self.messages.get() };
        messages[id] = None; // Clear the message

        let free_ids = unsafe { &mut *self.free_ids.get() };
        free_ids.push_back(id);
    }

    /// Put a result for a message and wake the waiter.
    /// Called by the receiver demux loop running on this reactor.
    pub fn put_result(&self, msg_id: MessageId, result: Box<dyn std::any::Any + Send>) {
        // Safety: Only accessed by owning reactor thread (message loop)
        let messages = unsafe { &mut *self.messages.get() };
        if let Some(msg) = &mut messages[msg_id] {
            msg.result = Some(result);
            if let Some(waker) = msg.waker.take() {
                waker.wake();
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// High-level Result Handle API
// ═══════════════════════════════════════════════════════════════════════════

/// Sender half of a cross-reactor result handle.
/// Send this to the target reactor to return results.
pub struct ResultSender<S> {
    msg_id: MessageId,
    sender: S,
}

// Backend-specific implementations are in each runtime's module
impl<S> ResultSender<S> {
    pub fn msg_id(&self) -> MessageId { self.msg_id }

    pub fn sender(&self) -> &S { &self.sender }

    pub fn into_parts(self) -> (MessageId, S) { (self.msg_id, self.sender) }
}

/// Receiver half of a cross-reactor result handle.
/// Implements Future directly - just await or call .wait<R>() for typed results.
pub struct ResultReceiver<'a, S, R> {
    msg_id: MessageId,
    channel: &'a ResultChannel<S, R>,
}

// ResultReceiver IS the Future - no wrapper needed!
impl<'a, S, R> Future for ResultReceiver<'a, S, R> {
    type Output = Box<dyn std::any::Any + Send>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // Safety: Only accessed by owning reactor thread (waiting future)
        let messages = unsafe { &mut *self.channel.messages.get() };

        if let Some(msg) = &mut messages[self.msg_id] {
            if let Some(result) = msg.result.take() {
                return Poll::Ready(result);
            } else {
                // Not ready yet - register waker for when result arrives
                msg.waker = Some(cx.waker().clone());
                return Poll::Pending;
            }
        }

        // Message slot was freed - this shouldn't happen
        panic!("ResultReceiver polled after message was freed");
    }
}

// Auto-cleanup on drop
impl<'a, S, R> Drop for ResultReceiver<'a, S, R> {
    fn drop(&mut self) {
        // Automatically return message slot to free pool
        self.channel.free_message(self.msg_id);
    }
}

// Convenience method for type-safe consumption
impl<'a, S, R> ResultReceiver<'a, S, R> {
    /// Wait for the result from the target reactor.
    /// This will block (async) until `ResultSender::put_result()` is called.
    pub async fn wait<T: 'static>(self) -> T {
        let result_box = self.await; // Use Future impl
        *result_box.downcast::<T>().expect("Type mismatch in cross-reactor result")
    }
}

impl<S: Clone, R> ResultChannel<S, R> {
    /// Create a result handle for cross-reactor RPC-style calls.
    /// Returns (result_tx, result_rx): send result_tx to target reactor, use result_rx to wait.
    pub fn create_result_handle(&self) -> (ResultSender<S>, ResultReceiver<'_, S, R>) {
        let msg_id = self.allocate_message();
        let sender = self.sender().clone();

        let result_tx = ResultSender { msg_id, sender };
        let result_rx = ResultReceiver { msg_id, channel: self };

        (result_tx, result_rx)
    }
}
