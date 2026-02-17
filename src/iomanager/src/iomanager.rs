use std::{
    any::Any,
    cell::RefCell,
    future::Future,
    mem::ManuallyDrop,
    pin::Pin,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc,
    },
    task::{Context, Poll},
};

#[cfg(feature = "glommio")]
use crate::glommio::{IOManagerImpl, LockId, Reactor, ReactorId, TaskCompletion};
#[cfg(feature = "tokio")]
use crate::tokio::{IOManagerImpl, LockId, Reactor, ReactorId, TaskCompletion};

// ═══════════════════════════════════════════════════════════════════════════
// Public Types
// ═══════════════════════════════════════════════════════════════════════════

/// Target reactor(s) for spawning tasks.
#[derive(Debug, Clone, Copy)]
pub enum ReactorTarget {
    /// Current reactor (panics if not on a reactor thread)
    Current,
    /// Specific reactor by ID
    Reactor(ReactorId),
    /// Any reactor (round-robin selection)
    Any,
    /// All reactors in parallel (only for spawn_detached)
    All,
}

/// Backend-specific implementation trait for IOManager.
/// Implementations must provide reactor spawning, task dispatch, and lifecycle
/// management.
pub trait IOManagerImplTrait {
    /// Spawn reactors with their internal channels.
    fn spawn_reactors(num_reactors: usize) -> Result<Vec<Reactor>, &'static str>;

    /// Get the current reactor ID, or 0 if not on a reactor thread.
    fn current_reactor_id() -> ReactorId;

    /// Spawn a fire-and-forget future on the given reactor (requires Send for cross-reactor).
    fn spawn_detached<F>(reactor: &Reactor, fut: F)
    where
        F: Future<Output = ()> + 'static + Send;

    /// Spawn a local (non-Send) future on the given reactor.
    #[allow(dead_code)]
    fn spawn_local<F>(reactor: &Reactor, fut: F)
    where
        F: Future<Output = ()> + 'static;

    /// Spawn a waitable future on the given reactor and return a Future for the result.
    fn spawn_waitable<F, R>(reactor: &Reactor, fut: F) -> impl Future<Output = R> + Send
    where
        F: Future<Output = R> + 'static + Send,
        R: Send + 'static;

    /// Execute a closure on each reactor sequentially, collecting results.
    fn spawn_waitable_all<F, Fut, R>(reactors: &[Reactor], f: F) -> impl Future<Output = Vec<R>> + Send
    where
        F: Fn(ReactorId) -> Fut + Send,
        Fut: Future<Output = R> + 'static + Send,
        R: Send + 'static;

    /// Shutdown all reactors gracefully.
    fn shutdown_reactors(reactors: &[Reactor]) -> impl Future<Output = Result<(), &'static str>> + Send;

    /// Yield control back to the executor to allow other tasks to run.
    fn yield_now() -> impl Future<Output = ()> + Send;

    /// Sleep for a duration (async)
    fn sleep(duration: std::time::Duration) -> impl Future<Output = ()> + Send;

    /// Run an async test using the backend-specific runtime.
    /// This blocks the current thread until the test completes.
    /// Includes shutdown at the end.
    fn run_test<F>(fut: F)
    where
        F: Future<Output = ()> + Send + 'static;
}

/// Unified IOManager with common functionality and backend-specific delegation.
pub struct IOManager {
    pub num_reactors: usize,
    pub reactors: Vec<Reactor>,
    lock_id_ctr: AtomicU64,
    shutting_down: AtomicU64, // 0 = running, 1 = shutting down
    spawn_rr: AtomicU64,      // round-robin counter for task scheduling
    drive_interface: Arc<crate::DriveInterface>,
}

#[allow(dead_code)]
impl IOManager {
    fn new(num_reactors: usize) -> Result<Self, &'static str> {
        let reactors = IOManagerImpl::spawn_reactors(num_reactors)?;

