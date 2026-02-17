use std::{cell::Cell, future::Future, pin::Pin, cell::UnsafeCell, sync::Arc, thread};

use tokio::sync::mpsc::{unbounded_channel, UnboundedSender};

use super::result_channel::{self, ResultChannel, ChannelMessage};

pub type ReactorId = usize;
pub type LockId = u64;
pub type ResultSender = crate::result_channel::ResultSender<tokio::sync::mpsc::UnboundedSender<ChannelMessage>>;
pub type ResultReceiver<'a> = crate::result_channel::ResultReceiver<
    'a,
    tokio::sync::mpsc::UnboundedSender<ChannelMessage>,
    tokio::sync::mpsc::UnboundedReceiver<ChannelMessage>,
>;

type BoxFutureSendable = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;
type BoxFutureLocal = Pin<Box<dyn Future<Output = ()> + 'static>>;

// Thread-local flag to mark reactor threads
thread_local! {
    static IS_REACTOR_THREAD: Cell<bool> = Cell::new(false);
    static LOCAL_TASK_SENDER: std::cell::RefCell<Option<UnboundedSender<BoxFutureLocal>>> = std::cell::RefCell::new(None);
}

pub struct Reactor {
    rid: ReactorId,
    remote_task_sender: UnsafeCell<Option<UnboundedSender<BoxFutureSendable>>>,
    thread_handle: UnsafeCell<Option<thread::JoinHandle<()>>>,
    channel: Arc<ResultChannel>,
    shutdown_tx: UnsafeCell<Option<tokio::sync::oneshot::Sender<()>>>,
}

// Safety: Reactor is Sync because task_sender and thread_handle are only accessed
// from the main thread (during shutdown). No concurrent access occurs.
unsafe impl Sync for Reactor {}

impl Reactor {
    /// Create a reactor that runs in its own thread with a tokio runtime.
    /// The reactor creates its own multiplexed channel for cross-reactor communication.
    pub fn create(rid: ReactorId) -> Self {
        // Create channel for remote (Send) tasks
        let (remote_task_sender, remote_task_receiver) = unbounded_channel::<BoxFutureSendable>();

        // Create shutdown signal
        let (shutdown_tx, shutdown_rx) = tokio::sync::oneshot::channel();

        // Create result channel for this reactor - owns both sender and receiver!
        // Arc-wrapped so it can be shared between Reactor struct and spawned thread
        let channel = Arc::new(result_channel::create());
        let channel_for_thread = Arc::clone(&channel);

        let handle = thread::Builder::new()
            .name(format!("reactor-{}", rid))
            .spawn(move || {
                // Mark this as a reactor thread
                IS_REACTOR_THREAD.with(|flag| flag.set(true));

                // Use current_thread runtime to ensure all tasks run on this specific thread
                let rt = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .expect("Failed to create tokio runtime");

                // Create LocalSet for spawning !Send futures
                let local = tokio::task::LocalSet::new();

                // Run the LocalSet until shutdown signal
                rt.block_on(async {
                    local
                        .run_until(async move {
                            // Create local channel inside the thread and store in thread-local
                            let (local_task_sender, local_task_receiver) = unbounded_channel::<BoxFutureLocal>();
                            LOCAL_TASK_SENDER.with(|sender| {
                                *sender.borrow_mut() = Some(local_task_sender);
                            });

                            // Spawn receiver loop - keep handle to wait for it
                            let channel_for_spawn = Arc::clone(&channel_for_thread);
                            let receiver_handle = tokio::task::spawn_local(async move {
                                channel_for_spawn.run_receiver_loop().await;
                            });

                            // Run work_loop in background - keep handle to wait for it
                            let work_handle =
                                tokio::task::spawn_local(Self::work_loop(remote_task_receiver, local_task_receiver));

                            // Wait for shutdown signal
                            let _ = shutdown_rx.await;

                            LOCAL_TASK_SENDER.with(|sender| {
                                *sender.borrow_mut() = None; // Drop the sender, closing the channel
                            });

                            // CRITICAL: Wait for all spawned tasks to complete
                            // Channels are already closed by shutdown() (remote_sender dropped)
                            // Now drain all pending tasks before exiting
                            let _ = work_handle.await; // Wait for work_loop to finish all queued tasks

                            // Force close the receiver to ensure receiver_loop exits
                            // This handles the case where spawned tasks may still hold ResultSender clones
                            // channel_for_thread.close_receiver();
                            // let _ = receiver_handle.await;  // Wait for receiver_loop to finish
                            receiver_handle.abort();
                        })
                        .await;
                });
            })
            .expect("Failed to spawn reactor thread");

        Self {
            rid,
            remote_task_sender: UnsafeCell::new(Some(remote_task_sender)),
            thread_handle: UnsafeCell::new(Some(handle)),
            channel,
            shutdown_tx: UnsafeCell::new(Some(shutdown_tx)),
        }
    }

