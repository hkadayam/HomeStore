//! ShardedBtree: fixed shards + dynamic partition registry.
//!
//! Routing: shard_id = bytes_to_part_id(key[..partition_key_len]) % num_shards
//! Registry key = part_id (order-preserving), enabling O(log n) range queries.
//! Backend dispatch via shard_call! macro — single impl BtreeIndex block.

use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::time::Duration;

use parking_lot::RwLock;
use smallvec::SmallVec;

use homestore::index::btree::btree::Btree;
use homestore::index::btree::btree_kvs::{BtreeKey, BtreeValue, Partitionable};
use homestore::index::btree::btree_types::{BtreeConfig, BtreeError};
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, PutStats, QueryResultHandle, RemoveFilter};
use homestore::index::btree::UnderlyingBtree;

use super::btree_index::{BtreeIndex, IndexQueryHandle};

/// Default shard count for sync_backend.
#[cfg(feature = "sync_backend")]
pub const DEFAULT_NUM_SHARDS: usize = 64;

/// Default maximum number of partition registry entries.
/// Must be a multiple of DEFAULT_NUM_SHARDS so that shard_id = part_id % num_shards
/// is consistent (same part_id → same shard).
pub const DEFAULT_MAX_PARTITIONS: usize = 8192;

/// Maximum number of partition key bytes used for part_id computation and storage.
/// Capped at 16 so that the full key prefix fits in a u128 for order-preserving quantization.
pub const MAX_PARTITION_KEY_BYTES: usize = 16;

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
// bytes_to_part_id — order-preserving linear quantization (u128, up to 16 bytes)
//
// Maps partition key bytes to a u32 bucket in [0, max_partitions).
// Property: key1 ≤ key2 (lexicographically) ⟹ part_id(key1) ≤ part_id(key2).
// This allows BTreeMap<u32, …> range queries to be used for key-range scans.
//==============================================================================

fn bytes_to_part_id(bytes: &[u8], max_partitions: usize) -> u32 {
    if max_partitions <= 1 || bytes.is_empty() { return 0; }
    // Interpret the leading (up to 16) bytes as a big-endian u128.
    // Shorter keys occupy the high bits; remaining bits are 0.
    // This preserves lexicographic order within the integer domain.
    let mut val: u128 = 0;
    for &b in bytes.iter().take(MAX_PARTITION_KEY_BYTES) {
        val = (val << 8) | b as u128;
    }
    // If the key is shorter than 16 bytes, shift left so it occupies the MSBs.
    // This ensures [0x01] maps to 0x01_00...00 (128-bit), not 0x01.
    let actual_len = bytes.len().min(MAX_PARTITION_KEY_BYTES);
    if actual_len < MAX_PARTITION_KEY_BYTES {
        val <<= 8 * (MAX_PARTITION_KEY_BYTES - actual_len);
    }
    // Linear quantization: divide [0, 2^128) into max_partitions equal buckets.
    let bucket_size = (u128::MAX / max_partitions as u128).saturating_add(1);
    (val / bucket_size).min(max_partitions as u128 - 1) as u32
}

//==============================================================================
// Partition — boundary helper + bloom-filter hint (unified, replaces old split)
//
// One struct holds: the representative partition key bytes (for range clamping
// and shard routing), plus an approximate live-entry count (bloom-filter hint).
//
// Registry entries are Arc<Partition> in BTreeMap<u32, Arc<Partition>>.
// Temporary "boundary cursor" values (used in put_range and put_range-like
// walking) are plain owned Partition with count=0, never inserted in the registry.
//==============================================================================

struct Partition {
    part_key: PartitionKey, // Partition key bytes: prefix of the keys in this partition,
                            // used for routing and range clamping.
    shard_id: usize, // Caching the shard_id this partition belongs to, avoid computing it repeatedly from part_key.
    count: AtomicI64, // Approx. count of live entries in this partition. A hint so that if count is 0, we could
                      // potentially remove the partition for registry. It would prevent routing to empty partitions.
}

impl Partition {
    fn new(part_key: PartitionKey, shard_id: usize) -> Self {
        Self { part_key, shard_id, count: AtomicI64::new(0) }
    }

    /// The first (smallest) key that belongs to this partition.
    fn first_key<K: BtreeKey>(&self) -> K {
        K::deserialize_from(self.part_key.as_ref(), true)
            .expect("K from partition key")
    }

    /// The next partition boundary key (all-bytes increment of part_key), used by clamp_range.
    fn next_first_key<K: BtreeKey>(&self) -> Option<K> {
        let len = self.part_key.len();
        if len == 0 { return None; }
        let mut nxt = self.part_key.clone();
        for i in (0..len).rev() {
            let (v, carry) = nxt[i].overflowing_add(1);
            nxt[i] = v;
            if !carry {
                return Some(K::deserialize_from(nxt.as_ref(), true).expect("K from next partition key"));
            }
        }
        None
    }
}

