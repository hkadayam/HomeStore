//! ShardedBtree: fixed shards + dynamic partition registry.
//!
//! Routing: shard_id = hash(key[..partition_key_len]) % num_shards
//! Backend dispatch via shard_call! macro — single impl BtreeIndex block.

use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicI64, Ordering::Relaxed};
use std::hash::{Hash, Hasher};

use parking_lot::RwLock;
use smallvec::SmallVec;

use homestore::index::btree::btree::Btree;
use homestore::index::btree::btree_kvs::{BtreeKey, BtreeValue, Partitionable};
use homestore::index::btree::btree_types::{BtreeConfig, BtreeError};
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, QueryResultHandle, RemoveFilter};
use homestore::index::btree::detail::PutResult;
use homestore::index::btree::UnderlyingBtree;

use super::btree_index::{BtreeIndex, IndexQueryHandle};

/// Default shard count for sync_backend.
#[cfg(feature = "sync_backend")]
pub const DEFAULT_NUM_SHARDS: usize = 64;

/// Partition key: leading bytes of a full key used as partition identifier.
type PartitionKey = SmallVec<[u8; 16]>;

//==============================================================================
// shard_call! — dispatch a btree op to the shard's backend.
//
// Usage: shard_call!(shard, [ref_vars_to_clone_for_async], expr_using_btree)
//
//   `btree` is implicitly bound:
//     sync_backend  → &Arc<Btree<..>>  (used by reference, no copy)
//     async_backend → Arc<Btree<..>>   (owned, moved into reactor task)
//
//   Listed vars are cloned ONLY for async_backend; sync uses refs directly.
//   Use &var in the expression — deref-coercion handles &&T→&T for sync.
//   The macro always returns R directly (never a Future).
//==============================================================================

// sync_backend: direct call, no reactor dispatch.
// Both `let btree` and `btree.$method(..)` live in the macro body → same hygiene → no issue.
#[cfg(feature = "sync_backend")]
macro_rules! shard_call {
    ($shard:expr, [$($var:ident),*], $method:ident($($args:tt)*)) => {{
        let btree = &$shard.btree;
        btree.$method($($args)*)
    }};
}

// async_backend + sync_frontend (sync_over_async): block caller on the reactor.
#[cfg(all(feature = "async_backend", not(feature = "async_frontend")))]
macro_rules! shard_call {
    ($shard:expr, [$($var:ident),*], $method:ident($($args:tt)*)) => {{
        let __shard = &$shard;
        let btree = ::std::sync::Arc::clone(&__shard.btree);
        $( let $var = $var.clone(); )*
        iomgr::spawn_and_block(
            iomgr::ReactorTarget::Reactor(__shard.reactor_id),
            async move { btree.$method($($args)*).await },
        )
    }};
}

// async_backend + async_frontend: await on the reactor.
#[cfg(all(feature = "async_backend", feature = "async_frontend"))]
macro_rules! shard_call {
    ($shard:expr, [$($var:ident),*], $method:ident($($args:tt)*)) => {{
        let __shard = &$shard;
        let btree = ::std::sync::Arc::clone(&__shard.btree);
        $( let $var = $var.clone(); )*
        iomgr::spawn_waitable(
            iomgr::ReactorTarget::Reactor(__shard.reactor_id),
            async move { btree.$method($($args)*).await },
        ).await
    }};
}

//==============================================================================
// Partition — boundary helper for range-clamping
//==============================================================================

struct Partition {
    part_key: PartitionKey,
}

impl Partition {
    fn from_key<K: Partitionable>(key: &K, partition_key_len: usize) -> Self {
        key.with_partition_bytes(|bytes| {
            let plen = if partition_key_len == 0 { bytes.len() } else { partition_key_len.min(bytes.len()) };
            Self { part_key: bytes[..plen].iter().copied().collect() }
        })
    }

    fn from_range<K: Partitionable>(range: &BtreeKeyRange<K>, partition_key_len: usize) -> Self {
        if partition_key_len == 0 { return Self { part_key: PartitionKey::new() }; }
        Self::from_key(&range.start_key, partition_key_len)
    }

    fn first_key<K: BtreeKey>(&self) -> K {
        K::deserialize_from(self.part_key.as_ref(), true)
            .expect("K from partition key")
    }

    fn next(&self) -> Option<Partition> {
        let len = self.part_key.len();
        if len == 0 { return None; }
        let mut nxt = self.part_key.clone();
        for i in (0..len).rev() {
            let (v, carry) = nxt[i].overflowing_add(1);
            nxt[i] = v;
            if !carry { return Some(Self { part_key: nxt }); }
        }
        None
    }

    fn is_out_of_range<K: BtreeKey>(&self, range: &BtreeKeyRange<K>) -> bool {
        let first: K = self.first_key();
        if range.end_incl { first > range.end_key } else { first >= range.end_key }
    }
}