        Ok(Self {
            num_reactors,
            reactors,
            lock_id_ctr: AtomicU64::new(1),
            shutting_down: AtomicU64::new(0),
            spawn_rr: AtomicU64::new(0),
            drive_interface: Arc::new(crate::DriveInterface::new()),
        })
    }

    pub fn num_reactors(&self) -> usize { self.num_reactors }

    /// Get the shared DriveInterface for IO operations.
    pub fn drive_interface(&self) -> &Arc<crate::DriveInterface> { &self.drive_interface }

    pub fn alloc_lock_id(&self) -> u64 { self.lock_id_ctr.fetch_add(1, Ordering::Relaxed) }

    pub fn reactor(&self, rid: ReactorId) -> &Reactor { &self.reactors[rid] }

    /// Get next reactor ID using round-robin selection.
    pub fn next_reactor(&self) -> ReactorId {
        (self.spawn_rr.fetch_add(1, Ordering::Relaxed) as usize) % self.num_reactors
    }

    pub fn current_reactor(&self) -> Option<&Reactor> {
        let rid = IOManagerImpl::current_reactor_id();
        if rid < self.num_reactors { Some(&self.reactors[rid]) } else { None }
    }

    /// Return current reactor id if running on a reactor thread, else 0.
    pub fn current_reactor_id(&self) -> ReactorId { IOManagerImpl::current_reactor_id() }

    pub fn wake(&self, rid: ReactorId, lock_id: LockId) { self.reactors[rid].wake(lock_id); }

    /// Spawn a fire-and-forget task on the specified reactor.
    ///
    /// # Arguments
    /// * `target` - Which reactor to run the task on (Current, Reactor(id), or Any)
    /// * `fut` - The async task to execute (must return ())
    ///
    /// # Panics
    /// * If target is `ReactorTarget::All` (use BackgroundTasks for spawning on all reactors)
    ///
    /// # Examples
    /// ```ignore
    /// use iomgr::{iomanager, ReactorTarget};
    ///
    /// // Spawn on any reactor
    /// iomgr().spawn_detached(ReactorTarget::Any, async {
    ///     println!("Running on a reactor");
    /// });
    /// ```
    pub fn spawn_detached<F>(&self, target: ReactorTarget, fut: F)
    where
        F: Future<Output = ()> + Send + 'static,
    {
        if matches!(target, ReactorTarget::All) {
            panic!("spawn_detached doesn't support ReactorTarget::All - use BackgroundTasks::spawn_on_all");
        }

        let reactor_id = self.resolve_target(target);
        let reactor = self.reactor(reactor_id);
        IOManagerImpl::spawn_detached(reactor, fut);
    }

    /// Spawn a waitable async task that returns a result.
    ///
    /// This function can only be called from a reactor thread. It spawns work on the
    /// target reactor and returns a Future that can be awaited for the result.
    ///
    /// # Arguments
    /// * `target` - Which reactor to run the task on (not All)
    /// * `fut` - The async task to execute
    ///
    /// # Returns
    /// A Future that resolves to the result of the task
    ///
    /// # Panics
    /// * If called from a non-reactor thread
    /// * If target is `ReactorTarget::All`
    ///
    /// # Examples
    /// ```ignore
    /// use iomgr::{iomanager, ReactorTarget};
    ///
    /// async fn my_reactor_function() {
    ///     let result = iomgr().spawn_waitable(ReactorTarget::Any, async {
    ///         expensive_computation().await
    ///     }).await;
    ///     
    ///     println!("Result: {}", result);
    /// }
    /// ```
    pub fn spawn_waitable<F, R>(&self, target: ReactorTarget, fut: F) -> impl Future<Output = R> + Send + 'static
    where
        F: Future<Output = R> + Send + 'static,
        R: Send + 'static,
    {
        if matches!(target, ReactorTarget::All) {
            panic!("spawn_waitable doesn't support ReactorTarget::All");
        }

        let calling_reactor_id = IOManagerImpl::current_reactor_id();
        if calling_reactor_id >= self.num_reactors() {
            panic!("spawn_waitable can only be called from a reactor thread");
        }

        let target_reactor_id = self.resolve_target(target);

        // We need to return a 'static future, so we use the reactor ID and re-fetch from IOManager
        async move {
            let target_reactor = iomgr().reactor(target_reactor_id);
            IOManagerImpl::spawn_waitable(target_reactor, fut).await
        }
    }

    /// Spawn an async task on any reactor (round-robin) - std::thread::spawn compatible.
    ///
    /// This is the primary way to spawn tasks from reactor threads. It automatically
    /// selects a reactor using round-robin and returns a handle that can be awaited.
    ///
    /// # Arguments
    /// * `fut` - The async task to execute
    ///
    /// # Returns
    /// A JoinHandle that can be awaited for the result
    ///
    /// # Panics
    /// * If called from a non-reactor thread
    ///
    /// # Examples
    /// ```ignore
    /// use iomgr::iomanager;
    ///
    /// async fn my_function() {
    ///     let handle = iomgr().spawn(async {
    ///         expensive_work().await;
    ///         42
    ///     });
    ///     
    ///     let result = handle.await;
    ///     assert_eq!(result, 42);
    /// }
    /// ```
    pub fn spawn<F, R>(&self, fut: F) -> JoinHandle<R>
    where
        F: Future<Output = R> + Send + 'static,
        R: Send + 'static,
    {
        let target_reactor_id = self.next_reactor();

        // Use crossbeam channel - works for both .await and .join()
        let (tx, rx) = crossbeam::channel::bounded(1);

        // Spawn the work on target reactor
        let target_reactor = self.reactor(target_reactor_id);
        IOManagerImpl::spawn_detached(target_reactor, async move {
            let result = fut.await;
            let _ = tx.send(result);
        });

        JoinHandle {
            receiver: Some(rx),
            reactor_id: target_reactor_id,
            detached: AtomicBool::new(false),
        }
    }

    /// Spawn a waitable task on all reactors sequentially, collecting results.
    ///
    /// The provided closure is called once for each reactor with its reactor_id.
    /// Each future runs sequentially (waits for previous to complete before starting next).
    ///
    /// # Arguments
    /// * `f` - Closure that takes reactor_id and returns a Future
    ///
    /// # Returns
    /// A Future that resolves to Vec<R> with results from all reactors
    ///
    /// # Examples
    /// ```ignore
    /// use iomgr::iomanager;
    ///
    /// async fn my_function() {
    ///     let results = iomgr().spawn_waitable_all(|reactor_id| async move {
    ///         compute_on_reactor(reactor_id).await
    ///     }).await;
    ///     
    ///     println!("Got {} results", results.len());
    /// }
    /// ```
    pub async fn spawn_waitable_all<F, Fut, R>(&self, f: F) -> Vec<R>
    where
        F: Fn(ReactorId) -> Fut + Send,
        Fut: Future<Output = R> + 'static + Send,
        R: Send + 'static,
    {
        IOManagerImpl::spawn_waitable_all(&self.reactors, f).await
    }

    /// Resolve ReactorTarget to a specific reactor ID.
    fn resolve_target(&self, target: ReactorTarget) -> ReactorId {
        match target {
            ReactorTarget::Current => IOManagerImpl::current_reactor_id(),
            ReactorTarget::Reactor(id) => {
                assert!(id < self.num_reactors(), "Invalid reactor ID: {}", id);
                id
            }
            ReactorTarget::Any => self.next_reactor(),
            ReactorTarget::All => {
                panic!("resolve_target called with ReactorTarget::All - handle separately")
            }
        }
    }

    pub async fn shutdown(&self) -> Result<(), &'static str> {
        if self.shutting_down.compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst).is_err() {
            return Err("Shutdown already in progress or completed");
        }
        IOManagerImpl::shutdown_reactors(&self.reactors).await
    }

    /// Yield control back to the executor to allow other tasks to run.
    ///
    /// This method allows long-running operations to periodically yield control
    /// back to the executor, preventing starvation of other tasks.
    ///
    /// # Backend Behavior
    ///
    /// - **glommio**: Uses `yield_if_needed()` - smart yielding that only yields if the task has been running for a
    ///   while and other tasks are waiting
    /// - **tokio**: Uses `yield_now()` - unconditional yielding
    ///
    /// # Example
    ///
    /// ```rust,no_run
    /// # use iomanager::iomanager;
    /// # async fn example() {
    /// for i in 0..1_000_000 {
    ///     // Do some work
    ///     process_item(i);
    ///
    ///     // Yield every 1000 iterations to prevent blocking other tasks
    ///     if i % 1000 == 0 {
    ///         iomgr().yield_now().await;
    ///     }
    /// }
    /// # fn process_item(i: i32) {}
    /// # }
    /// ```
    pub async fn yield_now(&self) { IOManagerImpl::yield_now().await }
}

