/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

//! Btree High-Level Layer
//!
//! This is the upper layer of the btree architecture that handles ALL locking logic.
//! Storage layers (COWBtree, MemBtree) handle ONLY persistence/storage, NO locking.
//!
//! Architecture:
//! ```
//! Btree (this layer) - handles ALL locking
//!   ↓
//! UnderlyingBtree trait (COWBtree/MemBtree) - handles ONLY storage/persistence
//!   ↓
//! Node/NodeCore (core library) - tangent to both layers
//! ```

use std::io;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use async_trait::async_trait;
use iomgr::{AsyncRwLock, AsyncRwReadGuard, AsyncRwWriteGuard};
use tracing;

//================================================================================
// Global Operation Counter for Tracing
//================================================================================

/// Global operation counter for sequential operation IDs across all btree instances
static GLOBAL_OP_COUNTER: AtomicU64 = AtomicU64::new(0);

use super::btree_node::{BNodeId, Node, NodeCore};
use super::btree_kvs::{BtreeKey, BtreeValue};
use super::detail::btree_req::{
    BtreeKeyRange,
    BtreeRemoveRequest, BtreeRemoveAnyRequest, BtreeRangeRemoveRequest, RemoveFilterFn,
    BtreeGetRequest, BtreeGetAnyRequest, BtreeQueryRequest, QueryResultHandle, GetFilterFn,
    BtreeSinglePutRequest,BtreeRangePutRequest, BtreePutType, PutFilterFn,
};

//================================================================================
// Error Types
//================================================================================

#[derive(Debug)]
pub enum BtreeError {
    Retry,     // Lock upgrade failed due to concurrent modification - retry operation
    CpMismatch, // Checkpoint context mismatch - retry operation from root
    HasMore,   // Range operation has more entries to process, keep going
    KeyNotFound, // Key not found in the btree
    KeyAlreadyExists, // Key already exists in the btree
    NodeNotFound,     // Node not found in storage (completely internal error)
    Io(io::Error),    // I/O error from storage layer
}

//================================================================================
// Btree Configuration
//================================================================================

/// Btree configuration (matches C++ BtreeConfig)
#[derive(Debug, Clone)]
pub struct BtreeConfig {
    pub node_size: u32,
    pub ideal_fill_pct: u8,
    pub suggested_min_pct: u8,
    pub split_pct: u8,
    pub max_merge_nodes: u32,
    pub rebalance_turned_on: bool,
    pub merge_turned_on: bool,
    pub leaf_node_type: u8,
    pub int_node_type: u8,
    pub btree_name: String,
    
    // Prefix compression config (for PrefixCompressNode variant)
    pub expected_prefix_size: u16, // 0 = dynamic, >0 = fixed split point
    
    // Precomputed values
    pub suggested_min_size: u32,
    pub ideal_fill_size: u32,
}

impl BtreeConfig {
    pub fn new(node_size: u32, btree_name: String) -> Self {
        let mut config = Self {
            node_size,
            expected_prefix_size: 0, // Default to dynamic
            ideal_fill_pct: 90,
            suggested_min_pct: 30,
            split_pct: 50,
            max_merge_nodes: 3,
            rebalance_turned_on: false,
            merge_turned_on: true,
            leaf_node_type: 0, // Simple node
            int_node_type: 0,  // Simple node
            btree_name,
            suggested_min_size: 0,
            ideal_fill_size: 0,
        };
        config.finalize();
        config
    }
    
    fn finalize(&mut self) {
        let node_header_size = std::mem::size_of::<super::btree_node::PersistentHeader>() as u32;
        let usable_size = self.node_size - node_header_size;
        self.ideal_fill_size = (usable_size * self.ideal_fill_pct as u32) / 100;
        self.suggested_min_size = (usable_size * self.suggested_min_pct as u32) / 100;
    }
    
    pub fn split_size(&self, filled_size: u32) -> u32 {
        (filled_size * self.split_pct as u32) / 100
    }
    
    pub fn ideal_fill_size(&self) -> u32 {
        self.ideal_fill_size
    }
    
    pub fn suggested_min_size(&self) -> u32 {
        self.suggested_min_size
    }
}

//================================================================================
// UnderlyingBtree Trait
//================================================================================