impl Hash for Partition {
    fn hash<H: Hasher>(&self, state: &mut H) { self.part_key.as_ref().hash(state); }
}

//==============================================================================
// Core data structures
//==============================================================================

struct Shard<K: 'static + BtreeKey, V: 'static + BtreeValue> {
    btree: Arc<Btree<K, V>>,
    #[cfg(feature = "async_backend")]
    reactor_id: usize,
}

struct PartitionEntry {
    entry_count: AtomicI64,
}

/// Pagination handle for ShardedBtree queries.
pub struct ShardedQueryHandle<K: 'static + BtreeKey, V: 'static + BtreeValue> {
    results: Vec<(K, V)>,
    input_range: BtreeKeyRange<K>,
    batch_size: u32,
    filter: Option<Arc<dyn GetFilter<K, V>>>,
    reverse: bool,
    partitions: Vec<PartitionKey>,
    cur_part_idx: usize,
    cur_handle: Option<QueryResultHandle<K, V>>,
}

impl<K: 'static + BtreeKey, V: 'static + BtreeValue> ShardedQueryHandle<K, V> {
    fn has_more_impl(&self) -> bool {
        self.cur_handle.as_ref().map_or(false, |h| h.has_more())
            || self.cur_part_idx < self.partitions.len()
    }
}

impl<K: 'static + BtreeKey, V: 'static + BtreeValue> IndexQueryHandle<K, V> for ShardedQueryHandle<K, V> {
    fn results(&self) -> &[(K, V)] { &self.results }
    fn has_more(&self) -> bool { self.has_more_impl() }
}

fn to_sharded_query_handle<K: 'static + BtreeKey, V: 'static + BtreeValue>(
    handle: Box<dyn IndexQueryHandle<K, V>>,
) -> Result<ShardedQueryHandle<K, V>, BtreeError> {
    super::btree_index::index_query_handle_into_any(handle)
        .downcast::<ShardedQueryHandle<K, V>>()
        .map(|b| *b)
        .map_err(|_| BtreeError::Io(std::io::Error::new(
            std::io::ErrorKind::Other, "ShardedBtree requires ShardedQueryHandle",
        )))
}

//==============================================================================
// ShardedBtree
//==============================================================================

pub struct ShardedBtree<K: 'static + Partitionable, V: 'static + BtreeValue> {
    shards: Vec<Arc<Shard<K, V>>>,
    registry: RwLock<BTreeMap<PartitionKey, Arc<PartitionEntry>>>,
    partition_key_len: usize,
}

//==============================================================================
// new_btree_on_shard — per-shard btree construction, reactor-routed for async
//==============================================================================

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
async fn new_btree_on_shard<K: 'static + BtreeKey, V: 'static + BtreeValue>(
    shard_id: usize,
    cfg: BtreeConfig,
    storage: Box<dyn UnderlyingBtree>,
) -> Result<Btree<K, V>, BtreeError> {
    cfg_if::cfg_if! {
        if #[cfg(feature = "async_backend")] {
            cfg_if::cfg_if! {
                if #[cfg(feature = "async_frontend")] {
                    iomgr::spawn_waitable(
                        iomgr::ReactorTarget::Reactor(shard_id),
                        async move { Btree::<K, V>::new(cfg, storage, None).await },
                    ).await
                } else {
                    iomgr::spawn_and_block(
                        iomgr::ReactorTarget::Reactor(shard_id),
                        async move { Btree::<K, V>::new(cfg, storage, None).await },
                    )
                }
            }
        } else {
            let _ = shard_id;
            Btree::<K, V>::new(cfg, storage, None)
        }
    }
}

//==============================================================================
// Construction
//==============================================================================

impl<K: 'static + Partitionable, V: 'static + BtreeValue> ShardedBtree<K, V> {
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(
        config: BtreeConfig,
        partition_key_len: usize,
        storage_factory: impl Fn(BtreeConfig) -> Box<dyn UnderlyingBtree>,
    ) -> Result<Self, BtreeError> {
        cfg_if::cfg_if! {
            if #[cfg(feature = "async_backend")] {
                let num_shards = std::cmp::max(1, iomgr::iomgr().num_reactors());
            } else {
                let num_shards = DEFAULT_NUM_SHARDS;
            }
        }

        let mut shards = Vec::with_capacity(num_shards);
        for i in 0..num_shards {
            let mut cfg = config.clone();
            cfg.btree_name = format!("{}_{}", config.btree_name, i);
            cfg_if::cfg_if! {
                if #[cfg(feature = "async_backend")] { cfg.is_single_threaded = true; }
            }
            let storage = storage_factory(cfg.clone());
            let btree = new_btree_on_shard::<K, V>(i, cfg, storage).await?;
            cfg_if::cfg_if! {
                if #[cfg(feature = "async_backend")] {
                    shards.push(Arc::new(Shard { btree: Arc::new(btree), reactor_id: i }));
                } else {
                    shards.push(Arc::new(Shard { btree: Arc::new(btree) }));
                }
            }
        }

        Ok(Self { shards, registry: RwLock::new(BTreeMap::new()), partition_key_len })
    }
}

