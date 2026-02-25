//! Common trait for a single ConcurrentBtree or LockFreeBtree.
//!
//! TableIndex holds either a ConcurrentBtree
//!  or a LockFreeBtree; both implement this trait.

use std::sync::Arc;

use homestore::index::btree::btree_types::BtreeError;
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, RemoveFilter};

use super::db_kv::{DbKey, DbValue};

/// Trait for a query result handle (single btree or partitioned). Implemented by each index backend.
pub trait IndexQueryHandle: Send + std::any::Any {
    fn results(&self) -> &[(DbKey, DbValue)];
    fn has_more(&self) -> bool;
}

/// Convert to Box<dyn Any> for downcast in query_next_batch. Requires IndexQueryHandle: Any.
/// Used by both ConcurrentBtree and LockFreeBtree; callers are cfg-gated so rust-analyzer
/// may not see both at once — suppress the spurious dead_code lint.
#[allow(dead_code)]
pub fn index_query_handle_into_any(me: Box<dyn IndexQueryHandle>) -> Box<dyn std::any::Any + Send> {
    unsafe { Box::from_raw(Box::into_raw(me) as *mut (dyn std::any::Any + Send)) }
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
pub trait BtreeIndex: Send + Sync {
    async fn put(&self, key: &DbKey, value: &DbValue) -> Result<(), BtreeError>;

    async fn put_range(
        &self,
        range: BtreeKeyRange<DbKey>,
        value: &DbValue,
        filter: Option<Arc<dyn PutFilter<DbKey, DbValue>>>,
    ) -> Result<(), BtreeError>;

    async fn remove(&self, key: &DbKey) -> Result<Option<DbValue>, BtreeError>;

    async fn remove_range(
        &self,
        range: BtreeKeyRange<DbKey>,
        filter: Option<Arc<dyn RemoveFilter<DbKey, DbValue>>>,
    ) -> Result<u32, BtreeError>;

    async fn get(&self, key: &DbKey) -> Result<Option<DbValue>, BtreeError>;

    /// Range query; reverse = true uses reverse order. Internally uses query_traversal.
    async fn query(
        &self,
        range: BtreeKeyRange<DbKey>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<DbKey, DbValue>>>,
        reverse: bool,
    ) -> Result<Box<dyn IndexQueryHandle>, BtreeError>;

    /// Fetch next batch for a previous query result handle.
    async fn query_next_batch(&self, handle: Box<dyn IndexQueryHandle>) -> Result<Box<dyn IndexQueryHandle>, BtreeError>;
}