//==============================================================================
// Core data structures
//==============================================================================

struct Shard<K: 'static + BtreeKey, V: 'static + BtreeValue> {
    btree: Arc<Btree<K, V>>,
    #[cfg(feature = "async_backend")]
    reactor_id: usize,
}

/// Pagination handle for ShardedBtree queries.
pub struct ShardedQueryHandle<K: 'static + BtreeKey, V: 'static + BtreeValue> {
    results: Vec<(K, V)>,
    input_range: BtreeKeyRange<K>,
    batch_size: u32,
    filter: Option<Arc<dyn GetFilter<K, V>>>,
    reverse: bool,
    /// Partitions to iterate (in order). Each entry has the part_key for shard routing
    /// and range clamping. For single-partition queries this is a temporary Arc (count=0).
    partitions: Vec<Arc<Partition>>,
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
    fn into_any_send(self: Box<Self>) -> Box<dyn std::any::Any + Send> { self }
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
    shards: Arc<Vec<Arc<Shard<K, V>>>>,
    /// Bounded partition registry. Key = part_id = bytes_to_part_id(part_key, max_partitions).
    /// part_id order ≈ lexicographic key order, so BTreeMap::range enables O(log n) range queries.
    /// At most max_partitions entries. Stale entries (count ≤ 0) are evicted lazily by
    /// PartitionCleaner after confirming the btree partition is truly empty.
    registry: Arc<RwLock<BTreeMap<u32, Arc<Partition>>>>,
    partition_key_len: usize,
    max_partitions: usize,
    /// Signals the PartitionCleaner background task to stop on drop.
    shutdown: Arc<AtomicBool>,
}

impl<K: 'static + Partitionable, V: 'static + BtreeValue> Drop for ShardedBtree<K, V> {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::Relaxed);
    }
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
        max_partitions: usize,
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

        // Cap partition_key_len at MAX_PARTITION_KEY_BYTES: bytes beyond the 16-byte boundary
        // are indistinguishable in the u128 quantization, so they add no routing precision.
        let partition_key_len = if partition_key_len == 0 {
            0
        } else {
            partition_key_len.min(MAX_PARTITION_KEY_BYTES)
        };
        let max_partitions = if max_partitions == 0 { DEFAULT_MAX_PARTITIONS } else { max_partitions };

        let shards = Arc::new(shards);
        let registry = Arc::new(RwLock::new(BTreeMap::new()));
        let shutdown = Arc::new(AtomicBool::new(false));

        Arc::new(PartitionCleaner {
            shards: Arc::clone(&shards),
            registry: Arc::clone(&registry),
            partition_key_len,
            shutdown: Arc::clone(&shutdown),
        }).spawn();

        Ok(Self { shards, registry, partition_key_len, max_partitions, shutdown })
    }
}

//==============================================================================
// Helper methods
//==============================================================================

impl<K: 'static + Partitionable, V: 'static + BtreeValue> ShardedBtree<K, V> {
    fn num_shards(&self) -> usize { self.shards.len() }

    fn get_part_key(&self, key: &K) -> PartitionKey {
        key.with_partition_bytes(|b| {
            let plen = if self.partition_key_len == 0 {
                b.len().min(MAX_PARTITION_KEY_BYTES)
            } else {
                self.partition_key_len.min(b.len())
            };
            SmallVec::from_slice(&b[..plen])
        })
    }

    fn get_part_id(&self, part_key: &PartitionKey) -> u32 {
        bytes_to_part_id(part_key.as_ref(), self.max_partitions)
    }

    /// Route `key` to its partition entry.
    ///
    /// `register_if_missing = true`  — write path: check registry, create if absent, bump count.
    ///                                  Call BEFORE the btree write to close the scan-stability race.
    /// `register_if_missing = false` — read path: pure math, no registry access. Returns a
    ///                                  temporary partition with the correct shard_id.
    fn route(&self, key: &K, register_if_missing: bool) -> Arc<Partition> {
        let part_key = self.get_part_key(key);
        let part_id  = self.get_part_id(&part_key);
        let shard_id = part_id as usize % self.num_shards();

        // Fast path: entry already in registry.
        {
            let reg = self.registry.read();
            if let Some(entry) = reg.get(&part_id) {
                if register_if_missing { entry.count.fetch_add(1, Ordering::Relaxed); }
                return entry.clone();
            }
        }

        if !register_if_missing {
            // Partition not registered: never written to (or evicted). Return temp for routing.
            // The shard will return None for the key; no count to bump.
            return Arc::new(Partition::new(part_key, shard_id));
        }

        // Slow path: create new registry entry (no eviction — PartitionCleaner handles that lazily).
        let mut reg = self.registry.write();
        let entry = reg.entry(part_id)
            .or_insert_with(|| Arc::new(Partition::new(part_key, shard_id)))
            .clone();
        entry.count.fetch_add(1, Ordering::Relaxed);
        entry
    }

