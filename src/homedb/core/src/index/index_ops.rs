//! `IndexOps` — the internal dispatch trait shared by `NonTxnOps` and `MvccOps`.
//!
//! `TableIndex` holds an `Arc<dyn IndexOps>` and delegates every operation here,
//! keeping itself free of MVCC-vs-plain branching.
//!
//! `NonTxnOps` returns `Err(InvalidOperation)` for snapshot methods.
//! `MvccOps` returns `Err(InvalidOperation)` for nothing — it implements everything.

use std::sync::Arc;
use crate::common::error::{HomeDbError, Result};
use crate::mvcc::gc::MvccGc;
use super::iterator::RangeIterator;

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
pub trait IndexOps: Send + Sync {
    // --- Core CRUD ---

    async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()>;
    async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>>;
    async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>>;

    async fn get_range(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
        batch_size: u32,
        reverse: bool,
    ) -> Result<RangeIterator>;

    async fn get_any(&self, start: Vec<u8>, end: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>>;

    async fn remove_any(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>>;

    // --- Snapshot lifecycle ---
    //
    // `NonTxnOps` returns `Err(InvalidOperation)` from `register_snapshot` and is
    // a no-op for `release_snapshot`. `MvccOps` maintains a `SnapshotRegistry`.

    /// Register a snapshot at `ts`. Returns its unique ID for later `release`.
    /// Returns `Err(InvalidOperation)` on plain (non-MVCC) indices.
    fn register_snapshot(&self, ts: u64) -> Result<u64>;

    /// Deregister a snapshot. No-op on plain indices.
    fn release_snapshot(&self, ts: u64, id: u64);

    // --- GC access ---

    /// Return the deferred GC controller for MVCC tables.
    /// Returns `None` for plain (non-MVCC) tables.
    /// Primarily used by tests to call `run_cycle` directly.
    fn mvcc_gc(&self) -> Option<Arc<MvccGc>> { None }

    // --- Snapshot reads ---
    //
    // `NonTxnOps` returns `Err(InvalidOperation)` for both.

    async fn snapshot_get(&self, key: Vec<u8>, ts: u64) -> Result<Option<Vec<u8>>>;

    async fn snapshot_get_range(
        &self,
        start: Vec<u8>,
        end: Vec<u8>,
        batch_size: u32,
        reverse: bool,
        ts: u64,
    ) -> Result<RangeIterator>;
}

// ---------------------------------------------------------------------------
// Shared error helpers for NonTxnOps snapshot stubs
// ---------------------------------------------------------------------------

pub(crate) fn err_not_mvcc(method: &str) -> HomeDbError {
    HomeDbError::InvalidOperation(format!(
        "{method} requires an MVCC table (set mvcc_supported = true in TableSpec)"
    ))
}
