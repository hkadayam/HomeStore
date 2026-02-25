//! LockFreeBtree: routes get/put/remove to per-reactor Btrees via execute.
//! Built for async_code and sync_over_async (not sync_code). sync_over_async uses spawn_and_block;
//! async_code uses spawn_waitable.
//!
//! ## How async_op! protects closures from maybe_async_cfg
//!
//! `maybe_async_cfg` in sync mode strips `async` from every `async move {}` block it sees in the
//! function body. Wrapping the closure in a `macro_rules!` invocation (`async_op!(…)`) hides the
//! tokens from the proc macro — proc macros cannot recurse into opaque macro invocations. The
//! `async_op!` macro is expanded later (after the proc macro finishes), so the `async move {}` and
//! inner `.await` survive intact. Only the outer `.await` on `self.execute(…)` is stripped.

use std::future::Future;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use smallvec::SmallVec;
use std::hash::{Hash, Hasher};

use homestore::index::btree::btree::Btree;
use homestore::index::btree::btree_kvs::BtreeKey;
use homestore::index::btree::btree_types::{BtreeError, BtreeConfig};
use homestore::index::btree::detail::btree_req::{BtreeKeyRange, GetFilter, PutFilter, QueryResultHandle, RemoveFilter};
use iomgr::{spawn_and_block, ReactorTarget};
use homestore::index::btree::UnderlyingBtree;

#[cfg(feature = "async_frontend")]
use iomgr::spawn_waitable;

use super::btree_index::BtreeIndex;
use super::db_kv::{DbKey, DbValue};

/// Wraps a body in `move || async move { … }`.
///
/// Because this is a `macro_rules!` invocation, `maybe_async_cfg` (a proc macro) cannot see
/// inside its argument — it treats the whole `async_op!(…)` as an opaque expression and leaves
/// it untouched. The closure and inner `.await` are therefore preserved in sync mode.
macro_rules! async_op {
    ($($body:tt)*) => { move || async move { $($body)* } };
}

/// Per-reactor Btrees; implements BtreeIndex by routing each op to the right reactor.
pub struct LockFreeBtree {
    btrees: Vec<Arc<Btree<DbKey, DbValue>>>,
    partition_key_len: usize,
    home_reactor: usize,
}

/// Represents a partition of the btree to be placed on a single reactor to run them as single-threaded btree
pub struct Partition {
    part_key: SmallVec<[u8; 32]>,
}

impl Partition {
    pub fn from_part_key(part_key: SmallVec<[u8; 32]>) -> Self { Self { part_key } }
    pub fn empty() -> Self { Self::from_part_key(SmallVec::new()) }
    pub fn from_key(key: &DbKey, partition_key_len: usize) -> Self {
        let bytes = key.as_bytes();
        let p_len = if partition_key_len == 0 { bytes.len() } else { partition_key_len.min(bytes.len()) };
        Partition::from_part_key(bytes.iter().take(p_len).copied().collect())
    }
    pub fn from_range(range: &BtreeKeyRange<DbKey>, partition_key_len: usize) -> Self {
        if partition_key_len == 0 {
            return Self::empty();
        }
        Self::from_key(&range.start_key, partition_key_len)
    }

    /// First key of this partition (the partition part_key as DbKey).
    pub fn first_key(&self) -> DbKey {
        <DbKey as BtreeKey>::deserialize_from(self.part_key.as_ref(), true).expect("DbKey from partition key")
    }

    /// Next partition in key order (lexicographic). None if overflow (e.g. all 0xff).
    pub fn next(&self) -> Option<Partition> {
        let len = self.part_key.len();
        if len == 0 {
            return None;
        }
        let mut next_part_key = self.part_key.clone();
        for i in (0..len).rev() {
            let (v, carry) = next_part_key[i].overflowing_add(1);
            next_part_key[i] = v;
            if !carry {
                return Some(Partition::from_part_key(next_part_key));
            }
        }
        None
    }

    pub fn is_out_of_range(&self, range: &BtreeKeyRange<DbKey>) -> bool {
        if range.end_incl { self.first_key() > range.end_key } else { self.first_key() >= range.end_key }
    }

    fn as_bytes(&self) -> &[u8] { self.part_key.as_ref() }
}

