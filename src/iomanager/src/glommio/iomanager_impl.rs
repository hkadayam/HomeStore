use std::future::Future;

use super::{current_reactor_id, Reactor, ReactorId};
use crate::iomanager::IOManagerImplTrait;

/// Glommio-specific implementation of IOManager backend.
/// This is a zero-sized type that provides static methods only.
pub struct IOManagerImpl;

impl IOManagerImplTrait for IOManagerImpl {
    fn spawn_reactors(num_reactors: usize) -> Result<Vec<Reactor>, &'static str> {
        // Reactors create their own channels internally!
        Reactor::spawn_all(num_reactors).map_err(|_| "executor spawn failed")
    }

    fn current_reactor_id() -> ReactorId { current_reactor_id().unwrap_or(0) }

    fn spawn_detached<F>(reactor: &Reactor, fut: F)
    where
        F: Future<Output = ()> + 'static + Send,
    {
        reactor.spawn_future(Box::pin(fut));
    }
    
    async fn spawn_waitable<F, R>(reactor: &Reactor, fut: F) -> R
    where
        F: Future<Output = R> + 'static + Send,
        R: Send + 'static,
    {
        let (result_tx, result_rx) = reactor.create_result_handle();
        
        Self::spawn_detached(reactor, async move {
            let result = fut.await;
            let _ = result_tx.send_result(result).await;
        });
        
        result_rx.wait().await
    }

    async fn spawn_waitable_all<F, Fut, R>(reactors: &[Reactor], f: F) -> Vec<R>
    where
        F: Fn(ReactorId) -> Fut,
        Fut: Future<Output = R> + 'static,
        R: Send + 'static,
    {
        let mut results = Vec::with_capacity(reactors.len());

        for (rid, reactor) in reactors.iter().enumerate() {
            let (result_tx, result_rx) = reactor.create_result_handle();
            let fut = f(rid);

            Self::spawn_detached(reactor, async move {
                let result = fut.await;
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
        // Use glommio's smart yielding - only yields if necessary
        glommio::yield_if_needed().await
    }

    fn run_test<F>(fut: F)
    where
        F: Future<Output = ()> + Send + 'static,
    {
        // Main thread flow (as specified):
        // 1. Main thread: iomgr() creates N reactor threads (already done by test macro)
        // 2. Main thread: spawn(fut) with completion signal
        // 3. Main thread: wait for completion
        // 4. Main thread: shutdown iomanager
        
        // Create glommio executor on main thread to run async operations
        glommio::LocalExecutorBuilder::new()
            .spawn(|| async move {
                // Step 1: Get iomanager (reactors already running)
                let io_mgr = crate::iomanager::iomgr();
                
                // Step 2: Create a shared_channel for completion notification
                // (Glommio's cross-executor communication mechanism)
                use glommio::channels::shared_channel;
                let (tx, rx) = shared_channel::new_bounded(1);
                
                // Spawn test on reactor 0 (fire-and-forget, but with completion signal)
                io_mgr.spawn_detached(crate::iomanager::ReactorTarget::Reactor(0), async move {
                    fut.await;
                    let _ = tx.try_send(()); // Signal completion
                });
                
                // Step 3: Wait for completion signal
                let _ = rx.recv().await;
                
                // Step 4: Shutdown iomanager
                let _ = crate::iomanager::shutdown_iomgr().await;
            })
            .expect("Failed to spawn glommio executor")
            .join()
            .expect("Failed to join glommio executor");
    }
}