impl Drop for IOManager {
    fn drop(&mut self) {
        if self.shutting_down.load(Ordering::Relaxed) == 0 {
            // IOManager is being dropped without shutdown being called!
            // This is a bug - reactors will become zombie threads
            // Log error to stderr for immediate visibility
            eprintln!("ERROR: IOManager dropped without calling shutdown()! Reactor threads will become zombies.");
            eprintln!("       This indicates a panic or early return in shutdown_iomgr()");
        }
    }
}

// IOManager Global Instance
//
// PRODUCTION: Uses `static mut` for zero-overhead access after init.
// TESTS: Uses `OnceLock` for thread-safe concurrent test execution.
//
// This is safe because:
// - Production: IOManager is initialized once at startup, never mutated
// - Tests: OnceLock provides proper synchronization


#[cfg(any(test, feature = "test-mode"))]
static IO_MANAGER: parking_lot::RwLock<Option<Box<IOManager>>> =
    parking_lot::RwLock::const_new(<parking_lot::RawRwLock as parking_lot::lock_api::RawRwLock>::INIT, None);

#[cfg(any(test, feature = "test-mode"))]
static INIT_COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

#[cfg(not(any(test, feature = "test-mode")))]
static mut IO_MANAGER: Option<IOManager> = None;

pub fn init_iomgr(num_reactors: usize) -> Result<(), &'static str> {
    #[cfg(any(test, feature = "test-mode"))]
    {
        use std::sync::atomic::Ordering;

        // Try fast path with read lock to prevent TOCTOU race with shutdown
        {
            let guard = IO_MANAGER.read();
            let count_check = INIT_COUNT.load(Ordering::Acquire);
            let has_manager = guard.is_some();
            if count_check > 0 && has_manager {
                let _ = INIT_COUNT.fetch_add(1, Ordering::SeqCst) + 1;
                return Ok(());
            }
        } // Release read lock

        // Slow path: acquire write lock and check if we're first
        let mut guard = IO_MANAGER.write();
        let prev = INIT_COUNT.fetch_add(1, Ordering::SeqCst);
        if prev == 0 || guard.is_none() {
            // We're first OR IOManager was destroyed in previous shutdown - (re)create it
            *guard = Some(Box::new(IOManager::new(num_reactors)?));
        }
        // else: someone beat us, they already created it

        Ok(())
    }

    #[cfg(not(any(test, feature = "test-mode")))]
    #[allow(static_mut_refs)]
    unsafe {
        if IO_MANAGER.is_some() {
            return Err("IOManager already initialized");
        }
        IO_MANAGER = Some(IOManager::new(num_reactors)?);
        Ok(())
    }
}