impl Hash for Partition {
    fn hash<H: Hasher>(&self, state: &mut H) { self.part_key.as_ref().hash(state); }
}

/// Partition query handle: merged results from multiple partitions.
pub struct PartitionQueryHandle {
    pub results: Vec<(DbKey, DbValue)>,

    pub input_range: BtreeKeyRange<DbKey>,
    pub batch_size: u32,
    pub filter: Option<Arc<dyn GetFilter<DbKey, DbValue>>>,
    pub reverse: bool,

    pub cur_handle: Option<QueryResultHandle<DbKey, DbValue>>,
    pub cur_partition: Option<Partition>,
}

impl PartitionQueryHandle {
    /// Empty handle (no partitions in range).
    pub fn empty(range: BtreeKeyRange<DbKey>) -> Self {
        Self {
            results: vec![],
            input_range: range,
            batch_size: 0,
            filter: None,
            reverse: false,
            cur_handle: None,
            cur_partition: None,
        }
    }

    pub fn has_more(&self) -> bool {
        let more_in_current =
            self.cur_handle.as_ref().map_or(false, |h: &QueryResultHandle<DbKey, DbValue>| h.has_more());
        let more_partitions = self
            .cur_partition
            .as_ref()
            .map_or(false, |p| p.next().as_ref().map_or(false, |n| !n.is_out_of_range(&self.input_range)));
        more_in_current || more_partitions
    }
}

impl super::btree_index::IndexQueryHandle for PartitionQueryHandle {
    fn results(&self) -> &[(DbKey, DbValue)] { self.results.as_slice() }

    fn has_more(&self) -> bool { PartitionQueryHandle::has_more(self) }
}

fn to_partition_query_handle(
    handle: Box<dyn super::btree_index::IndexQueryHandle>,
) -> Result<PartitionQueryHandle, BtreeError> {
    super::btree_index::index_query_handle_into_any(handle)
        .downcast::<PartitionQueryHandle>()
        .map(|b| *b)
        .map_err(|_| {
            BtreeError::Io(std::io::Error::new(
                std::io::ErrorKind::Other,
                "LockFreeBtree requires PartitionQueryHandle",
            ))
        })
}

//================================================================================
// execute_on — free function so new() can use it before `self` exists.
// Sync: blocks on target reactor. Async: spawns and awaits.
//================================================================================

#[cfg(not(feature = "async_frontend"))]
fn execute_on<F, Fut, R>(reactor_id: usize, op: F) -> R
where
    F: FnOnce() -> Fut,
    Fut: Future<Output = R> + Send + 'static,
    R: Send + 'static,
{
    spawn_and_block(ReactorTarget::Reactor(reactor_id), op())
}

#[cfg(feature = "async_frontend")]
async fn execute_on<F, Fut, R>(reactor_id: usize, op: F) -> R
where
    F: FnOnce() -> Fut,
    Fut: Future<Output = R> + Send + 'static,
    R: Send + 'static,
{
    spawn_waitable(ReactorTarget::Reactor(reactor_id), op()).await
}

//================================================================================
// LockFreeBtree impl
//================================================================================

static NEXT_HOME_REACTOR: AtomicUsize = AtomicUsize::new(0);

