use std::ops::{Deref, DerefMut};

use tokio::sync::RwLock as TokioRwLock;

pub struct AsyncRwLock<T> {
    inner: TokioRwLock<T>,
}

impl<T> AsyncRwLock<T> {
    pub fn new(val: T) -> Self { Self { inner: TokioRwLock::new(val) } }

    pub async fn write(&self) -> AsyncRwWriteGuard<'_, T> { AsyncRwWriteGuard { guard: self.inner.write().await } }

    pub async fn read(&self) -> AsyncRwReadGuard<'_, T> { AsyncRwReadGuard { guard: self.inner.read().await } }

    pub async fn write_on(&self, _rid: usize) -> AsyncRwWriteGuard<'_, T> { self.write().await }

    pub async fn read_on(&self, _rid: usize) -> AsyncRwReadGuard<'_, T> { self.read().await }
}

pub struct AsyncRwWriteGuard<'a, T> {
    guard: tokio::sync::RwLockWriteGuard<'a, T>,
}

impl<'a, T> Deref for AsyncRwWriteGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { &*self.guard }
}

impl<'a, T> DerefMut for AsyncRwWriteGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut Self::Target { &mut *self.guard }
}

pub struct AsyncRwReadGuard<'a, T> {
    guard: tokio::sync::RwLockReadGuard<'a, T>,
}

impl<'a, T> Deref for AsyncRwReadGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { &*self.guard }
}

// Safety: Tokio's underlying guards are already Send, we just need to mark ours as well
unsafe impl<'a, T: Send> Send for AsyncRwReadGuard<'a, T> {}
unsafe impl<'a, T: Send> Send for AsyncRwWriteGuard<'a, T> {}