pub fn iomgr() -> &'static IOManager {
    #[cfg(any(test, feature = "test-mode"))]
    {
        use std::sync::atomic::Ordering;

        let count = INIT_COUNT.load(Ordering::Acquire);
        assert!(count > 0, "IOManager not initialized. Call init_iomgr() first.");

        let guard = IO_MANAGER.read();
        let mgr_ref = guard.as_ref().expect("IOManager missing despite count > 0");

        // Safety: count > 0 means IOManager is alive
        // Tests are disciplined: they finish using it before shutdown
        unsafe { std::mem::transmute::<&IOManager, &'static IOManager>(mgr_ref.as_ref()) }
    }

    #[cfg(not(any(test, feature = "test-mode")))]
    #[allow(static_mut_refs)]
    unsafe {
        IO_MANAGER.as_ref().expect("IOManager not initialized")
    }
}

#[allow(unused_variables)]
#[allow(dead_code)]
pub async fn restart(num_reactors: usize) -> Result<(), &'static str> {
    #[cfg(any(test, feature = "test-mode"))]
    {
        // Can't restart with OnceLock - would need to add this capability
        Err("restart not supported in test builds")
    }

    #[cfg(not(any(test, feature = "test-mode")))]
    unsafe {
        // Shutdown existing
        if let Some(ref mgr) = IO_MANAGER {
            mgr.shutdown().await?;
        }
        // Drop happens automatically when we replace
        IO_MANAGER = Some(IOManager::new(num_reactors)?);
        Ok(())
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Clean Public API - Convenience Functions
// ═══════════════════════════════════════════════════════════════════════════

/// Spawn a fire-and-forget task on the specified reactor.
///
/// Convenience function that calls `iomgr().spawn_detached()`.
///
/// # Examples
/// ```ignore
/// use iomgr::{spawn_detached, ReactorTarget};
///
/// spawn_detached(ReactorTarget::Any, async {
///     println!("Running on a reactor");
/// });
/// ```
pub fn spawn_detached<F>(target: ReactorTarget, fut: F)
where
    F: Future<Output = ()> + Send + 'static,
{
    iomgr().spawn_detached(target, fut)
}

/// Spawn a local (non-Send) fire-and-forget future on the current reactor.
///
/// This allows spawning futures that don't implement `Send`, such as those
/// holding references to `RwLockReadGuard` or other !Send types. The future
/// will execute on the current reactor thread and never move to another thread.
///
/// # Panics
/// Panics if called from a non-reactor thread.
#[allow(dead_code)]
pub fn spawn_local<F>(fut: F)
where
    F: Future<Output = ()> + 'static,
{
    let reactor_id = IOManagerImpl::current_reactor_id();
    if reactor_id >= iomgr().num_reactors() {
        panic!("spawn_local() called from non-reactor thread");
    }
    let reactor = iomgr().reactor(reactor_id);
    IOManagerImpl::spawn_local(reactor, fut);
}

/// Spawn a waitable async task that returns a result.
///
/// Convenience function that calls `iomgr().spawn_waitable()`.
///
/// # Examples
/// ```ignore
/// use iomgr::{spawn_waitable, ReactorTarget};
///
/// async fn my_reactor_function() {
///     let result = spawn_waitable(ReactorTarget::Any, async {
///         expensive_computation().await
///     }).await;
/// }
/// ```
pub fn spawn_waitable<F, R>(target: ReactorTarget, fut: F) -> impl Future<Output = R> + Send + 'static
where
    F: Future<Output = R> + Send + 'static,
    R: Send + 'static,
{
    iomgr().spawn_waitable(target, fut)
}

/// JoinHandle for spawned tasks - compatible with std::thread::JoinHandle.
///
/// This handle allows you to wait for the result of a spawned task in multiple ways:
/// - `.await` - Async wait (non-blocking, for use in async contexts)
/// - `.join()` - Blocking wait (for use in sync contexts)
/// - `.is_finished()` - Check if task completed without blocking
/// - `.detach()` - Consume handle without waiting
/// - `.reactor_id()` - Get the ID of the reactor running the task
///
/// # Examples
/// ```ignore
/// // Async wait
/// let handle = spawn(async { 42 });
/// let result = handle.await;
///
/// // Blocking wait (from non-reactor thread)
/// let handle = spawn(async { 42 });
/// let result = handle.join();
///
/// // Check and poll
/// let mut handle = spawn(async { expensive_work().await });
/// while !handle.is_finished() {
///     do_other_work();
/// }
/// let result = handle.join();
///
/// // Fire and forget
/// let handle = spawn(async { background_work().await });
/// handle.detach();
/// ```
pub struct JoinHandle<R> {
    // Crossbeam channel receiver - works for both .await and .join()
    receiver: Option<crossbeam::channel::Receiver<R>>,
    reactor_id: ReactorId,
    detached: AtomicBool,
}

impl<R: Send + 'static> JoinHandle<R> {
    /// Block the current thread and wait for the task result.
    ///
    /// This is similar to `std::thread::JoinHandle::join()`. It blocks the calling thread
    /// until the spawned task completes. Uses crossbeam channel internally so it works
    /// from any thread without needing an executor.
    ///
    /// # Panics
    /// - If called from a reactor thread (use `.await` instead)
    ///
    /// # Examples
    /// ```ignore
    /// fn main() {
    ///     iomgr::init_iomgr(4);
    ///     let handle = iomanager::spawn(async { 42 });
    ///     let result = handle.join();  // Blocks until complete
    ///     assert_eq!(result, 42);
    /// }
    /// ```
    pub fn join(self) -> R {
        if IOManagerImpl::current_reactor_id() < iomgr().num_reactors() {
            panic!("JoinHandle::join() called from reactor thread - use .await instead");
        }

        // Use ManuallyDrop to prevent Drop from running
        let handle = ManuallyDrop::new(self);

        // Mark as detached since we're consuming via .join()
        handle.detached.store(true, Ordering::Release);

        // Safety: We're consuming the handle and won't use it again
        let receiver = unsafe { std::ptr::read(&handle.receiver) };

        // Block on crossbeam channel - works from any non-reactor thread
        receiver.expect("JoinHandle already consumed").recv().expect("Task channel disconnected")
    }

    /// Get the reactor ID where this task is running.
    ///
    /// This is analogous to `std::thread::JoinHandle::thread()`.
    ///
    /// # Examples
    /// ```ignore
    /// let handle = spawn(async { work().await });
    /// println!("Task running on reactor {}", handle.reactor_id());
    /// ```
    pub fn reactor_id(&self) -> ReactorId { self.reactor_id }

    /// Check if the task has finished without blocking.
    ///
    /// This is analogous to `std::thread::JoinHandle::is_finished()`.
    ///
    /// # Examples
    /// ```ignore
    /// let handle = spawn(async { expensive_work().await });
    ///
    /// while !handle.is_finished() {
    ///     // Do other work while waiting
    ///     do_something_else();
    /// }
    ///
    /// let result = handle.join();  // Won't block, already finished
    /// ```
    pub fn is_finished(&self) -> bool {
        if let Some(ref rx) = self.receiver {
            // Check if result is ready without blocking
            !rx.is_empty()
        } else {
            false
        }
    }

    /// Detach the task, allowing it to run in the background without waiting.
    ///
    /// This consumes the handle without waiting for the result. The task continues
    /// to run in the background.
    ///
    /// This is analogous to dropping a `std::thread::JoinHandle` without calling `join()`,
    /// but more explicit.
    ///
    /// # Examples
    /// ```ignore
    /// let handle = spawn(async {
    ///     background_cleanup().await;
    /// });
    /// handle.detach();  // Task runs in background, we don't wait for it
    /// ```
    pub fn detach(self) {
        self.detached.store(true, Ordering::Release);
        // Just drop the handle
    }
}

