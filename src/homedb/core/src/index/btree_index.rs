//! Common trait implemented by UnshardedBtree and ShardedBtree.

use std::sync::Arc;

use homestore::index::btree::btree_kvs::{BtreeKey, BtreeValue};
use homestore::index::btree::btree_types::BtreeError;
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, PutStats, RemoveFilter};

/// Trait for a query result handle (single btree or partitioned). Implemented by each index backend.
pub trait IndexQueryHandle<K: 'static + BtreeKey, V: 'static + BtreeValue>: Send + std::any::Any {
    fn results(&self) -> &[(K, V)];
    fn has_more(&self) -> bool;
    fn into_any_send(self: Box<Self>) -> Box<dyn std::any::Any + Send>;
    /// Take ownership of the results, avoiding a clone. Default falls back to `to_vec()`.
    fn into_results(self: Box<Self>) -> Vec<(K, V)>;
}

/// Convert to Box<dyn Any> for downcast in query_next_batch. Requires IndexQueryHandle: Any.
/// Used by both UnshardedBtree and ShardedBtree; callers are cfg-gated so rust-analyzer
/// may not see both at once — suppress the spurious dead_code lint.
#[allow(dead_code)]
pub fn index_query_handle_into_any<K: 'static + BtreeKey, V: 'static + BtreeValue>(
    me: Box<dyn IndexQueryHandle<K, V>>,
) -> Box<dyn std::any::Any + Send> {
    me.into_any_send()
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
pub trait BtreeIndex<K: 'static + BtreeKey, V: 'static + BtreeValue>: Send + Sync {
    async fn put(&self, key: &K, value: &V) -> Result<PutStats, BtreeError>;

    async fn put_range(
        &self,
        range: BtreeKeyRange<K>,
        value: &V,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
    ) -> Result<PutStats, BtreeError>;

    async fn remove(&self, key: &K) -> Result<Option<V>, BtreeError>;

    async fn remove_range(
        &self,
        range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn RemoveFilter<K, V>>>,
    ) -> Result<u32, BtreeError>;

    async fn get(&self, key: &K) -> Result<Option<V>, BtreeError>;

    /// Range query; reverse = true uses reverse order. Internally uses query_traversal.
    async fn query(
        &self,
        range: BtreeKeyRange<K>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<K, V>>>,
        reverse: bool,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError>;

    /// Return the first matching entry in `range`, or `None` if empty.
    /// Uses direct descent (not sweep), efficient for single-result lookups.
    async fn get_first(
        &self,
        range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn GetFilter<K, V>>>,
    ) -> Result<Option<(K, V)>, BtreeError>;

    /// Fetch next batch for a previous query result handle.
    async fn query_next_batch(
        &self,
        handle: Box<dyn IndexQueryHandle<K, V>>,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError>;

    /// Scan a key range within a single leaf node, apply `filter` to old entries (inline GC),
    /// then insert `insert_key` → `value` — all in one leaf write lock cycle.
    ///
    /// `filter.mutate_key(&mut insert_key)` is called inside the write lock: this is where
    /// MVCC callers stamp `commit_ts = GLOBAL_SEQ.fetch_add(SeqCst)` onto the key.
    ///
    /// Both `insert_key` and `value` are taken by value so they can be moved into async
    /// reactor tasks for `ShardedBtree` without lifetime issues.
    ///
    /// Returns `(PutResult, hit_boundary)`:
    /// - `hit_boundary = true` if `scan_range` extends beyond the current leaf node,
    ///   meaning cross-node old versions may exist and deferred GC should be enqueued.
    async fn scan_and_put_one(
        &self,
        insert_key: K,
        value: V,
        scan_range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
        max_scan: usize,
    ) -> Result<(PutStats, bool), BtreeError>;
}
