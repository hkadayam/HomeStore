use std::cell::RefCell;
use std::collections::VecDeque;
use std::future::Future;

use glommio::channels::shared_channel;

use super::{current_reactor_id, Reactor, ReactorId};
use crate::iomanager::IOManagerImplTrait;

type BoxAny = Box<dyn std::any::Any + Send + 'static>;
type WaiterPair = (
    shared_channel::SharedSender<BoxAny>,
    shared_channel::SharedReceiver<BoxAny>,
);

// Per-thread pool of reusable shared_channel::new_bounded(1) pairs.
// Hot path: pop from pool (zero allocation).
// Pool grows on demand when concurrency exceeds current capacity.
// Works from any glommio async context.
thread_local! {
    static WAITER_POOL: RefCell<VecDeque<WaiterPair>> = RefCell::new(VecDeque::new());
}

fn acquire_waiter() -> WaiterPair {
    WAITER_POOL.with(|p| p.borrow_mut().pop_front())
        .unwrap_or_else(|| shared_channel::new_bounded(1))
}

fn release_waiter(pair: WaiterPair) {
    WAITER_POOL.with(|p| p.borrow_mut().push_back(pair));
}

/// Glommio-specific implementation of IOManager backend.
pub struct IOManagerImpl;

impl IOManagerImplTrait for IOManagerImpl {
    fn spawn_reactors(num_reactors: usize) -> Result<Vec<Reactor>, &'static str> {
        Reactor::spawn_all(num_reactors).map_err(|_| "executor spawn failed")
    }

    fn current_reactor_id() -> ReactorId { current_reactor_id().unwrap_or(usize::MAX) }

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
        let (tx, rx) = acquire_waiter();
        let tx_clone = tx.clone();

        Self::spawn_detached(reactor, async move {
            let result = fut.await;
            // try_send always succeeds: slot is empty (just popped from pool)
            let _ = tx_clone.try_send(Box::new(result) as BoxAny);
        });

        let result_box = rx.recv().await
            .expect("spawn_waitable: reactor dropped before sending result");

        release_waiter((tx, rx));

        *result_box.downcast::<R>().expect("spawn_waitable: type mismatch")
    }

    async fn spawn_waitable_all<F, Fut, R>(reactors: &[Reactor], f: F) -> Vec<R>
    where
        F: Fn(ReactorId) -> Fut,
        Fut: Future<Output = R> + 'static,
        R: Send + 'static,
    {
        let mut results = Vec::with_capacity(reactors.len());
        for (rid, reactor) in reactors.iter().enumerate() {
            let result = Self::spawn_waitable(reactor, f(rid)).await;
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
        glommio::yield_if_needed().await
    }

    fn run_test<F>(fut: F)
    where
        F: Future<Output = ()> + Send + 'static,
    {
        glommio::LocalExecutorBuilder::new()
            .spawn(|| async move {
                let io_mgr = crate::iomanager::iomgr();

                use glommio::channels::shared_channel;
                let (tx, rx) = shared_channel::new_bounded(1);

                io_mgr.spawn_detached(crate::iomanager::ReactorTarget::Reactor(0), async move {
                    fut.await;
                    let _ = tx.try_send(());
                });

                let _ = rx.recv().await;
                let _ = crate::iomanager::shutdown_iomgr().await;
            })
            .expect("Failed to spawn glommio executor")
            .join()
            .expect("Failed to join glommio executor");
    }
}
