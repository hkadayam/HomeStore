use std::ops::{Deref, DerefMut};

use tokio::sync::Mutex as TokioMutex;

pub struct AsyncMutex<T> {
    inner: TokioMutex<T>,
}

impl<T> AsyncMutex<T> {
    pub fn new(val: T) -> Self { Self { inner: TokioMutex::new(val) } }

    pub async fn lock(&self) -> AsyncMutexGuard<'_, T> { AsyncMutexGuard { guard: self.inner.lock().await } }
}

pub struct AsyncMutexGuard<'a, T> {
    guard: tokio::sync::MutexGuard<'a, T>,
}

impl<'a, T> Deref for AsyncMutexGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { &*self.guard }
}

impl<'a, T> DerefMut for AsyncMutexGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut Self::Target { &mut *self.guard }
}
