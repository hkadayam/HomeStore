use std::{
    cell::UnsafeCell,
    ops::{Deref, DerefMut},
    sync::atomic::{AtomicU64, Ordering},
};

use super::current_reactor_id;
use crate::{iomanager::iomanager, LockId, ReactorId};

#[inline]
fn lock_bit() -> u64 { 1 }
#[inline]
fn waiter_mask(rid: ReactorId) -> u64 { 1u64 << (1 + rid) }

pub struct AsyncMutex<T> {
    lockword: AtomicU64,
    data: UnsafeCell<T>,
    lock_id: LockId,
}

unsafe impl<T: Send> Send for AsyncMutex<T> {}
unsafe impl<T: Send> Sync for AsyncMutex<T> {}

impl<T> AsyncMutex<T> {
    pub fn new(val: T) -> Self {
        Self { lockword: AtomicU64::new(0), data: UnsafeCell::new(val), lock_id: iomgr().alloc_lock_id() }
    }

    async fn try_lock_fast(&self) -> bool {
        self.lockword.compare_exchange(0, lock_bit(), Ordering::Acquire, Ordering::Relaxed).is_ok()
    }

    async fn lock_slow_with_rid(&self, rid: usize) {
        let waiter_bit = waiter_mask(rid);
        loop {
            let mut word = self.lockword.load(Ordering::Relaxed);
            loop {
                if (word & lock_bit()) == 0 {
                    if self.try_lock_fast().await {
                        return;
                    }
                }
                let new_word = word | waiter_bit;
                match self.lockword.compare_exchange(word, new_word, Ordering::AcqRel, Ordering::Relaxed) {
                    Ok(_) => break,
                    Err(v) => word = v,
                }
            }
            let wq = iomgr().reactor(rid).wait_queue(self.lock_id);
            wq.wait().await;
            if self.try_lock_fast().await {
                return;
            }
        }
    }

    pub async fn lock(&self) -> AsyncMutexGuard<'_, T> {
        let rid = current_reactor_id().expect("AsyncMutex::lock called outside reactor thread");
        if self.try_lock_fast().await {
            return AsyncMutexGuard { mu: self };
        }
        self.lock_slow_with_rid(rid).await;
        AsyncMutexGuard { mu: self }
    }

    fn unlock(&self) {
        let old = self.lockword.fetch_and(!lock_bit(), Ordering::Release);
        let waiters_bitmap = old >> 1;
        if waiters_bitmap == 0 {
            return;
        }
        let rid = waiters_bitmap.trailing_zeros() as usize;
        let mask = !(1u64 << (rid + 1));
        self.lockword.fetch_and(mask, Ordering::AcqRel);
        iomgr().wake(rid, self.lock_id);
    }
}

pub struct AsyncMutexGuard<'a, T> {
    mu: &'a AsyncMutex<T>,
}

impl<'a, T> Deref for AsyncMutexGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { unsafe { &*self.mu.data.get() } }
}

impl<'a, T> DerefMut for AsyncMutexGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut Self::Target { unsafe { &mut *self.mu.data.get() } }
}

impl<'a, T> Drop for AsyncMutexGuard<'a, T> {
    fn drop(&mut self) { self.mu.unlock(); }
}