impl LockFreeBtree {
    /// Builds from config, partition_key_len, and a per-partition storage factory.
    /// Caller provides storage (e.g. MemBtree in mem_db). Homestore must be async_code.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(
        config: BtreeConfig,
        partition_key_len: usize,
        storage_factory: impl Fn(BtreeConfig) -> Box<dyn UnderlyingBtree>,
    ) -> Result<Self, BtreeError> {
        let num_reactors = std::cmp::max(1, iomgr::iomgr().num_reactors());
        let mut btrees = Vec::with_capacity(num_reactors);

        for i in 0..num_reactors {
            let mut cfg = config.clone();
            cfg.btree_name = format!("{}_{}", config.btree_name, i);
            cfg.is_single_threaded = true;
            let storage = storage_factory(cfg.clone());
            let btree =
                execute_on(i, async_op!(Btree::<DbKey, DbValue>::new(cfg, storage, None).await)).await?;
            btrees.push(Arc::new(btree));
        }

        let home_reactor = NEXT_HOME_REACTOR.fetch_add(1, Ordering::Relaxed) % num_reactors;
        Ok(Self { btrees, partition_key_len, home_reactor })
    }

    fn num_reactors(&self) -> usize { self.btrees.len() }

    #[inline]
    fn route(&self, key: &DbKey) -> usize { self.get_reactor(&Partition::from_key(key, self.partition_key_len)) }

    /// Thin wrapper around execute_on for use by methods that already have `self`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    #[inline]
    async fn execute<F, Fut, R>(&self, reactor_id: usize, op: F) -> R
    where
        F: FnOnce() -> Fut,
        Fut: Future<Output = R> + Send + 'static,
        R: Send + 'static,
    {
        execute_on(reactor_id, op).await
    }

    /// Run query for the current partition (handle.cur_partition); fill handle.results and set
    /// handle.cur_handle. Caller ensures handle.cur_partition is valid and is inside the input range.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    async fn query_one_partition(&self, handle: &mut PartitionQueryHandle) -> Result<(), BtreeError> {
        let partition = handle.cur_partition.as_ref().expect("cur_partition set");
        let reactor_id = self.get_reactor(partition);
        let this_range = self.clamp_range(&handle.input_range, partition);

        let btree = Arc::clone(&self.btrees[reactor_id]);
        let remaining = handle.batch_size - (handle.results.len() as u32);
        let filter = handle.filter.clone();
        let reverse = handle.reverse;
        let mut inner_handle = self
            .execute(reactor_id, async_op!(
                if reverse {
                    btree.query_traversal(this_range, remaining, filter, reverse).await
                } else {
                    btree.query(this_range, remaining, filter).await
                }
            ))
            .await?;

        handle.results.append(&mut inner_handle.results);
        handle.cur_handle = if inner_handle.has_more() { Some(inner_handle) } else { None };
        Ok(())
    }

    //////////////////////// Helper methods ////////////////////////
    #[inline]
    fn clamp_range(&self, range: &BtreeKeyRange<DbKey>, partition: &Partition) -> BtreeKeyRange<DbKey> {
        if self.partition_key_len == 0 {
            return range.clone();
        }

        let mut ret = range.clone();
        let this_first = partition.first_key();
        if range.start_key < this_first {
            ret.start_key = this_first;
            ret.start_incl = true;
        }

        if let Some(next_part) = partition.next() {
            let next_first = next_part.first_key();
            if range.end_key >= next_first {
                ret.end_key = next_first;
                ret.end_incl = false;
            }
        }
        ret
    }

    #[inline]
    fn get_reactor(&self, partition: &Partition) -> usize {
        if self.num_reactors() <= 1 {
            return 0;
        }
        use std::collections::hash_map::DefaultHasher;
        let mut h = DefaultHasher::new();
        partition.hash(&mut h);
        let hash = h.finish();
        ((hash as usize) % self.num_reactors() + self.home_reactor) % self.num_reactors()
    }
}

#[cfg_attr(feature = "async_frontend", async_trait::async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
impl BtreeIndex for LockFreeBtree {
    async fn put(&self, key: &DbKey, value: &DbValue) -> Result<(), BtreeError> {
        let reactor_id = self.route(key);

        let btree = Arc::clone(&self.btrees[reactor_id]);
        let key = key.clone();
        let value = value.clone();
        self.execute(reactor_id, async_op!(btree.put_one(&key, &value, None).await)).await
    }

    async fn put_range(
        &self,
        range: BtreeKeyRange<DbKey>,
        value: &DbValue,
        filter: Option<Arc<dyn PutFilter<DbKey, DbValue>>>,
    ) -> Result<(), BtreeError> {
        let mut cur_part = Some(Partition::from_range(&range, self.partition_key_len));

        while let Some(ref part) = cur_part {
            if part.is_out_of_range(&range) {
                break;
            }

            let reactor_id = self.get_reactor(part);
            let this_range = self.clamp_range(&range, part);

            let btree = Arc::clone(&self.btrees[reactor_id]);
            let value = value.clone();
            let filter = filter.clone();
            self.execute(reactor_id, async_op!(
                btree.put_range(this_range, &value, filter.as_ref().map(|a| a.as_ref())).await
            ))
            .await?;

            cur_part = part.next();
        }

        Ok(())
    }