/// Trait for underlying storage implementation (COWBtree, MemBtree)
///
/// Storage layers handle ONLY persistence/storage, NO locking.
/// All locking is handled by the Btree layer above.
#[async_trait]
pub trait UnderlyingBtree: Send + Sync {
    /// Read node from storage - returns UNLOCKED node
    ///
    /// The Btree layer is responsible for locking the returned node.
    async fn read_node(&self, id: BNodeId) -> Result<Arc<NodeCore>, BtreeError>;

    /// Write node to storage
    ///
    /// The node is already locked by the Btree layer before this call.
    async fn write_node(&self, node: &Node) -> Result<(), BtreeError>;

    /// Create new node in storage (storage allocates node_id internally)
    ///
    /// Returns UNLOCKED node that Btree layer will lock as needed.
    async fn create_node(&self, is_leaf: bool, node_type: u8) -> Result<Arc<NodeCore>, BtreeError>;

    /// Delete node from storage (for cleanup)
    async fn delete_node(&self, id: BNodeId) -> Result<(), BtreeError>;

    /// Notify storage that root node has changed (for metadata persistence)
    async fn on_root_changed(&self, root_node_id: BNodeId) -> Result<(), BtreeError>;
}

//================================================================================
// Tree Lock Guards
//================================================================================

/// Tree lock guard (shared) - ensures guard lives through operation scope
///
/// **CRITICAL**: This guard MUST be held for the entire operation scope!
/// If it drops early, the tree becomes unprotected while still operating.
pub struct TreeLockGuard<'a> {
    pub(super) _guard: AsyncRwReadGuard<'a, ()>,
}

/// Tree lock guard (exclusive) - for root split/collapse operations
///
/// **CRITICAL**: This guard MUST be held for the entire operation scope!
pub struct TreeLockGuardExclusive<'a> {
    pub(super) _guard: AsyncRwWriteGuard<'a, ()>,
}

//================================================================================
// Btree - Upper Layer (Handles ALL Locking)
//================================================================================

/// Btree upper layer - handles ALL locking logic
///
/// The storage layer (COWBtree/MemBtree) handles ONLY persistence/storage.
/// This layer is responsible for:
/// - Tree-wide locking (btree_lock)
/// - Node-level locking (via read_and_lock_node)
/// - Lock upgrades (via upgrade_node_lock/upgrade_node_locks functions)
pub struct Btree<K, V>
where
    K: BtreeKey,
    V: BtreeValue,
{
    /// Tree-wide lock (matches C++ m_btree_lock)
    ///
    /// Protects root node changes and tree structure modifications.
    /// - Shared lock: Normal operations (GET/PUT/REMOVE)
    /// - Exclusive lock: Root split/collapse
    pub(super) btree_lock: AsyncRwLock<()>,

    /// Btree configuration
    pub(super) config: BtreeConfig,

    /// Underlying storage (COWBtree or MemBtree)
    ///
    /// Handles persistence only, NO locking.
    /// pub(super) allows btree_node_mgr.rs to access for method implementations
    pub(super) storage: Box<dyn UnderlyingBtree>,

    /// Root node ID (AtomicU64 for thread-safe interior mutability)
    pub(super) root_node_id: AtomicU64,

    _phantom: std::marker::PhantomData<(K, V)>,
}