    /// Return all registered partitions whose part_id falls in the key range. O(log n + k).
    fn route_range(&self, range: &BtreeKeyRange<K>) -> Vec<Arc<Partition>> {
        let start_id = self.get_part_id(&self.get_part_key(&range.start_key));
        let end_id   = self.get_part_id(&self.get_part_key(&range.end_key));
        let reg = self.registry.read();
        reg.range(start_id..=end_id).map(|(_, e)| e.clone()).collect()
    }

    // Clamp the input key range to the partition boundaries
    fn clamp_range(&self, range: &BtreeKeyRange<K>, partition: &Partition) -> BtreeKeyRange<K> {
        if self.partition_key_len == 0 { return range.clone(); }
        let mut ret = range.clone();
        let first: K = partition.first_key();
        if range.start_key < first { ret.start_key = first; ret.start_incl = true; }
        if let Some(nf) = partition.next_first_key::<K>() {
            if range.end_key >= nf { ret.end_key = nf; ret.end_incl = false; }
        }
        ret
    }

    /// Fill the query handle with the next batch across partitions.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn fill_query_handle(&self, handle: &mut ShardedQueryHandle<K, V>) -> Result<(), BtreeError> {
        while handle.cur_part_idx < handle.partitions.len()
            && (handle.results.len() as u32) < handle.batch_size
        {
            let entry = handle.partitions[handle.cur_part_idx].clone();
            let shard = &self.shards[entry.shard_id];
            let this_range = self.clamp_range(&handle.input_range, &entry);
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
    async fn put(&self, key: &K, value: &V) -> Result<PutStats, BtreeError> {
        let part = self.route(key, /*register_if_missing=*/true);
        let result = shard_call!(&self.shards[part.shard_id], [key, value], put_one(&key, &value, None));
        match &result {
            Ok(stats) => { part.count.fetch_add(stats.count_delta() - 1, Ordering::Relaxed); }
            Err(_)    => { part.count.fetch_add(-1, Ordering::Relaxed); }
        }
        result
    }

    async fn put_range(
        &self,
        range: BtreeKeyRange<K>,
        value: &V,
        filter: Option<Arc<dyn PutFilter<K, V>>>,
    ) -> Result<PutStats, BtreeError> {
        let mut total = PutStats::default();
        for part in self.route_range(&range) {
            let shard = &self.shards[part.shard_id];
            let this_range = self.clamp_range(&range, &part);
            let stats = shard_call!(shard, [value, filter], put_range(this_range, &value, filter.as_ref().map(|f| f.as_ref())))?;
            part.count.fetch_add(stats.count_delta(), Ordering::Relaxed);
            total.merge(&stats);
        }
        Ok(total)
    }

    async fn remove(&self, key: &K) -> Result<Option<V>, BtreeError> {
        let part = self.route(key, false);
        let shard = &self.shards[part.shard_id];
        let result = shard_call!(shard, [key], remove_one(&key, None));
        if let Ok(Some(_)) = &result { part.count.fetch_add(-1, Ordering::Relaxed); }
        result
    }

    async fn remove_range(
        &self,
        range: BtreeKeyRange<K>,
        filter: Option<Arc<dyn RemoveFilter<K, V>>>,
    ) -> Result<u32, BtreeError> {
        let mut total: u32 = 0;

        for part in self.route_range(&range) {
            let shard = &self.shards[part.shard_id];
            let this_range = self.clamp_range(&range, &part);
            let count = shard_call!(shard, [filter], remove_range(this_range, filter.as_ref().map(|f| f.as_ref())))?;
            if count > 0 {
                total += count;
                part.count.fetch_add(-(count as i64), Ordering::Relaxed);
            }
        }
        Ok(total)
    }

    async fn get(&self, key: &K) -> Result<Option<V>, BtreeError> {
        let part = self.route(key, false);
        shard_call!(&self.shards[part.shard_id], [key], get(&key))
    }

    async fn get_first(
        &self,
        range: BtreeKeyRange<K>,
        _filter: Option<Arc<dyn GetFilter<K, V>>>,
    ) -> Result<Option<(K, V)>, BtreeError> {
        let part = self.route(&range.start_key, false);
        let shard = &self.shards[part.shard_id];
        shard_call!(shard, [range], get_first(range))
    }

    async fn query(
        &self,
        range: BtreeKeyRange<K>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<K, V>>>,
        reverse: bool,
    ) -> Result<Box<dyn IndexQueryHandle<K, V>>, BtreeError> {
        let mut partitions = self.route_range(&range);
        // For reverse queries, traverse partitions from highest to lowest
        // so fill_query_handle visits the largest keys first.
        if reverse {
            partitions.reverse();
        }

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
            let shard = &self.shards[handle.partitions[handle.cur_part_idx].shard_id];
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
        max_scan: usize,
    ) -> Result<(PutStats, bool), BtreeError> {
        // Route by scan_range.start_key: all versions of the same user key share identical
        // partition bytes (MvccKey<K: Partitionable> delegates to inner key), so all versions
        // land in the same shard. No cross-shard coordination needed.
        //
        // insert_key is owned (mut) and NOT in the shard_call clone list — it is captured by
        // move into the async block, where &mut insert_key is valid as a local borrow.
        // Pre-register BEFORE the btree write (see route() doc for race details).
        let entry = self.route(&scan_range.start_key, true);
        let shard = &self.shards[entry.shard_id];

        let result = shard_call!(shard, [value, scan_range, filter], scan_and_put_one(&mut insert_key, &value, &scan_range, filter.as_ref().map(|f| f.as_ref()), max_scan));
        match &result {
            Ok((stats, _)) => { entry.count.fetch_add(stats.count_delta() - 1, Ordering::Relaxed); }
            Err(_)         => { entry.count.fetch_add(-1, Ordering::Relaxed); }
        }
        result
    }
}

//==============================================================================
// PartitionCleaner — background task that lazily evicts truly-empty partitions.
//
// count is a hint (can go negative with inline GC). If count ≤ 0 we query the
// actual btree shard to confirm the partition has no data before removing it
// from the registry. This avoids evicting partitions with live MVCC history.
//
// Spawned by ShardedBtree::new(); stopped via `shutdown` flag on Drop.
// Uses the same cfg_if! pattern as MvccGc::spawn_background: iomgr::spawn for
// async_backend, std::thread::spawn for sync_backend.
//==============================================================================

struct PartitionCleaner<K: 'static + Partitionable, V: 'static + BtreeValue> {
    shards: Arc<Vec<Arc<Shard<K, V>>>>,
    registry: Arc<RwLock<BTreeMap<u32, Arc<Partition>>>>,
    partition_key_len: usize,
    shutdown: Arc<AtomicBool>,
}

impl<K: 'static + Partitionable, V: 'static + BtreeValue> PartitionCleaner<K, V> {
    fn spawn(self: Arc<Self>) {
        cfg_if::cfg_if! {
            if #[cfg(feature = "async_backend")] {
                iomgr::spawn(async move {
                    while !self.shutdown.load(Ordering::Relaxed) {
                        iomgr::sleep(Duration::from_secs(5)).await;
                        self.run_cycle().await;
                    }
                });
            } else {
                std::thread::spawn(move || {
                    while !self.shutdown.load(Ordering::Relaxed) {
                        std::thread::sleep(Duration::from_secs(5));
                        self.run_cycle();
                    }
                });
            }
        }
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn run_cycle(&self) {
        // Snapshot all count ≤ 0 candidates under read lock (no write lock held during btree query).
        let candidates: Vec<(u32, Arc<Partition>)> = {
            let reg = self.registry.read();
            reg.iter()
                .filter(|(_, e)| e.count.load(Ordering::Relaxed) <= 0)
                .map(|(k, e)| (*k, e.clone()))
                .collect()
        };

        for (part_id, entry) in candidates {
            if self.shutdown.load(Ordering::Relaxed) { break; }

            // Re-check count (a concurrent write may have bumped it since we snapshotted).
            let count_before = entry.count.load(Ordering::Relaxed);
            if count_before > 0 { continue; }

            // Build a range covering exactly this partition's key space.
            let first_key: K = entry.first_key();
            let end_key = match entry.next_first_key::<K>() {
                Some(k) => k,
                None    => continue, // all-0xFF key: can't determine upper bound, skip.
            };
            let range = BtreeKeyRange::new(first_key, true, end_key, false);
            let shard = &self.shards[entry.shard_id];

            // Query with batch_size=1; if result is empty the partition has no live data.
            let is_empty = shard_call!(shard, [range], query(range, 1, None))
                .map(|h| h.results.is_empty() && !h.has_more())
                .unwrap_or(false);

            if is_empty {
                // Only remove if count has not changed since the query started (no concurrent writes).
                let count_after = entry.count.load(Ordering::Relaxed);
                if count_after == count_before {
                    let mut reg = self.registry.write();
                    // Re-check under write lock in case a writer sneaked in.
                    if let Some(e) = reg.get(&part_id) {
                        if e.count.load(Ordering::Relaxed) <= 0 {
                            reg.remove(&part_id);
                        }
                    }
                }
            }
        }
    }

    /// Returns `partition_key_len` (kept for potential future use in cleaner logic).
    #[allow(dead_code)]
    fn partition_key_len(&self) -> usize { self.partition_key_len }
}
