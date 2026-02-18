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

pub struct AsyncRwLock<T> {
    lockword: AtomicU64,
    reader_count: AtomicU64,
    data: UnsafeCell<T>,
    lock_id: LockId,
}

unsafe impl<T: Send> Send for AsyncRwLock<T> {}
unsafe impl<T: Send + Sync> Sync for AsyncRwLock<T> {}

impl<T> AsyncRwLock<T> {
    pub fn new(val: T) -> Self {
        Self {
            lockword: AtomicU64::new(0),
            reader_count: AtomicU64::new(0),
            data: UnsafeCell::new(val),
            lock_id: iomgr().alloc_lock_id(),
        }
    }

    async fn slow_write(&self, rid: usize) {
        let waiter_bit = waiter_mask(rid);
        loop {
            let mut lw = self.lockword.load(Ordering::Acquire);
            loop {
                let readers = self.reader_count.load(Ordering::Acquire);
                if lw == 0 && readers == 0 {
                    if self.lockword.compare_exchange(lw, lock_bit(), Ordering::AcqRel, Ordering::Relaxed).is_ok() {
                        return;
                    }
                }
                let new_lw = lw | waiter_bit;
                match self.lockword.compare_exchange(lw, new_lw, Ordering::AcqRel, Ordering::Relaxed) {
                    Ok(_) => break,
                    Err(v) => lw = v,
                }
            }
            let wq = iomgr().reactor(rid).wait_queue(self.lock_id);
            wq.wait().await;
            let lw2 = self.lockword.load(Ordering::Acquire);
            if lw2 == 0 && self.reader_count.load(Ordering::Acquire) == 0 {
                if self.lockword.compare_exchange(lw2, lock_bit(), Ordering::AcqRel, Ordering::Relaxed).is_ok() {
                    return;
                }
            }
        }
    }