impl<K, V> Btree<K, V>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    /// Create a new Btree with the given configuration and storage backend
    /// 
    /// If root_node_id is None, creates a new root node
    pub async fn new(config: BtreeConfig, storage: Box<dyn UnderlyingBtree>, root_node_id: Option<BNodeId>) 
                   -> Result<Self, BtreeError> {
        // Create the btree first (root will be initialized after if needed)
        let btree = Self {
            btree_lock: AsyncRwLock::new(()),
            config,
            storage,
            root_node_id: AtomicU64::new(0), // Temporary, will be set if needed
            _phantom: std::marker::PhantomData,
        };
        
        // Handle root node creation/initialization
        let root_id = match root_node_id {
            Some(id) => id,
            None => {
                // Create and initialize new root node
                btree.create_root_node().await?
            }
        };
        
        btree.root_node_id.store(root_id, Ordering::Relaxed);
        Ok(btree)
    }

    /// Create a new root node (called during initialization or after root split)
    async fn create_root_node(&self) -> Result<BNodeId, BtreeError> {
        let root_core = self.storage.create_node(true, self.config.leaf_node_type).await?;
        
        // Initialize the node with proper variant
        let _root_node = self.init_new_variant_node(root_core.clone(), self.config.leaf_node_type).await?;
        
        let root_id = root_core.node_id();
        self.root_node_id.store(root_id, Ordering::Relaxed);
        self.storage.on_root_changed(root_id).await?;
        Ok(root_id)
    }

    //================================================================================
    // Public API Methods
    //================================================================================

    /// Insert or update a single key-value pair (matches C++ Btree::put)
    ///
    /// Returns:
    /// - PutResult::Success if new key was inserted
    /// - PutResult::Updated if existing key was updated
    #[tracing::instrument(
        skip(self, key, value, filter_fn), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), btree=%self.config.btree_name, key=?key))]
    pub async fn put_one(&self, key: &K, value: &V, filter_fn: Option<&PutFilterFn<K, V>>) 
        -> Result<(), BtreeError> {
        tracing::debug!("Starting put operation");
        let req = BtreeSinglePutRequest::new(key, value, BtreePutType::Upsert, filter_fn);
        let result = self.put_one_internal(&req).await;
        if result.is_ok() { tracing::info!("Put completed"); } else { tracing::warn!(?result, "Put failed"); }
        result
    }

    /// Range PUT - Update multiple keys in a range with the same value
    ///
    /// Inserts or updates all keys in the range [start, end) with the given value.
    ///
    /// # Arguments
    /// * `start` - Start key (inclusive)
    /// * `end` - End key (exclusive)
    /// * `value` - Value to insert/update for all keys in range
    /// * `filter_fn` - Optional filter function for conditional updates
    ///
    /// Returns:
    /// - PutResult::Success if operation completed successfully
    #[tracing::instrument(skip(self, start, end, value, filter_fn), 
        fields(op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), 
               btree=%self.config.btree_name, start=?start, end=?end))]

    pub async fn put_range(&self, start: &K, end: &K, value: &V, filter_fn: Option<&PutFilterFn<K, V>>)
        -> Result<(), BtreeError> {
        tracing::debug!("Starting range put");
        let range = BtreeKeyRange::new(start.clone(), true, end.clone(), false);
        let req = BtreeRangePutRequest::new(range, BtreePutType::Upsert, value, 1000, filter_fn);
        let result = self.put_range_internal(req).await;
        if result.is_ok() { tracing::info!("Range put completed"); } else { tracing::warn!("Range put failed"); }
        result
    }

    /// Get value for a single key (matches C++ Btree::get)
    ///
    /// Returns:
    /// - Some(value) if key exists
    /// - None if key not found
    #[tracing::instrument(
        skip(self, key), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), btree=%self.config.btree_name, key=?key))]

    pub async fn get(&self, key: &K) -> Result<Option<V>, BtreeError> {
        tracing::debug!("Starting get operation");
        let req = BtreeGetRequest::new(key);
        let result = self.get_one_internal(&req).await;
        match &result { 
            Ok(Some(_)) => tracing::debug!("Key found"), 
            Ok(None) => tracing::debug!("Key not found"), 
            Err(_) => tracing::warn!("Get failed") 
        }
        result
    }
    
    //================================================================================
    // Remove Operations
    //================================================================================

    /// Remove a single key-value pair
    ///
    /// # Arguments
    /// * `key` - Key to remove
    ///
    /// # Returns
    /// * `Ok(Some(value))` - Key found and removed, returns the value
    /// * `Ok(None)` - Key not found
    /// * `Err(BtreeError)` - Error occurred
    #[tracing::instrument(
        skip(self, key), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), btree=%self.config.btree_name, key=?key))]

    pub async fn remove_one(&self, key: &K) -> Result<Option<V>, BtreeError> {
        tracing::debug!("Starting remove operation");
        let req = BtreeRemoveRequest::new(key);
        let result = self.remove_one_internal(req).await;
        match &result { Ok(Some(_)) => tracing::info!("Key removed successfully"), Ok(None) => tracing::debug!("Key not found"), Err(_) => tracing::warn!("Remove failed") }
        result
    }

    /// Remove any one key-value pair in the given range
    ///
    /// If the range matches multiple keys, randomly picks one and removes it.
    ///
    /// # Arguments
    /// * `range` - Key range to search
    ///
    /// # Returns
    /// * `Ok(Some((key, value)))` - Found and removed a key-value pair
    /// * `Ok(None)` - No keys found in range
    /// * `Err(BtreeError)` - Error occurred
    #[tracing::instrument(
        skip(self, range), 
        fields(op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), btree = %self.config.btree_name))]

    pub async fn remove_any(&self, range: BtreeKeyRange<K>) -> Result<Option<(K, V)>, BtreeError> {
        tracing::debug!("Starting remove_any operation");
        let req = BtreeRemoveAnyRequest::new(range);
        let result = self.remove_any_internal(req).await;
        match &result { Ok(Some((k, _))) => tracing::info!(key = ?k, "Removed one key from range"), Ok(None) => tracing::debug!("No keys in range"), Err(_) => tracing::warn!("Remove_any failed") }
        result
    }

    /// Remove all keys in the given range (with pagination)
    ///
    /// # Arguments
    /// * `range` - Key range to remove
    /// * `batch_size` - Maximum number of keys to remove per batch
    /// * `filter_fn` - Optional filter function to select which entries to remove
    ///
    /// # Returns
    /// * `Ok(count)` - Number of keys removed
    /// * `Err(BtreeError::HasMore)` - More keys to remove (call again)
    /// * `Err(BtreeError)` - Error occurred
    #[tracing::instrument(
        skip(self, range, filter_fn), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), 
                 btree=%self.config.btree_name, batch_size=batch_size))]

    pub async fn remove_range(&self, range: BtreeKeyRange<K>, batch_size: u32,
                              filter_fn: Option<&RemoveFilterFn<K, V>>) 
        -> Result<u32, BtreeError> {
        tracing::debug!("Starting range remove");
        let req = BtreeRangeRemoveRequest::new(range, batch_size, filter_fn);
        let result = self.remove_range_internal(req).await;
        match &result { 
            Ok(count) => tracing::info!(removed_count = count, "Range remove completed"), 
            Err(BtreeError::HasMore) => tracing::debug!("Range remove: more entries to remove"), 
            Err(_) => tracing::warn!("Range remove failed") 
        }
        result
    }

    /// Get any key-value pair in the given range (optimization, matches C++ Btree::get_any)
    /// 
    /// Returns the first match found during traversal.
    /// Useful when you need to check existence or get a sample from a range.
    ///
    /// # Arguments
    /// * `start` - Start key (inclusive)
    /// * `end` - End key (exclusive)
    ///
    /// Returns:
    /// - Some((key, value)) if any key exists in range
    /// - None if range is empty
    #[tracing::instrument(
        skip(self, start, end), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), 
               btree=%self.config.btree_name, start=?start, end=?end))]

    pub async fn get_any(&self, start: &K, end: &K) -> Result<Option<(K, V)>, BtreeError> {
        tracing::debug!(start = ?start, end = ?end, "Starting get_any");
        let range = BtreeKeyRange::new(start.clone(), true, end.clone(), false);
        let req = BtreeGetAnyRequest::new(range);
        let result = self.get_any_internal(&req).await;
        match &result { 
            Ok(Some((k, _))) => tracing::debug!(key = ?k, "Found key in range"), 
            Ok(None) => tracing::debug!("No keys in range"), 
            Err(_) => tracing::warn!("Get_any failed") 
        }
        result
    }

    /// Sweep query - returns multiple key-value pairs in range
    /// 
    /// 
    /// Efficiently collects key-value pairs by following leaf node sibling links.
    /// Returns up to `batch_size` results. Use `query_next()` for pagination if has_more() is true.
    /// 
    /// # Arguments
    /// * `range` - Key range to query
    /// * `batch_size` - Maximum number of results to return
    /// 
    /// # Returns
    /// * `Ok(QueryResultHandle)` - Handle with results and pagination state
    /// * `Err(BtreeError)` - Internal errors
    /// 
    /// # Example
    /// ```rust,ignore
    /// let range = BtreeKeyRange::new(10, true, 100, false);
    /// let handle = btree.query(range, 50).await?;
    /// 
    /// for (key, value) in &handle.results {
    ///     println!("key: {}, value: {}", key, value);
    /// }
    /// 
    /// if handle.has_more() {
    ///     let next_handle = btree.query_next(handle).await?;
    ///     // Process next batch...
    /// }
    /// ```
    #[tracing::instrument(
        skip(self, range, filter_fn), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), 
               btree=%self.config.btree_name, range=?range))]

    pub async fn query<'a>(&self, range: BtreeKeyRange<K>, batch_size: u32,
                       filter_fn: Option<&'a GetFilterFn<K, V>>) 
        -> Result<QueryResultHandle<'a, K, V>, BtreeError> {
        tracing::debug!("Starting query");
        let req = BtreeQueryRequest::new(range, batch_size, filter_fn, /*reverse_order=*/false);
        let result = self.query_internal(req).await;
        if let Ok(ref handle) = result { tracing::debug!(result_count = handle.results.len(), has_more = handle.has_more(), "Query completed"); }
        result
    }

    /// Query with tree traversal (supports reverse iteration, no sibling links)
    /// 
    /// Executes a range query using tree traversal without following sibling links.
    /// This method is required for reverse iteration and avoids potential deadlocks
    /// in scenarios like TiKV integration.
    /// 
    /// # Arguments
    /// * `range` - Key range to query
    /// * `batch_size` - Maximum results per batch
    /// * `filter_fn` - Optional filter function
    /// * `reverse_order` - If true, iterate in reverse order (high to low)
    /// 
    /// # Returns
    /// * `QueryResultHandle` - Contains results and has_more() indicator for pagination
    #[tracing::instrument(
        skip(self, range, filter_fn), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), 
               btree=%self.config.btree_name, range=?range, reverse=reverse_order))]
    pub async fn query_traversal<'a>(&self, range: BtreeKeyRange<K>, batch_size: u32,
                                     filter_fn: Option<&'a GetFilterFn<K, V>>, reverse_order: bool) 
        -> Result<QueryResultHandle<'a, K, V>, BtreeError> {
        tracing::debug!("Starting traversal query");
        let req = BtreeQueryRequest::new(range, batch_size, filter_fn, reverse_order);
        let result = self.traversal_query_internal(req).await;
        if let Ok(ref handle) = result { tracing::debug!(result_count = handle.results.len(), has_more = handle.has_more(), "Traversal query completed"); }
        result
    }

    /// Paginate for next batch of results on previous query result handle.
    /// If there are no more results, returns an empty result handle.
    /// If there are more results, returns a new result handle which user is expected to call next time.
    /// 
    /// Routes to the appropriate query method (sweep or traversal) based on the request.
    #[tracing::instrument(
        skip(self, handle), 
        fields(op_id=GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed), 
               btree=%self.config.btree_name))]

    pub async fn query_next_batch<'a>(&self, handle: QueryResultHandle<'a, K, V>) 
        -> Result<QueryResultHandle<'a, K, V>, BtreeError> {
        let req = handle.request();
        if req.reverse_order() {
            // Use traversal query for reverse iteration
            self.traversal_query_internal(req).await
        } else {
            // Use sweep query for forward iteration
            self.query_internal(req).await
        }
    }
}