impl<R: Send + 'static> Future for JoinHandle<R> {
    type Output = R;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<R> {
        if let Some(ref rx) = self.receiver {
            // Try to receive result without blocking
            match rx.try_recv() {
                Ok(result) => {
                    // Mark as detached since we're consuming via .await
                    self.detached.store(true, Ordering::Release);
                    Poll::Ready(result)
                }
                Err(crossbeam::channel::TryRecvError::Empty) => {
                    // Not ready yet - wake to poll again
                    // Note: crossbeam doesn't natively support wakers, so this will
                    // cause polling until the result arrives. For better efficiency,
                    // use spawn_waitable() directly which uses ResultChannel with proper waker support.
                    cx.waker().clone().wake();
                    Poll::Pending
                }
                Err(crossbeam::channel::TryRecvError::Disconnected) => {
                    panic!("JoinHandle: task channel disconnected before result received")
                }
            }
        } else {
            panic!("JoinHandle already consumed")
        }
    }
}

impl<R> Drop for JoinHandle<R> {
    fn drop(&mut self) {
        if !self.detached.load(Ordering::Acquire) && !std::thread::panicking() {
            // Warn if handle is dropped without being awaited or detached
            // This mirrors std::thread::JoinHandle behavior
            eprintln!("Warning: JoinHandle dropped without being awaited or detached");
        }
    }
}

