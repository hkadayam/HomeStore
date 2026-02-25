use std::{cell::Cell, future::Future, pin::Pin, cell::UnsafeCell, thread};

use tokio::sync::mpsc::UnboundedSender;

pub type ReactorId = usize;
pub type LockId = u64;

type BoxFutureSendable = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;
type BoxFutureLocal = Pin<Box<dyn Future<Output = ()> + 'static>>;

// Thread-locals for reactor threads
thread_local! {
    static IS_REACTOR_THREAD: Cell<bool> = Cell::new(false);
    static REACTOR_ID: Cell<ReactorId> = Cell::new(usize::MAX);
}

pub struct Reactor {
    rid: ReactorId,
    remote_task_sender: UnsafeCell<Option<UnboundedSender<BoxFutureSendable>>>,
    thread_handle: UnsafeCell<Option<thread::JoinHandle<()>>>,
    shutdown_tx: UnsafeCell<Option<tokio::sync::oneshot::Sender<()>>>,
}

// Safety: Reactor fields accessed only from owning thread or main thread during shutdown.
unsafe impl Sync for Reactor {}

impl Reactor {
    pub fn create(rid: ReactorId) -> Self {
        let (remote_task_sender, remote_task_receiver) =
            tokio::sync::mpsc::unbounded_channel::<BoxFutureSendable>();
        let (shutdown_tx, shutdown_rx) = tokio::sync::oneshot::channel();

        let handle = thread::Builder::new()
            .name(format!("reactor-{}", rid))
            .spawn(move || {
                IS_REACTOR_THREAD.with(|flag| flag.set(true));
                REACTOR_ID.with(|id| id.set(rid));

                let rt = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .expect("Failed to create tokio runtime");

                let local = tokio::task::LocalSet::new();

                rt.block_on(async {
                    local
                        .run_until(async move {
                            let work_handle =
                                tokio::task::spawn_local(Self::work_loop(remote_task_receiver));

                            // Wait for shutdown signal
                            let _ = shutdown_rx.await;

                            // Drain remaining queued tasks before exiting
                            let _ = work_handle.await;
                        })
                        .await;
                });
            })
            .expect("Failed to spawn reactor thread");

        Self {
            rid,
            remote_task_sender: UnsafeCell::new(Some(remote_task_sender)),
            thread_handle: UnsafeCell::new(Some(handle)),
            shutdown_tx: UnsafeCell::new(Some(shutdown_tx)),
        }
    }

    /// Drain remote task channel and spawn each future on the LocalSet.
    async fn work_loop(mut remote_receiver: tokio::sync::mpsc::UnboundedReceiver<BoxFutureSendable>) {
        while let Some(fut) = remote_receiver.recv().await {
            tokio::task::spawn_local(fut);
        }
    }

    pub fn spawn_all(num: usize) -> Vec<Reactor> {
        (0..num).map(|rid| Reactor::create(rid)).collect()
    }

    pub fn id(&self) -> ReactorId { self.rid }

    /// Enqueue a Send future to run on this reactor's thread.
    pub fn spawn_future(&self, fut: BoxFutureSendable) {
        // Safety: Only main thread or reactor threads call this before shutdown
        unsafe {
            if let Some(sender) = (*self.remote_task_sender.get()).as_ref() {
                let _ = sender.send(fut);
            }
        }
    }

    /// Spawn a local (!Send) future on this reactor's thread.
    /// Must be called from within this reactor's LocalSet (i.e., from reactor thread).
    pub fn spawn_local_future(&self, fut: BoxFutureLocal) {
        tokio::task::spawn_local(fut);
    }

    pub fn wake(&self, _lock_id: LockId) {
        // No-op for tokio — no distributed locking support
    }

    pub async fn shutdown(&self) {
        // Safety: Only called once from main thread during shutdown
        unsafe {
            if let Some(tx) = (*self.shutdown_tx.get()).take() {
                let _ = tx.send(());
            }

            // Drop sender to close the channel — work_loop will drain then exit
            let remote_sender = (*self.remote_task_sender.get()).take();
            drop(remote_sender);

            if let Some(handle) = (*self.thread_handle.get()).take() {
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
#[allow(dead_code)]
pub fn is_reactor_thread() -> bool { IS_REACTOR_THREAD.with(|flag| flag.get()) }

/// Return the current reactor ID, or None if not on a reactor thread.
pub fn current_reactor_id() -> Option<ReactorId> {
    let id = REACTOR_ID.with(|id| id.get());
    if id == usize::MAX { None } else { Some(id) }
}