    /// Work loop - receives and executes spawned tasks from both channels.
    async fn work_loop(
        mut remote_receiver: tokio::sync::mpsc::UnboundedReceiver<BoxFutureSendable>,
        mut local_receiver: tokio::sync::mpsc::UnboundedReceiver<BoxFutureLocal>,
    ) {
        loop {
            tokio::select! {
                Some(fut) = remote_receiver.recv() => {
                    tokio::task::spawn_local(fut);
                }
                Some(fut) = local_receiver.recv() => {
                    tokio::task::spawn_local(fut);
                }
                else => break,
            }
        }
    }

    pub fn spawn_all(num: usize) -> Vec<Reactor> { (0..num).map(|rid| Reactor::create(rid)).collect() }

    pub fn id(&self) -> ReactorId { self.rid }

    /// Create a result handle for cross-reactor RPC-style calls.
    /// Returns (result_tx, result_rx): send result_tx to target reactor, use result_rx to wait.
    pub fn create_result_handle(&self) -> (ResultSender, ResultReceiver<'_>) { self.channel.create_result_handle() }

    /// Enqueue a Send future to run on this reactor's thread (for cross-reactor spawns).
    pub fn spawn_future(&self, fut: BoxFutureSendable) {
        // Safety: Only main thread or reactor threads call this before shutdown
        unsafe {
            if let Some(sender) = (*self.remote_task_sender.get()).as_ref() {
                let _ = sender.send(fut); // Ignore if channel is closed
            }
        }
    }

    /// Enqueue a local (non-Send) future to run on this reactor's thread.
    /// Must be called from the reactor's own thread.
    pub fn spawn_local_future(&self, fut: BoxFutureLocal) {
        LOCAL_TASK_SENDER.with(|sender| {
            if let Some(ref s) = *sender.borrow() {
                let _ = s.send(fut); // Ignore if channel is closed
            }
        });
    }

    pub fn wake(&self, _lock_id: LockId) {
        // No-op for tokio - no distributed locking support
    }

    pub async fn shutdown(&self) {
        // Safety: Only called once from main thread during shutdown
        unsafe {
            // Send shutdown signal to reactor thread
            if let Some(tx) = (*self.shutdown_tx.get()).take() {
                let _ = tx.send(()); // Ignore if already closed
            }

            // Take and drop the remote sender to close the channel
            // Local sender is thread-local and will be cleaned up when the thread exits
            let remote_sender = (*self.remote_task_sender.get()).take();
            drop(remote_sender);

            // Take the thread handle and join it
            if let Some(handle) = (*self.thread_handle.get()).take() {
                // Use spawn_blocking and AWAIT it to ensure we wait for thread completion
                tokio::task::spawn_blocking(move || {
                    let _ = handle.join();
                })
                .await
                .expect("Failed to join reactor thread");
            }
        }
    }
}

/// Check if current thread is a reactor thread.
pub fn is_reactor_thread() -> bool { IS_REACTOR_THREAD.with(|flag| flag.get()) }

/// Accessor for current reactor id.
/// Tokio doesn't have reactor affinity, returns Some(0) for reactor threads, None for non-reactor.
pub fn current_reactor_id() -> Option<ReactorId> { if is_reactor_thread() { Some(0) } else { None } }