//==============================================================================
// Helper methods
//==============================================================================

impl<K: 'static + Partitionable, V: 'static + BtreeValue> ShardedBtree<K, V> {
    fn num_shards(&self) -> usize { self.shards.len() }

    fn shard_from_part_key(&self, part_key: &PartitionKey) -> &Arc<Shard<K, V>> {
        use std::collections::hash_map::DefaultHasher;
        let mut h = DefaultHasher::new();
        part_key.as_ref().hash(&mut h);
        &self.shards[(h.finish() as usize) % self.num_shards()]
    }

    fn get_part_key(&self, key: &K) -> PartitionKey {
        key.with_partition_bytes(|b| {
            let plen = if self.partition_key_len == 0 { b.len() } else { self.partition_key_len.min(b.len()) };
            SmallVec::from_slice(&b[..plen])
        })
    }

    fn get_or_create_entry(&self, part_key: &PartitionKey) -> Arc<PartitionEntry> {
        if let Some(e) = self.registry.read().get(part_key) { return Arc::clone(e); }
        Arc::clone(
            self.registry.write()
                .entry(part_key.clone())
                .or_insert_with(|| Arc::new(PartitionEntry { entry_count: AtomicI64::new(0) })),
        )
    }

    fn decrement_and_cleanup(&self, part_key: &PartitionKey, count: i64) {
        let maybe_zero = self.registry.read().get(part_key)
            .map(|e| e.entry_count.fetch_sub(count, Relaxed) - count == 0)
            .unwrap_or(false);
        if maybe_zero {
            let mut map = self.registry.write();
            if let Some(e) = map.get(part_key) {
                if e.entry_count.load(Relaxed) == 0 { map.remove(part_key); }
            }
        }
    }

    /// List partition keys within [start_part_key, end_part_key] from the registry.
    fn active_partitions_in_range(&self, start_part_key: &PartitionKey, end_part_key: &PartitionKey) -> Vec<PartitionKey> {
        self.registry.read()
            .range(start_part_key.clone()..=end_part_key.clone())
            .map(|(k, _)| k.clone())
            .collect()
    }

    fn clamp_range(&self, range: &BtreeKeyRange<K>, partition: &Partition) -> BtreeKeyRange<K> {
        if self.partition_key_len == 0 { return range.clone(); }
        let mut ret = range.clone();
        let first: K = partition.first_key();
        if range.start_key < first { ret.start_key = first; ret.start_incl = true; }
        if let Some(next) = partition.next() {
            let nf: K = next.first_key();
            if range.end_key >= nf { ret.end_key = nf; ret.end_incl = false; }
        }
        ret
    }

    /// Return the partition at position `idx` in a snapshotted partition list.
    fn nth_partition(parts: &[PartitionKey], idx: usize) -> Partition {
        Partition { part_key: parts[idx].clone() }
    }

    /// Fill the query handle with the next batch across partitions.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn fill_query_handle(&self, handle: &mut ShardedQueryHandle<K, V>) -> Result<(), BtreeError> {
        while handle.cur_part_idx < handle.partitions.len()
            && (handle.results.len() as u32) < handle.batch_size
        {
            let part_key = handle.partitions[handle.cur_part_idx].clone();
            let shard = self.shard_from_part_key(&part_key);
            let part = Self::nth_partition(&handle.partitions, handle.cur_part_idx);
            let this_range = self.clamp_range(&handle.input_range, &part);
            let remaining = handle.batch_size - handle.results.len() as u32;
            let filter = handle.filter.clone();
            let reverse = handle.reverse;

            let mut inner = if reverse {
                shard_call!(shard, [], query_traversal(this_range, remaining, filter, reverse))
            } else {
                shard_call!(shard, [], query(this_range, remaining, filter))
            }?;

            handle.results.append(&mut inner.results);
            if inner.has_more() { handle.cur_handle = Some(inner); break; }
            handle.cur_part_idx += 1;
        }
        Ok(())
    }
}

