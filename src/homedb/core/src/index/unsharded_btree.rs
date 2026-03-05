//! UnshardedBtree: wraps a single homestore Btree and implements BtreeIndex.
//! Pass-through: sync_code -> homestore sync; async_code -> homestore async.

use std::sync::Arc;

use homestore::index::btree::btree::Btree;
use homestore::index::btree::btree_kvs::{BtreeKey, BtreeValue};
use homestore::index::btree::btree_types::{BtreeConfig, BtreeError};
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, PutStats, QueryResultHandle, RemoveFilter};
use homestore::index::btree::UnderlyingBtree;

use super::btree_index::{BtreeIndex, IndexQueryHandle};

/// Wrapper so homestore's QueryResultHandle can implement IndexQueryHandle.
struct SingleQueryHandle<K: 'static + BtreeKey, V: 'static + BtreeValue>(QueryResultHandle<K, V>);

fn to_single_query_handle<K: 'static + BtreeKey, V: 'static + BtreeValue>(
    handle: Box<dyn IndexQueryHandle<K, V>>,
) -> Result<SingleQueryHandle<K, V>, BtreeError> {
    super::btree_index::index_query_handle_into_any(handle)
        .downcast::<SingleQueryHandle<K, V>>()
        .map(|b| *b)
        .map_err(|_| {
            BtreeError::Io(std::io::Error::new(
                std::io::ErrorKind::Other,
                "UnshardedBtree requires SingleQueryHandle",
            ))
        })
}

impl<K: 'static + BtreeKey, V: 'static + BtreeValue> IndexQueryHandle<K, V> for SingleQueryHandle<K, V> {
    fn results(&self) -> &[(K, V)] {
        self.0.results.as_slice()
    }

    fn has_more(&self) -> bool {
        self.0.has_more()
    }

    fn into_any_send(self: Box<Self>) -> Box<dyn std::any::Any + Send> {
        self
    }

    fn into_results(self: Box<Self>) -> Vec<(K, V)> {
        self.0.results
    }
}

pub struct UnshardedBtree<K: 'static + BtreeKey, V: 'static + BtreeValue> {
    btree: Arc<Btree<K, V>>,
}

impl<K: 'static + BtreeKey, V: 'static + BtreeValue> UnshardedBtree<K, V> {
    /// Builds from config and storage factory (single btree, no sharding).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(
        config: BtreeConfig,
        storage_factory: impl Fn(BtreeConfig) -> Box<dyn UnderlyingBtree>,
    ) -> Result<Self, BtreeError> {
        let storage = storage_factory(config.clone());
        let btree = Arc::new(Btree::<K, V>::new(config, storage, None).await?);
        Ok(Self { btree })
    }

    pub fn btree(&self) -> &Arc<Btree<K, V>> { &self.btree }
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl<K: 'static + BtreeKey, V: 'static + BtreeValue> BtreeIndex<K, V> for UnshardedBtree<K, V> {
    async fn put(&self, key: &K, value: &V) -> Result<PutStats, BtreeError> {
        self.btree.put_one(key, value, None).await
    }

    async fn put_range(
        &self,
        range: BtreeKeyRange<K>,
        value: &V,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
    ) -> Result<PutStats, BtreeError> {
        self.btree.put_range(range, value, filter.as_ref().map(|a| a.as_ref())).await
    }

    async fn remove(&self, key: &K) -> Result<Option<V>, BtreeError> {
        self.btree.remove_one(key, None).await
    }

    async fn remove_range(
        &self,
        range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn RemoveFilter<K, V>>>,
    ) -> Result<u32, BtreeError> {
        self.btree.remove_range(range, filter.as_ref().map(|a| a.as_ref())).await
    }

    async fn get(&self, key: &K) -> Result<Option<V>, BtreeError> { self.btree.get(key).await }

    async fn get_first(
        &self,
        range: BtreeKeyRange<K>,
        _filter: Option<Arc<dyn GetFilter<K, V>>>,
    ) -> Result<Option<(K, V)>, BtreeError> {
        self.btree.get_first(range).await
    }

    async fn query(
        &self,
        range: BtreeKeyRange<K>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<K, V>>>,
        reverse: bool,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError> {
        let handle = if reverse {
            self.btree.query_traversal(range, batch_size, filter, reverse).await?
        } else {
            self.btree.query(range, batch_size, filter).await?
        };
        Ok(Box::new(SingleQueryHandle(handle)))
    }

    async fn query_next_batch(
        &self,
        handle: Box<dyn IndexQueryHandle<K, V>>,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError> {
        let h = to_single_query_handle(handle)?;
        let next = self.btree.query_next_batch(h.0).await?;
        Ok(Box::new(SingleQueryHandle(next)))
    }

    async fn scan_and_put_one(
        &self,
        mut insert_key: K,
        value: V,
        scan_range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
        max_scan: usize,
    ) -> Result<(PutStats, bool), BtreeError> {
        self.btree.scan_and_put_one(&mut insert_key, &value, &scan_range, filter.as_ref().map(|f| f.as_ref()), max_scan).await
    }
}