/// Spawn an async task on any reactor (round-robin) - std::thread::spawn compatible.
///
/// Convenience function that calls `iomgr().spawn()`.
///
/// # Examples
/// ```ignore
/// use iomgr::spawn;
///
/// async fn my_function() {
///     let handle = spawn(async {
///         expensive_work().await;
///         42
///     });
///     
///     let result = handle.await;
/// }
/// ```
pub fn spawn<F, R>(fut: F) -> JoinHandle<R>
where
    F: Future<Output = R> + Send + 'static,
    R: Send + 'static,
{
    iomgr().spawn(fut)
}

/// Spawn a waitable task on all reactors sequentially, collecting results.
///
/// Convenience function that calls `iomgr().spawn_waitable_all()`.
///
/// # Examples
/// ```ignore
/// use iomgr::spawn_waitable_all;
///
/// async fn my_function() {
///     let results = spawn_waitable_all(|reactor_id| async move {
///         compute_on_reactor(reactor_id).await
///     }).await;
/// }
/// ```
pub async fn spawn_waitable_all<F, Fut, R>(f: F) -> Vec<R>
where
    F: Fn(ReactorId) -> Fut + Send,
    Fut: Future<Output = R> + 'static + Send,
    R: Send + 'static,
{
    iomgr().spawn_waitable_all(f).await
}

