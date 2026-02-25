//! ConcurrentBtree: wraps a single homestore Btree and implements BtreeIndex.
//! Pass-through: sync_code -> homestore sync; async_code -> homestore async.

use std::sync::Arc;

use homestore::index::btree::btree::Btree;
use homestore::index::btree::btree_types::{BtreeConfig, BtreeError};
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, QueryResultHandle, RemoveFilter};
use homestore::index::btree::UnderlyingBtree;

use super::{
    btree_index::{BtreeIndex, IndexQueryHandle},
    db_kv::{DbKey, DbValue},
};

/// Wrapper so homestore's QueryResultHandle can implement IndexQueryHandle.
struct SingleQueryHandle(QueryResultHandle<DbKey, DbValue>);

fn to_single_query_handle(
    handle: Box<dyn IndexQueryHandle>,
) -> Result<SingleQueryHandle, BtreeError> {
    super::btree_index::index_query_handle_into_any(handle)
        .downcast::<SingleQueryHandle>()
        .map(|b| *b)
        .map_err(|_| {
            BtreeError::Io(std::io::Error::new(
                std::io::ErrorKind::Other,
                "ConcurrentBtree requires SingleQueryHandle",
            ))
        })
}

impl IndexQueryHandle for SingleQueryHandle {
    fn results(&self) -> &[(DbKey, DbValue)] {
        self.0.results.as_slice()
    }

    fn has_more(&self) -> bool {
        self.0.has_more()
    }
}

pub struct ConcurrentBtree {
    btree: Arc<Btree<DbKey, DbValue>>,
}

impl ConcurrentBtree {
    /// Builds from config and storage factory. Same pattern as LockFreeBtree::new (single btree).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(
        config: BtreeConfig,
        storage_factory: impl Fn(BtreeConfig) -> Box<dyn UnderlyingBtree>,
    ) -> Result<Self, BtreeError> {
        let storage = storage_factory(config.clone());
        let btree = Arc::new(Btree::<DbKey, DbValue>::new(config, storage, None).await?);
        Ok(Self { btree })
    }

    pub fn btree(&self) -> &Arc<Btree<DbKey, DbValue>> { &self.btree }
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl BtreeIndex for ConcurrentBtree {
    async fn put(&self, key: &DbKey, value: &DbValue) -> Result<(), BtreeError> {
        self.btree.put_one(key, value, None).await
    }

    async fn put_range(
        &self,
        range: BtreeKeyRange<DbKey>,
        value: &DbValue,
        filter: Option<Arc<dyn PutFilter<DbKey, DbValue>>>,
    ) -> Result<(), BtreeError> {
        self.btree.put_range(range, value, filter.as_ref().map(|a| a.as_ref())).await
    }

    async fn remove(&self, key: &DbKey) -> Result<Option<DbValue>, BtreeError> {
        self.btree.remove_one(key, None).await
    }

    async fn remove_range(
        &self,
        range: BtreeKeyRange<DbKey>,
        filter: Option<Arc<dyn RemoveFilter<DbKey, DbValue>>>,
    ) -> Result<u32, BtreeError> {
        self.btree.remove_range(range, filter.as_ref().map(|a| a.as_ref())).await
    }

    async fn get(&self, key: &DbKey) -> Result<Option<DbValue>, BtreeError> { self.btree.get(key).await }

    async fn query(
        &self,
        range: BtreeKeyRange<DbKey>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<DbKey, DbValue>>>,
        reverse: bool,
    ) -> Result<Box<dyn IndexQueryHandle>, BtreeError> {
        let handle = if reverse {
            self.btree.query_traversal(range, batch_size, filter, reverse).await?
        } else {
            self.btree.query(range, batch_size, filter).await?
        };
        Ok(Box::new(SingleQueryHandle(handle)))
    }

    async fn query_next_batch(&self, handle: Box<dyn IndexQueryHandle>) -> Result<Box<dyn IndexQueryHandle>, BtreeError> {
        let h = to_single_query_handle(handle)?;
        let next = self.btree.query_next_batch(h.0).await?;
        Ok(Box::new(SingleQueryHandle(next)))
    }
}