//==============================================================================
// BtreeIndex impl — single block, both backends
//==============================================================================

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl<K: 'static + Partitionable, V: 'static + BtreeValue> BtreeIndex<K, V> for ShardedBtree<K, V> {
    async fn put(&self, key: &K, value: &V) -> Result<(), BtreeError> {
        let part_key = self.get_part_key(key);
        let shard = self.shard_from_part_key(&part_key);

        let result = shard_call!(shard, [key, value], put_one(&key, &value, None));
        if matches!(result, Ok(PutResult::Success)) {
            self.get_or_create_entry(&part_key).entry_count.fetch_add(1, Relaxed);
        }
        result.map(|_| ())
    }

    async fn put_range(
        &self,
        range: BtreeKeyRange<K>,
        value: &V,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
    ) -> Result<(), BtreeError> {
        let mut cur_part = Some(Partition::from_range(&range, self.partition_key_len));
        while let Some(ref part) = cur_part {
            if part.is_out_of_range(&range) { break; }

            let this_range = self.clamp_range(&range, part);
            let shard = self.shard_from_part_key(&part.part_key);
            shard_call!(shard, [value, filter], put_range(this_range, &value, filter.as_ref().map(|f| f.as_ref())))?;
            cur_part = part.next();
        }
        Ok(())
    }

    async fn remove(&self, key: &K) -> Result<Option<V>, BtreeError> {
        let part_key = self.get_part_key(key);
        let shard = self.shard_from_part_key(&part_key);
        let result = shard_call!(shard, [key], remove_one(&key, None));
        if let Ok(Some(_)) = &result { self.decrement_and_cleanup(&part_key, 1); }
        result
    }

    async fn remove_range(
        &self,
        range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn RemoveFilter<K, V>>>,
    ) -> Result<u32, BtreeError> {
        let start_part = self.get_part_key(&range.start_key);
        let end_part = self.get_part_key(&range.end_key);

        let parts = self.active_partitions_in_range(&start_part, &end_part);
        let mut total: u32 = 0;

        for idx in 0..parts.len() {
            let part = Self::nth_partition(&parts, idx);
            let shard = self.shard_from_part_key(&parts[idx]);
            let this_range = self.clamp_range(&range, &part);
            let count = shard_call!(shard, [filter], remove_range(this_range, filter.as_ref().map(|f| f.as_ref())))?;
            if count > 0 {
                total += count;
                self.decrement_and_cleanup(&parts[idx], count as i64);
            }
        }
        Ok(total)
    }

    async fn get(&self, key: &K) -> Result<Option<V>, BtreeError> {
        let part_key = self.get_part_key(key);
        let shard = self.shard_from_part_key(&part_key);
        shard_call!(shard, [key], get(&key))
    }

    async fn query(
        &self,
        range: BtreeKeyRange<K>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<K, V>>>,
        reverse: bool,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError> {
        let start_part = self.get_part_key(&range.start_key);
        let end_part = self.get_part_key(&range.end_key);
        let partitions = self.active_partitions_in_range(&start_part, &end_part);

        let mut handle = ShardedQueryHandle {
            results: Vec::new(), input_range: range, batch_size, filter, reverse,
            partitions, cur_part_idx: 0, cur_handle: None,
        };
        self.fill_query_handle(&mut handle).await?;
        Ok(Box::new(handle))
    }

    async fn query_next_batch(
        &self,
        h: Box<dyn IndexQueryHandle<K, V>>,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError> {
        let mut handle = to_sharded_query_handle(h)?;
        handle.results.clear();

        if let Some(inner_h) = handle.cur_handle.take() {
            let shard = self.shard_from_part_key(&handle.partitions[handle.cur_part_idx]);
            let mut next = shard_call!(shard, [], query_next_batch(inner_h))?;
            handle.results.append(&mut next.results);

            if next.has_more() { handle.cur_handle = Some(next); return Ok(Box::new(handle)); }
            handle.cur_part_idx += 1;
        }

        self.fill_query_handle(&mut handle).await?;
        Ok(Box::new(handle))
    }

    async fn scan_and_put_one(
        &self,
        mut insert_key: K,
        value: V,
        scan_range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
    ) -> Result<(PutResult, bool), BtreeError> {
        // Route by scan_range.start_key: all versions of the same user key share identical
        // partition bytes (MvccKey<K: Partitionable> delegates to inner key), so all versions
        // land in the same shard. No cross-shard coordination needed.
        //
        // insert_key is owned (mut) and NOT in the shard_call clone list — it is captured by
        // move into the async block, where &mut insert_key is valid as a local borrow.
        let part_key = self.get_part_key(&scan_range.start_key);
        let shard = self.shard_from_part_key(&part_key);
        let result = shard_call!(shard, [value, scan_range, filter], scan_and_put_one(&mut insert_key, &value, &scan_range, filter.as_ref().map(|f| f.as_ref())));
        // Register the partition so that query() can find it via active_partitions_in_range.
        // scan_and_put_one always inserts one new key (MVCC keys are unique per commit_ts).
        if result.is_ok() {
            self.get_or_create_entry(&part_key).entry_count.fetch_add(1, Relaxed);
        }
        result
    }
}