/// Spawn a task on a reactor and block the current thread until completion.
///
/// This function is designed for **non-reactor threads** (e.g., main thread, sync worker threads)
/// that need to call into the async reactor system and wait for the result synchronously.
///
/// Uses a thread-local crossbeam channel that is cached and reused across multiple calls
/// from the same thread, making it efficient for critical path usage.
///
/// # Panics
/// - If called from a reactor thread (use `spawn_waitable()` instead for async contexts)
/// - If target is `ReactorTarget::Current` (no current reactor for non-reactor threads)
/// - If target is `ReactorTarget::All` (use `spawn_waitable_all()` for multiple reactors)
///
/// # Examples
/// ```ignore
/// use iomgr::{spawn_and_block, ReactorTarget};
///
/// fn main() {
///     iomanager::init_iomanager(4);
///     
///     // Block until result is ready
///     let result: i32 = spawn_and_block(ReactorTarget::Any, async {
///         expensive_computation().await;
///         42
///     });
///     
///     println!("Result: {}", result);
/// }
/// ```
pub fn spawn_and_block<F, R>(target: ReactorTarget, fut: F) -> R
where
    F: Future<Output = R> + Send + 'static,
    R: Send + 'static,
{
    // Thread-local channel - created once per thread, reused across calls
    thread_local! {
        static CHANNEL: RefCell<Option<(
            crossbeam::channel::Sender<Box<dyn Any + Send>>,
            crossbeam::channel::Receiver<Box<dyn Any + Send>>
        )>> = RefCell::new(None);
    }

    // Panic if called from reactor thread
    let current_id = IOManagerImpl::current_reactor_id();
    if current_id < iomgr().num_reactors() {
        panic!("spawn_and_block() cannot be called from reactor thread - use spawn_waitable() instead");
    }

    CHANNEL.with(|cell| {
        let mut opt = cell.borrow_mut();

        // Create channel on first use for this thread
        if opt.is_none() {
            *opt = Some(crossbeam::channel::bounded(1));
        }

        let (tx, rx) = opt.as_ref().unwrap();
        let tx_clone = tx.clone();

        // Resolve target reactor
        let reactor = match target {
            ReactorTarget::Current => {
                panic!("spawn_and_block() called from non-reactor thread - ReactorTarget::Current is invalid")
            }
            ReactorTarget::Any => iomgr().reactor(iomgr().next_reactor()),
            ReactorTarget::Reactor(id) => iomgr().reactor(id),
            ReactorTarget::All => {
                panic!("spawn_and_block() doesn't support ReactorTarget::All - use spawn_waitable_all() instead")
            }
        };

        // Spawn task on target reactor
        IOManagerImpl::spawn_detached(reactor, async move {
            let result = fut.await;
            // Send result via crossbeam channel (works from async context)
            let _ = tx_clone.send(Box::new(result) as Box<dyn Any + Send>);
        });

        // Block current thread waiting for result
        let result_box = rx.recv().expect("spawn_and_block: channel disconnected - reactor may have shut down");

        // Downcast to concrete type
        *result_box.downcast::<R>().expect("spawn_and_block: type mismatch in result")
    })
}

pub async fn shutdown_iomgr() -> Result<(), &'static str> {
    #[cfg(any(test, feature = "test-mode"))]
    {
        use std::sync::atomic::Ordering;

        // Acquire write lock first to serialize with init
        let mut guard = IO_MANAGER.write();
        let prev = INIT_COUNT.fetch_sub(1, Ordering::SeqCst);

        assert!(prev > 0, "Shutdown called more times than init");

        if prev == 1 {
            // Last shutdown - destroy IOManager
            let mgr = guard.take().ok_or("IOManager already destroyed")?;
            mgr.shutdown().await?;
            drop(guard);
        }
        // else: count > 1, just decremented, keep IOManager alive

        Ok(())
    }

    #[cfg(not(any(test, feature = "test-mode")))]
    {
        iomgr().shutdown().await
    }
}

#[allow(dead_code)]
/// Sleep for a duration (async, runtime-agnostic)
pub async fn sleep(duration: std::time::Duration) { IOManagerImpl::sleep(duration).await }

/// Run an async test using the iomanager runtime.
/// This is a helper function for tests that blocks the current thread until the async test completes.
/// Includes shutdown at the end.
pub fn run_test<F>(fut: F)
where
    F: Future<Output = ()> + Send + 'static,
{
    IOManagerImpl::run_test(fut);
}

// ═══════════════════════════════════════════════════════════════════════════
// Background Tasks
// ═══════════════════════════════════════════════════════════════════════════

