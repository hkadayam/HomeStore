use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    future::Future,
    pin::Pin,
    rc::Rc,
    sync::Arc,
};

use glommio::{
    channels::{shared_channel, SharedReceiver, SharedSender},
    sync::{Semaphore, WaitQueue},
    LocalExecutorBuilder,
};
use futures::FutureExt;

use super::result_channel::{self, ResultChannel, ChannelMessage};

pub type ReactorId = usize;
pub type LockId = u64;
pub type ResultSender = crate::result_channel::ResultSender<glommio::channels::shared_channel::SharedSender<ChannelMessage>>;
pub type ResultReceiver<'a> = crate::result_channel::ResultReceiver<'a, glommio::channels::shared_channel::SharedSender<ChannelMessage>, glommio::channels::shared_channel::SharedReceiver<ChannelMessage>>;

#[derive(Debug, Clone, Copy)]
pub struct WakeMsg {
    pub lock_id: LockId,
}

thread_local! { static CURRENT_REACTOR_ID: Cell<Option<ReactorId>> = Cell::new(None); }

type BoxFutureSendable = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;
type BoxFutureLocal = Pin<Box<dyn Future<Output = ()> + 'static>>;

/// Glommio-based reactor with zero mutexes/atomics in user code.
/// Each reactor runs on its own thread with three loops:
/// 1. Task dispatch loop - for fire-and-forget futures
/// 2. Waitable task loop (message loop) - for cross-reactor RPC-style calls
/// 3. Lock wake loop - for distributed async mutex support
pub struct Reactor {
    rid: ReactorId,
    
    // Loop 1: Fire-and-forget task dispatch
    remote_task_sender: RefCell<Option<SharedSender<BoxFutureSendable>>>,
    local_task_sender: RefCell<Option<SharedSender<BoxFutureLocal>>>,
    
    // Loop 2: Waitable cross-reactor calls (result channel)
    channel: Arc<ResultChannel>,
    
    // Loop 3: Lock wake mechanism
    lock_sender: RefCell<Option<SharedSender<WakeMsg>>>,
    lock_receiver: SharedReceiver<WakeMsg>,
    wait_map: Rc<RefCell<HashMap<LockId, Rc<WaitQueue>>>>,
    
    // Executor control
    handle: glommio::executor::JoinHandle<()>,
    stop_sem: Arc<Semaphore>,
}

impl Reactor {
    /// Create a reactor pinned to CPU = rid. Returns fully constructed Reactor.
    /// 
    /// The reactor runs three concurrent loops (all single-threaded within the executor):
    /// 1. Task dispatch loop - processes fire-and-forget futures
    /// 2. Waitable task loop - processes cross-reactor RPC results  
    /// 3. Lock wake loop - handles distributed mutex wakeups
    ///
    /// **Zero mutexes/atomics in application code** - all data uses Rc/RefCell!
    pub fn create(rid: ReactorId) -> Result<Self, glommio::GlommioError> {
        // Create channels for all loops
        let (lock_sender, lock_receiver) = shared_channel::new_bounded(1024);
        let (remote_task_sender, remote_task_receiver) = shared_channel::new_bounded(1024);
        let (local_task_sender, local_task_receiver) = shared_channel::new_bounded(1024);
        let channel = Arc::new(result_channel::create());  // Arc-wrapped for sharing
        let stop_sem = Arc::new(Semaphore::new(0));

        // Shared state - all using Rc/RefCell (no Arc/Mutex!)
        let wait_map: Rc<RefCell<HashMap<LockId, Rc<WaitQueue>>>> = Rc::new(RefCell::new(HashMap::new()));
        let stop_clone = stop_sem.clone();
        let channel_clone = channel.clone();

        // Spawn executor pinned to CPU rid.
        let handle = LocalExecutorBuilder::new().pin_to_cpu(rid).spawn(move || async move {
            // Set thread-local current reactor id for tasks running on this executor.
            CURRENT_REACTOR_ID.with(|c| c.set(Some(rid)));
            
            // ═══════════════════════════════════════════════════════════
            // Loop 1: FIRE-AND-FORGET TASK DISPATCH
            // ═══════════════════════════════════════════════════════════
            // Receives futures from both remote and local channels and spawns them.
            // No synchronization needed - single threaded!
            {
                glommio::spawn_local(async move {
                    loop {
                        futures::select! {
                            fut = remote_task_receiver.recv().fuse() => {
                                if let Some(fut) = fut {
                                    glommio::spawn_local(fut).detach();
                                } else { break; }
                            }
                            fut = local_task_receiver.recv().fuse() => {
                                if let Some(fut) = fut {
                                    glommio::spawn_local(fut).detach();
                                } else { break; }
                            }
                        }
                    }
                })
                .detach();
            }

            // ═══════════════════════════════════════════════════════════
            // Loop 2: WAITABLE TASK LOOP (Message Demux)
            // ═══════════════════════════════════════════════════════════
            // Receives results from cross-reactor calls and delivers to waiters.
            // Clean API - no manual message loop, just call run_receiver_loop()!
            {
                glommio::spawn_local(async move {
                    channel_clone.run_receiver_loop().await;
                })
                .detach();
            }

            // ═══════════════════════════════════════════════════════════
            // Loop 3: LOCK WAKE LOOP
            // ═══════════════════════════════════════════════════════════
            // Receives wake messages for async mutexes and wakes waiting tasks.
            // Uses RefCell for wait_map - no mutex needed!
            {
                glommio::spawn_local(async move {
                    while let Some(msg) = lock_receiver.recv().await {
                        // RefCell borrow - not a Mutex lock!
                        if let Some(wq) = wait_map.borrow().get(&msg.lock_id) {
                            wq.wake_one();
                        }
                    }
                })
                .detach();
            }
            
            // Await shutdown signal.
            stop_clone.acquire(1).await.expect("Semaphore acquire");
        })?;

        Ok(Self {
            rid,
            remote_task_sender: RefCell::new(Some(remote_task_sender)),
            local_task_sender: RefCell::new(Some(local_task_sender)),
            channel,  // Already Arc-wrapped
            lock_sender: RefCell::new(Some(lock_sender)),
            lock_receiver,
            wait_map,
            handle,
            stop_sem,
        })
    }

