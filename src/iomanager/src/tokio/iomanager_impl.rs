use std::future::Future;

use super::{Reactor, ReactorId};
use crate::iomanager::IOManagerImplTrait;

/// Tokio-specific implementation of IOManager backend.
/// This is a zero-sized type that provides static methods only.
pub struct IOManagerImpl;

impl IOManagerImplTrait for IOManagerImpl {
    fn spawn_reactors(num_reactors: usize) -> Result<Vec<Reactor>, &'static str> {
        // Reactors create their own channels internally!
        Ok(Reactor::spawn_all(num_reactors))
    }

    fn current_reactor_id() -> ReactorId {
        // Check if we're on a reactor thread
        if super::reactor::is_reactor_thread() {
            0  // Tokio doesn't have thread affinity, but mark as reactor thread
        } else {
            usize::MAX  // Not on a reactor thread
        }
    }

    fn spawn_detached<F>(reactor: &Reactor, fut: F)
    where
        F: Future<Output = ()> + 'static + Send,
    {
        reactor.spawn_future(Box::pin(fut));
    }
    
    fn spawn_local<F>(reactor: &Reactor, fut: F)
    where
        F: Future<Output = ()> + 'static,
    {
        reactor.spawn_local_future(Box::pin(fut));
    }

    async fn spawn_waitable<F, R>(reactor: &Reactor, fut: F) -> R
    where
        F: Future<Output = R> + 'static + Send,
        R: Send + 'static,
    {
        let (result_tx, result_rx) = reactor.create_result_handle();
        
        reactor.spawn_future(Box::pin(async move {
            let result = fut.await;
            let _ = result_tx.send_result(result).await;
        }));
        
        result_rx.wait().await
    }

    async fn spawn_waitable_all<F, Fut, R>(reactors: &[Reactor], f: F) -> Vec<R>
    where
        F: Fn(ReactorId) -> Fut + Send,
        Fut: Future<Output = R> + 'static + Send,
        R: Send + 'static,
    {
        let mut results = Vec::with_capacity(reactors.len());
        
        for (rid, reactor) in reactors.iter().enumerate() {
            let (result_tx, result_rx) = reactor.create_result_handle();
            let fut = f(rid);
            
            // Spawn on the specific reactor
            Self::spawn_detached(reactor, async move {
                let result = fut.await;  // Runs on reactor rid
                let _ = result_tx.send_result(result).await;
            });
            
            // Await result from this reactor before proceeding to next
            let result = result_rx.wait().await;
            results.push(result);
        }
        
        results
    }

    async fn shutdown_reactors(reactors: &[Reactor]) -> Result<(), &'static str> {
        for r in reactors {
            r.shutdown().await;
        }
        Ok(())
    }

    async fn yield_now() {
        // Use tokio's unconditional yielding
        tokio::task::yield_now().await
    }

    async fn sleep(duration: std::time::Duration) {
        tokio::time::sleep(duration).await
    }

    fn run_test<F>(fut: F)
    where
        F: Future<Output = ()> + Send + 'static,
    {
        // Main thread flow (as specified):
        // 1. Main thread: iomgr() creates N reactor threads (already done by test macro)
        // 2. Main thread: spawn(fut) on a reactor
        // 3. Main thread: wait for completion
        // 4. Main thread: shutdown iomanager
        
        // Create tokio runtime on main thread to run async operations
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("Failed to create tokio runtime");
        
        rt.block_on(async {
            // Step 1: Get iomanager (reactors already running)
            let io_mgr = crate::iomanager::iomgr();
            
            // Step 2: Create a oneshot channel for completion notification
            let (tx, rx) = tokio::sync::oneshot::channel();
            
            // Spawn test on reactor 0 (fire-and-forget, but with completion signal)
            io_mgr.spawn_detached(crate::iomanager::ReactorTarget::Reactor(0), async move {
                fut.await;
                let _ = tx.send(()); // Signal completion
            });
            
            // Step 3: Wait for completion signal
            let _ = rx.await;
            
            // Step 4: Shutdown iomanager
            let _ = crate::iomanager::shutdown_iomgr().await;
        });
    }
}