    async fn remove(&self, key: &DbKey) -> Result<Option<DbValue>, BtreeError> {
        let reactor_id = self.route(key);
        let btree = Arc::clone(&self.btrees[reactor_id]);
        let key = key.clone();
        self.execute(reactor_id, async_op!(btree.remove_one(&key, None).await)).await
    }

    async fn remove_range(
        &self,
        range: BtreeKeyRange<DbKey>,
        filter: Option<Arc<dyn RemoveFilter<DbKey, DbValue>>>,
    ) -> Result<u32, BtreeError> {
        let mut removed_count: u32 = 0;
        let mut cur_part = Some(Partition::from_range(&range, self.partition_key_len));

        while let Some(ref part) = cur_part {
            if part.is_out_of_range(&range) {
                break;
            }
            let reactor_id = self.get_reactor(part);
            let this_range = self.clamp_range(&range, part);

            let btree = Arc::clone(&self.btrees[reactor_id]);
            let filter = filter.clone();
            removed_count += self
                .execute(reactor_id, async_op!(
                    btree.remove_range(this_range, filter.as_ref().map(|a| a.as_ref())).await
                ))
                .await?;
            cur_part = part.next();
        }
        Ok(removed_count)
    }

    async fn get(&self, key: &DbKey) -> Result<Option<DbValue>, BtreeError> {
        let reactor_id = self.route(key);
        let btree = Arc::clone(&self.btrees[reactor_id]);
        let key = key.clone();
        self.execute(reactor_id, async_op!(btree.get(&key).await)).await
    }

    async fn query(
        &self,
        range: BtreeKeyRange<DbKey>,
        batch_size: u32,
        filter: Option<Arc<dyn GetFilter<DbKey, DbValue>>>,
        reverse: bool,
    ) -> Result<Box<dyn super::btree_index::IndexQueryHandle>, BtreeError> {
        let range_clone = range.clone();
        let mut handle = PartitionQueryHandle {
            results: Vec::new(),
            input_range: range,
            batch_size,
            filter,
            reverse,
            cur_handle: None,
            cur_partition: Some(Partition::from_range(&range_clone, self.partition_key_len)),
        };

        while handle.cur_partition.as_ref().map_or(false, |p: &Partition| !p.is_out_of_range(&range_clone))
            && (handle.results.len() as u32) < handle.batch_size
        {
            self.query_one_partition(&mut handle).await?;

            // If there are more results within this partition itself, we are done for this batch.
            if handle.cur_handle.is_some() {
                break;
            }
            handle.cur_partition = handle.cur_partition.unwrap().next()
        }
        Ok(Box::new(handle))
    }

    async fn query_next_batch(
        &self,
        h: Box<dyn super::btree_index::IndexQueryHandle>,
    ) -> Result<Box<dyn super::btree_index::IndexQueryHandle>, BtreeError> {
        let mut handle = to_partition_query_handle(h)?;

        if let Some(inner_handle) = handle.cur_handle.take() {
            let cur_part = handle.cur_partition.as_ref().expect("cur_partition set");
            let reactor_id = self.get_reactor(cur_part);

            let btree = Arc::clone(&self.btrees[reactor_id]);
            let mut next_handle =
                self.execute(reactor_id, async_op!(btree.query_next_batch(inner_handle).await)).await?;
            handle.results = std::mem::take(&mut next_handle.results);

            if next_handle.has_more() {
                handle.cur_handle = Some(next_handle);
                return Ok(Box::new(handle));
            }

            // Fall through to next partition
            handle.cur_handle = None;
            handle.cur_partition = cur_part.next();
        }

        while handle.cur_partition.as_ref().map_or(false, |p: &Partition| !p.is_out_of_range(&handle.input_range))
            && (handle.results.len() as u32) < handle.batch_size
        {
            self.query_one_partition(&mut handle).await?;

            // If there are more results within this partition itself, we are done for this batch.
            if handle.cur_handle.is_some() {
                break;
            }
            handle.cur_partition = handle.cur_partition.unwrap().next()
        }
        Ok(Box::new(handle))
    }
}