//================================================================================
// Usage Examples (for documentation)
//================================================================================

/// Example: Simple read operation (GET)
///
/// ```ignore
/// // Btree layer acquires tree lock, gets unlocked node from storage, locks it
/// let _tree_lock = btree.lock_tree_shared().await;  // MUST keep in scope!
/// let node = btree.read_and_lock_node(node_id, LockType::Read).await?;
///
/// // Use the locked node
/// let value: MyValue = node.get_nth_value(index);
/// drop(node); // Auto-unlock via RAII
/// // tree_lock still held until function exits
/// ```
///
/// Example: Write operation with lock upgrade (PUT/REMOVE)
///
/// ```ignore
/// // Acquire shared tree lock (normal operation)
/// let _tree_lock = btree.lock_tree_shared().await;
///
/// // Read parent and child with ReadInteriorWriteLeaf
/// // Interior nodes get READ lock, leaf nodes get WRITE lock
/// let parent = btree.read_and_lock_node(parent_id, LockType::ReadInteriorWriteLeaf).await?;
/// let child = btree.read_and_lock_node(child_id, LockType::ReadInteriorWriteLeaf).await?;
///
/// if child.needs_split() {
///     // Upgrade both to WRITE locks
///     let (parent, child) = upgrade_node_locks(parent, child).await?;
///
///     // Now both have WRITE locks - can mutate
///     split_node(&parent, &child)?;
/// }
/// ```
///
/// Example: Root split operation
///
/// ```ignore
/// // Acquire EXCLUSIVE tree lock for root changes
/// let _tree_lock = btree.lock_tree_exclusive().await;
///
/// // Lock root with WRITE
/// let root = btree.read_and_lock_node(root_id, LockType::Write).await?;
///
/// // Perform root split
/// create_new_root(&root)?;
/// ```
pub(crate) mod examples {}