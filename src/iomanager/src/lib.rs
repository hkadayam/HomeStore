mod iomanager;
mod reactor_local;

#[cfg(feature = "glommio")]
mod glommio;
#[cfg(feature = "tokio")]
mod tokio;

// Export backend-specific types
#[cfg(feature = "glommio")]
pub use crate::glommio::{
    current_reactor_id, AsyncMutex, AsyncMutexGuard, AsyncRwLock, AsyncRwReadGuard, AsyncRwWriteGuard, DriveInterface,
    IOBuffer, IoDevice, LockId, Reactor, ReactorId, WakeMsg,
};
pub use crate::iomanager::{
    init_iomgr, iomgr, run_test, shutdown_iomgr,
    BackgroundTasks, IOManager, ReactorTarget, JoinHandle,
    spawn, spawn_and_block, spawn_detached, spawn_waitable, spawn_waitable_all,
    sleep,
};
pub use crate::reactor_local::ReactorLocal;
pub use iomanager_macros::iomanager_test;
#[cfg(feature = "tokio")]
pub use crate::tokio::{
    current_reactor_id, AsyncMutex, AsyncMutexGuard, AsyncRwLock, AsyncRwReadGuard, AsyncRwWriteGuard, DriveInterface,
    IOBuffer, IoDevice, LockId, Reactor, ReactorId,
};
