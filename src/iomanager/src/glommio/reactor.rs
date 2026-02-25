use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    future::Future,
    pin::Pin,
    rc::Rc,
    sync::Arc,
};

use glommio::{
    channels::{shared_channel, SharedSender},
    sync::{Semaphore, WaitQueue},
    LocalExecutorBuilder,
};

pub type ReactorId = usize;
pub type LockId = u64;

#[derive(Debug, Clone, Copy)]
pub struct WakeMsg {
    pub lock_id: LockId,
}

thread_local! { static CURRENT_REACTOR_ID: Cell<Option<ReactorId>> = Cell::new(None); }

type BoxFutureSendable = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;
type BoxFutureLocal = Pin<Box<dyn Future<Output = ()> + 'static>>;

/// Glommio-based reactor pinned to one CPU/thread.
/// Three loops run concurrently inside the executor:
///   1. Remote task dispatch — spawns Send futures from other threads
///   2. Lock wake loop — wakes async mutexes on this reactor
pub struct Reactor {
    rid: ReactorId,

    // Loop 1: task dispatch from other threads
    remote_task_sender: RefCell<Option<SharedSender<BoxFutureSendable>>>,

    // Loop 2: lock wake mechanism
    lock_sender: RefCell<Option<SharedSender<WakeMsg>>>,
    wait_map: Rc<RefCell<HashMap<LockId, Rc<WaitQueue>>>>,

    // Executor control
    handle: glommio::executor::JoinHandle<()>,
    stop_sem: Arc<Semaphore>,
}

impl Reactor {
    pub fn create(rid: ReactorId) -> Result<Self, glommio::GlommioError> {
        let (lock_sender, lock_receiver) = shared_channel::new_bounded(1024);
        let (remote_task_sender, remote_task_receiver) = shared_channel::new_bounded(1024);
        let stop_sem = Arc::new(Semaphore::new(0));

        let wait_map: Rc<RefCell<HashMap<LockId, Rc<WaitQueue>>>> =
            Rc::new(RefCell::new(HashMap::new()));
        let wait_map_inner = wait_map.clone();
        let stop_clone = stop_sem.clone();

        let handle = LocalExecutorBuilder::new().pin_to_cpu(rid).spawn(move || async move {
            CURRENT_REACTOR_ID.with(|c| c.set(Some(rid)));

            // Loop 1: REMOTE TASK DISPATCH
            glommio::spawn_local(async move {
                while let Some(fut) = remote_task_receiver.recv().await {
                    glommio::spawn_local(fut).detach();
                }
            })
            .detach();

            // Loop 2: LOCK WAKE
            glommio::spawn_local(async move {
                while let Some(msg) = lock_receiver.recv().await {
                    if let Some(wq) = wait_map_inner.borrow().get(&msg.lock_id) {
                        wq.wake_one();
                    }
                }
            })
            .detach();

            stop_clone.acquire(1).await.expect("Semaphore acquire");
        })?;

        Ok(Self {
            rid,
            remote_task_sender: RefCell::new(Some(remote_task_sender)),
            lock_sender: RefCell::new(Some(lock_sender)),
            wait_map,
            handle,
            stop_sem,
        })
    }

    pub fn id(&self) -> ReactorId { self.rid }

    /// Enqueue a Send future to run on this reactor's executor.
    pub fn spawn_future(&self, fut: BoxFutureSendable) {
        if let Some(ref sender) = *self.remote_task_sender.borrow() {
            let _ = sender.try_send(fut);
        }
    }

    /// Spawn a local (!Send) future — must be called from this reactor's thread.
    pub fn spawn_local_future(&self, fut: BoxFutureLocal) {
        glommio::spawn_local(fut).detach();
    }

    /// Get or create WaitQueue for a lock ID (single-threaded, no mutex needed).
    pub fn wait_queue(&self, lock_id: LockId) -> Rc<WaitQueue> {
        if let Some(existing) = self.wait_map.borrow().get(&lock_id) {
            return existing.clone();
        }
        let wq = Rc::new(WaitQueue::new());
        self.wait_map.borrow_mut().insert(lock_id, wq.clone());
        wq
    }

    /// Send a wake message to this reactor for async mutex support.
    pub fn wake(&self, lock_id: LockId) {
        if let Some(s) = self.lock_sender.borrow().as_ref() {
            let _ = s.try_send(WakeMsg { lock_id });
        }
    }

    pub async fn shutdown(&self) {
        // Close all channels — loops will exit on None
        let _ = self.lock_sender.borrow_mut().take();
        let _ = self.remote_task_sender.borrow_mut().take();

        // Signal executor to stop
        self.stop_sem.add_permits(1);
        let _ = self.handle.join().await;
    }

    pub fn spawn_all(num: usize) -> Result<Vec<Reactor>, glommio::GlommioError> {
        let mut v = Vec::with_capacity(num);
        for rid in 0..num {
            v.push(Reactor::create(rid)?);
        }
        Ok(v)
    }
}

/// Accessor for current reactor id; returns None if called outside a reactor thread.
pub fn current_reactor_id() -> Option<ReactorId> { CURRENT_REACTOR_ID.with(|c| c.get()) }