    pub async fn write(&self) -> AsyncRwWriteGuard<'_, T> {
        let rid = current_reactor_id().expect("AsyncRwLock::write_lock outside reactor");
        let lw = self.lockword.load(Ordering::Acquire);
        if lw == 0 && self.reader_count.load(Ordering::Acquire) == 0 {
            if self.lockword.compare_exchange(0, lock_bit(), Ordering::AcqRel, Ordering::Relaxed).is_ok() {
                return AsyncRwWriteGuard { rw: self };
            }
        }
        self.slow_write(rid).await;
        AsyncRwWriteGuard { rw: self }
    }

    pub async fn write_on(&self, rid: usize) -> AsyncRwWriteGuard<'_, T> {
        let lw = self.lockword.load(Ordering::Acquire);
        if lw == 0 && self.reader_count.load(Ordering::Acquire) == 0 {
            if self.lockword.compare_exchange(0, lock_bit(), Ordering::AcqRel, Ordering::Relaxed).is_ok() {
                return AsyncRwWriteGuard { rw: self };
            }
        }
        self.slow_write(rid).await;
        AsyncRwWriteGuard { rw: self }
    }

    async fn slow_read(&self, rid: usize) {
        let waiter_bit = waiter_mask(rid);
        loop {
            let lw = self.lockword.load(Ordering::Acquire);
            let writer_waiting = (lw >> 1) != 0;
            let writer_active = (lw & lock_bit()) != 0;
            if !writer_active && !writer_waiting {
                let _prev = self.reader_count.fetch_add(1, Ordering::AcqRel);
                let lw2 = self.lockword.load(Ordering::Acquire);
                let w_wait2 = (lw2 >> 1) != 0;
                let w_act2 = (lw2 & lock_bit()) != 0;
                if w_act2 || w_wait2 {
                    self.reader_count.fetch_sub(1, Ordering::AcqRel);
                } else {
                    return;
                }
            }
            let mut cur = lw;
            loop {
                let new_cur = cur | waiter_bit;
                match self.lockword.compare_exchange(cur, new_cur, Ordering::AcqRel, Ordering::Relaxed) {
                    Ok(_) => break,
                    Err(v) => cur = v,
                }
            }
            let wq = iomgr().reactor(rid).wait_queue(self.lock_id);
            wq.wait().await;
        }
    }

    pub async fn read(&self) -> AsyncRwReadGuard<'_, T> {
        loop {
            let rid = current_reactor_id().expect("AsyncRwLock::read_lock outside reactor");
            let lw = self.lockword.load(Ordering::Acquire);
            let writer_waiting = (lw >> 1) != 0;
            let writer_active = (lw & lock_bit()) != 0;
            if !writer_active && !writer_waiting {
                let _prev = self.reader_count.fetch_add(1, Ordering::AcqRel);
                let lw2 = self.lockword.load(Ordering::Acquire);
                let w_wait2 = (lw2 >> 1) != 0;
                let w_act2 = (lw2 & lock_bit()) != 0;
                if w_act2 || w_wait2 {
                    self.reader_count.fetch_sub(1, Ordering::AcqRel);
                } else {
                    return AsyncRwReadGuard { rw: self };
                }
            }
            self.slow_read(rid).await;
            return AsyncRwReadGuard { rw: self };
        }
    }

    pub async fn read_on(&self, rid: usize) -> AsyncRwReadGuard<'_, T> {
        loop {
            let lw = self.lockword.load(Ordering::Acquire);
            let writer_waiting = (lw >> 1) != 0;
            let writer_active = (lw & lock_bit()) != 0;
            if !writer_active && !writer_waiting {
                let _prev = self.reader_count.fetch_add(1, Ordering::AcqRel);
                let lw2 = self.lockword.load(Ordering::Acquire);
                let w_wait2 = (lw2 >> 1) != 0;
                let w_act2 = (lw2 & lock_bit()) != 0;
                if w_act2 || w_wait2 {
                    self.reader_count.fetch_sub(1, Ordering::AcqRel);
                } else {
                    return AsyncRwReadGuard { rw: self };
                }
            }
            self.slow_read(rid).await;
            return AsyncRwReadGuard { rw: self };
        }
    }

    fn write_unlock(&self) {
        let old = self.lockword.fetch_and(!lock_bit(), Ordering::Release);
        self.wake_one(old);
    }

    fn read_unlock(&self) {
        let prev = self.reader_count.fetch_sub(1, Ordering::Release);
        if prev == 1 {
            let lw = self.lockword.load(Ordering::Acquire);
            self.wake_one(lw);
        }
    }

    fn wake_one(&self, word: u64) {
        let waiters = word >> 1;
        if waiters == 0 {
            return;
        }
        let rid = waiters.trailing_zeros() as usize;
        let mask = !(1u64 << (rid + 1));
        self.lockword.fetch_and(mask, Ordering::AcqRel);
        iomgr().wake(rid, self.lock_id);
    }

}

pub struct AsyncRwWriteGuard<'a, T> {
    rw: &'a AsyncRwLock<T>,
}

impl<'a, T> Deref for AsyncRwWriteGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { unsafe { &*self.rw.data.get() } }
}

impl<'a, T> DerefMut for AsyncRwWriteGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut Self::Target { unsafe { &mut *self.rw.data.get() } }
}

impl<'a, T> Drop for AsyncRwWriteGuard<'a, T> {
    fn drop(&mut self) { self.rw.write_unlock(); }
}

pub struct AsyncRwReadGuard<'a, T> {
    rw: &'a AsyncRwLock<T>,
}

impl<'a, T> Deref for AsyncRwReadGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { unsafe { &*self.rw.data.get() } }
}

impl<'a, T> Drop for AsyncRwReadGuard<'a, T> {
    fn drop(&mut self) { self.rw.read_unlock(); }
}

// Safety: AsyncRwReadGuard can be Send because:
// 1. It only holds a reference to AsyncRwLock<T>
// 2. AsyncRwLock<T> is already Send + Sync (see line 22-23)
// 3. The guard only provides access to T through Deref, which is safe across threads when T: Send
// 4. The Drop implementation (read_unlock) is thread-safe
unsafe impl<'a, T: Send> Send for AsyncRwReadGuard<'a, T> {}

// Safety: AsyncRwWriteGuard can be Send for the same reasons as AsyncRwReadGuard
unsafe impl<'a, T: Send> Send for AsyncRwWriteGuard<'a, T> {}