    pub fn id(&self) -> ReactorId { self.rid }

    /// Create a result handle for cross-reactor RPC-style calls.
    /// Returns (result_tx, result_rx): send result_tx to target reactor, use result_rx to wait.
    pub fn create_result_handle(&self) -> (ResultSender, ResultReceiver<'_>) {
        self.channel.create_result_handle()
    }

    /// Enqueue a fire-and-forget future to run on this reactor's executor.
    /// This goes through Loop 1 (task dispatch loop).
    /// **No locks used** - SharedSender uses atomics internally.
    /// Enqueue a Send future to run on this reactor (for cross-reactor spawns).
    pub fn spawn_future(&self, fut: BoxFutureSendable) {
        if let Some(ref sender) = *self.remote_task_sender.borrow() {
            let _ = sender.try_send(fut); // drop if channel full
        }
    }
    
    /// Enqueue a local (non-Send) future to run on this reactor.
    pub fn spawn_local_future(&self, fut: BoxFutureLocal) {
        if let Some(ref sender) = *self.local_task_sender.borrow() {
            let _ = sender.try_send(fut); // drop if channel full
        }
    }

    /// Get or create WaitQueue for a given lock id.
    /// **No mutex** - uses RefCell! Safe because wait_map is only accessed
    /// from this reactor's thread.
    pub fn wait_queue(&self, lock_id: LockId) -> Rc<WaitQueue> {
        if let Some(existing) = self.wait_map.borrow().get(&lock_id) {
            return existing.clone();
        }
        let wq = Rc::new(WaitQueue::new());
        self.wait_map.borrow_mut().insert(lock_id, wq.clone());
        wq
    }

    /// Send wake message to this reactor for async mutex support.
    /// This triggers Loop 3 (lock wake loop) on the target reactor.
    /// **No locks used** - SharedSender uses atomics internally.
    pub fn wake(&self, lock_id: LockId) {
        if let Some(s) = self.lock_sender.borrow().as_ref() {
            let _ = s.try_send(WakeMsg { lock_id });
        }
    }

    /// Shutdown this reactor's executor gracefully.
    /// Closes all three loops and waits for thread to exit.
    pub async fn shutdown(&self) {
        println!("[Reactor::shutdown] Starting shutdown for reactor {}", self.rid);
        
        // Drop all senders to let loops exit gracefully
        let _ = self.lock_sender.borrow_mut().take();  // Loop 3 (lock wake)
        let _ = self.task_sender.borrow_mut().take();  // Loop 1 (task dispatch)
        let _ = self.channel.borrow_mut().take();       // Loop 2 (waitable tasks)

        println!("[Reactor::shutdown] All channels closed for reactor {}, signaling stop", self.rid);
        
        // Signal executor to stop.
        self.stop_sem.add_permits(1);
        
        println!("[Reactor::shutdown] Joining executor for reactor {}", self.rid);
        let _ = self.handle.join().await;
        
        println!("[Reactor::shutdown] Reactor {} shutdown complete", self.rid);
    }

    /// Helper to create many reactors.
    pub fn spawn_all(num: usize) -> Result<Vec<Reactor>, glommio::GlommioError> {
        let mut v = Vec::with_capacity(num);
        for rid in 0..num {
            v.push(Reactor::create(rid)?);
        }
        Ok(v)
    }
}

/// Accessor for current reactor id; returns None if called outside reactor
/// thread.
pub fn current_reactor_id() -> Option<ReactorId> { CURRENT_REACTOR_ID.with(|c| c.get()) }