/// Manager for background tasks that can be spawned on reactors and joined later.
///
/// Use this when you need to:
/// - Spawn multiple tasks across reactors
/// - Wait for all of them to complete
/// - Not care about individual task results (fire-and-forget with join)
///
/// # Example
/// ```ignore
/// use iomgr::{BackgroundTasks, ReactorTarget};
///
/// let bg_tasks = BackgroundTasks::new();
///
/// // Spawn on random reactors (round-robin)
/// bg_tasks.spawn(ReactorTarget::Any, async { do_work().await });
///
/// // Spawn on current reactor
/// bg_tasks.spawn(ReactorTarget::Current, async { do_local_work().await });
///
/// // Spawn on specific reactor by ID
/// bg_tasks.spawn(ReactorTarget::Reactor(0), async { do_work_on_reactor_0().await });
///
/// // Wait for all tasks to complete
/// bg_tasks.join_all().await;
/// ```
pub struct BackgroundTasks {
    completion: Arc<TaskCompletion>,
}

impl BackgroundTasks {
    /// Create a new background task manager.
    pub fn new() -> Self {
        Self {
            completion: Arc::new(TaskCompletion::new()),
        }
    }

    /// Spawn a fire-and-forget background task on the specified reactor(s).
    ///
    /// # Arguments
    /// * `target` - Where to run the task (Current, Reactor(id), or Any)
    /// * `fut` - The async task to execute
    ///
    /// # Examples
    /// ```ignore
    /// use iomgr::{BackgroundTasks, ReactorTarget};
    ///
    /// let bg_tasks = BackgroundTasks::new();
    ///
    /// // Spawn on any reactor (round-robin)
    /// bg_tasks.spawn(ReactorTarget::Any, async { work().await });
    ///
    /// // Spawn on current reactor
    /// bg_tasks.spawn(ReactorTarget::Current, async { local_work().await });
    ///
    /// // Spawn on specific reactor
    /// bg_tasks.spawn(ReactorTarget::Reactor(2), async { work_on_2().await });
    ///
    /// bg_tasks.join_all().await;
    /// ```
    pub fn spawn<F>(&self, target: ReactorTarget, fut: F)
    where
        F: Future<Output = ()> + 'static + Send,
    {
        if matches!(target, ReactorTarget::All) {
            panic!("BackgroundTasks::spawn doesn't support ReactorTarget::All - use spawn_on_all");
        }

        let io_mgr = iomgr();
        let reactor_id = match target {
            ReactorTarget::Current => IOManagerImpl::current_reactor_id(),
            ReactorTarget::Reactor(id) => {
                assert!(id < io_mgr.num_reactors(), "Invalid reactor ID: {}", id);
                id
            }
            ReactorTarget::Any => io_mgr.next_reactor(),
            ReactorTarget::All => unreachable!(),
        };

        let reactor = io_mgr.reactor(reactor_id);
        self.spawn_on(reactor, fut);
    }

    /// Spawn a fire-and-forget task on all reactors in parallel.
    ///
    /// The provided closure is called once for each reactor with its reactor_id,
    /// and all resulting futures run concurrently across reactors.
    ///
    /// # Example
    /// ```ignore
    /// use iomgr::BackgroundTasks;
    ///
    /// let bg_tasks = BackgroundTasks::new();
    /// bg_tasks.spawn_on_all(|reactor_id| async move {
    ///     println!("Running on reactor {}", reactor_id);
    ///     do_work_on_reactor(reactor_id).await;
    /// });
    /// bg_tasks.join_all().await; // Waits for all reactors to complete
    /// ```
    pub fn spawn_on_all<F, Fut>(&self, f: F)
    where
        F: Fn(ReactorId) -> Fut + Send,
        Fut: Future<Output = ()> + 'static + Send,
    {
        let io_mgr = iomgr();
        for reactor_id in 0..io_mgr.num_reactors() {
            let fut = f(reactor_id);
            self.spawn(ReactorTarget::Reactor(reactor_id), fut);
        }
    }

    /// Internal helper to spawn on a specific reactor reference.
    fn spawn_on<F>(&self, reactor: &Reactor, fut: F)
    where
        F: Future<Output = ()> + 'static + Send,
    {
        // Mark task as started before spawning
        self.completion.task_started();

        // Clone the completion tracker for the spawned task
        let completion = self.completion.clone();

        // Spawn the task with completion tracking
        reactor.spawn_future(Box::pin(async move {
            fut.await;
            completion.task_completed();
        }));
    }

    /// Wait for all spawned tasks to complete.
    ///
    /// This will block until all tasks spawned via `spawn()`, `spawn_local()`,
    /// or `spawn_on_reactor()` have finished. Returns immediately if no tasks are running.
    pub async fn join_all(&self) { self.completion.wait_all().await; }
}

impl Default for BackgroundTasks {
    fn default() -> Self { Self::new() }
}
