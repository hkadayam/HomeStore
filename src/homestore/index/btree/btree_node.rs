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

//! B-tree Node Implementation
//!
//! This module provides a non-persistent version of BtreeNode for the COW B-tree.
//! The persistent header portion is intentionally skipped for this initial port.
//! 
//! Key design choices:
//! - Uses `ArcSwap<Vec<u8>>` for lock-free COW semantics on the physical buffer
//! - Atomic operations for modified_cp_id and other metadata
//! - Simple buffer management without complex memory pooling initially

use std::sync::Arc;
use iomgr::AsyncRwLock;
use super::btree_kvs::{BtreeKey, BtreeValue};
use super::variant;
use super::detail::btree_req::{GetFilterFn, PutFilterFn, RemoveFilterFn};

//================================================================================
// Pagination Status for multi_get
//================================================================================

/// Status returned by multi_get to indicate pagination state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PaginationStatus {
    /// Range query completed - no more results in this node or siblings
    Completed,
    /// More results available, stopped due to max_count limit
    Continue,
    /// Unknown - reached end of current node, may have more in siblings
    Unknown,
}

//================================================================================
// Lock Types and Core Structures
//================================================================================

/// Lock type for node locking (matches C++ locktype_t)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LockType {
    None,                    // No lock (initial state, used in lock upgrade patterns)
    Read,                    // Read lock for both interior and leaf nodes
    Write,                   // Write lock for both interior and leaf nodes
    ReadInteriorWriteLeaf,   // Common pattern: READ interior, WRITE leaf (PUT/REMOVE ops)
}

/// Inner empty struct for lock (public for btree_base module)
pub struct NodeInner {}

/// Node ID type (64-bit)
/// [63:48] - 16 bits: btree_ordinal
/// [47]    - 1 bit: overflow_bit
/// [46:0]  - 47 bits: node_number
pub type BNodeId = u64;

/// BtreeNodePtr - Shared pointer to NodeCore
pub type BtreeNodePtr = triomphe::Arc<NodeCore>;

/// Compact node ID (48-bit, 6 bytes) - used for persistent storage to save space
/// This is the node_number portion (47 bits) + overflow_bit (1 bit)
pub type CompactNodeId = [u8; 6];

/// Pack a u64 into 48-bit representation (6 bytes, little-endian)
#[inline]
pub fn pack_compact_id(id: u64) -> CompactNodeId {
    debug_assert!(id < (1u64 << 48), "id {} exceeds 48-bit limit", id);
    [
        (id & 0xFF) as u8,
        ((id >> 8) & 0xFF) as u8,
        ((id >> 16) & 0xFF) as u8,
        ((id >> 24) & 0xFF) as u8,
        ((id >> 32) & 0xFF) as u8,
        ((id >> 40) & 0xFF) as u8,
    ]
}

/// Unpack 48-bit representation (6 bytes, little-endian) into u64
#[inline]
pub fn unpack_compact_id(bytes: CompactNodeId) -> u64 {
    (bytes[0] as u64)
        | ((bytes[1] as u64) << 8)
        | ((bytes[2] as u64) << 16)
        | ((bytes[3] as u64) << 24)
        | ((bytes[4] as u64) << 32)
        | ((bytes[5] as u64) << 40)
}

/// Sentinel value for empty node ID
pub const EMPTY_BNODEID: BNodeId = u64::MAX;

/// Internal guard enum (public for btree_base module) - holds actual lock guards
pub enum InternalLockGuard {
    Read(iomgr::AsyncRwReadGuard<'static, NodeInner>),
    Write(iomgr::AsyncRwWriteGuard<'static, NodeInner>),
    None,  // For unlocked nodes from storage layer
}

//================================================================================
// Node Operations Trait (for variant dispatch)
//================================================================================

/// Trait for node operations (matches C++ BtreeNode virtual methods)
/// Implementations receive &NodeCore, NOT Arc<NodeCore> to avoid memory bloat
///
/// Generic parameters K, V are at trait level (not method level) to enable dyn compatibility
pub trait NodeOps<K: BtreeKey, V: BtreeValue>: Send + Sync {
    /// Initialize a newly created node's internal structure
    /// Called once after node creation to set up initial state (nentries=0, edge=invalid, etc.)
    /// Default implementation does nothing - variants override if needed
    fn init_new_node(&self, _core: &NodeCore) {
        // Default: no initialization needed
    }

    fn get_all_kvs(&self, core: &NodeCore) -> Vec<(K, V)>;

    fn insert(&self, core: &NodeCore, idx: u32, key: &K, val: &V) -> Result<(), super::btree::BtreeError>;

    fn remove(&self, core: &NodeCore, idx: u32) -> Result<(), super::btree::BtreeError>;

    fn get_nth_key(&self, core: &NodeCore, idx: u32, copy: bool) -> K;

    fn get_nth_value(&self, core: &NodeCore, idx: u32, copy: bool) -> V;

    fn update(&self, core: &NodeCore, idx: u32, val: &V) -> Result<(), super::btree::BtreeError>;

    /// Update both key and value at index atomically (C++ update(idx, key, val))
    fn update_with_key(&self, core: &NodeCore, idx: u32, key: &K, val: &V) -> Result<(), super::btree::BtreeError>;

    // Range operations
    fn remove_range(&self, core: &NodeCore, start_idx: u32, end_idx: u32) -> Result<(), super::btree::BtreeError>;
    
    fn remove_all(&self, core: &NodeCore);

    // Node split/merge operations
    fn move_out_to_right_by_entries(&self, src_core: &NodeCore, dst_core: &NodeCore, nentries: u32) -> u32;
    
    fn move_out_to_right_by_size(&self, src_core: &NodeCore, dst_core: &NodeCore, size: u32) -> u32;
    
    fn append_copy_in_upto_size(&self, dst_core: &NodeCore, src_core: &NodeCore, 
                                other_cursor: &mut u32, upto_size: u32, copy_only_if_fits: bool) -> bool;

    // Space management
    fn available_size(&self, core: &NodeCore) -> u32;
    
    fn has_room_for_put(&self, core: &NodeCore, put_type: super::detail::btree_req::BtreePutType, 
                        key_size: u32, value_size: u32) -> bool;

    // Search operations with default implementations
    
    /// Find key position (binary search) - default implementation
    /// Override only if variant needs special search logic
    fn find(&self, core: &NodeCore, key: &K) -> (bool, u32) {
        let nentries = core.get_persistent_header().nentries();
        if nentries == 0 {
            return (false, 0);
        }

        let mut left = 0;
        let mut right = nentries;

        while left < right {
            let mid = left + (right - left) / 2;
            let mid_key = self.get_nth_key(core, mid, /*copy=*/true);

            match key.cmp(&mid_key) {
                std::cmp::Ordering::Less => right = mid,
                std::cmp::Ordering::Equal => return (true, mid),
                std::cmp::Ordering::Greater => left = mid + 1,
            }
        }

        (false, left)
    }

    /// Match a key range to indices - default implementation
    /// Override only if variant needs special range matching logic
    fn match_range(&self, core: &NodeCore, range: &super::detail::btree_req::BtreeKeyRange<K>) 
        -> (bool, u32, u32) {
        // Get the start index (C++ line 252)
        let (sfound, mut start_idx) = self.find(core, &range.start_key);
        if sfound && !range.start_incl {
            start_idx += 1;
        }

        let nentries = core.get_persistent_header().nentries();
        let is_leaf = core.is_leaf();
        let has_edge = core.get_persistent_header().edge_id != EMPTY_BNODEID;

        // Check if we're at the end (C++ lines 258-261)
        if start_idx == nentries {
            return (!is_leaf && has_edge, start_idx, start_idx);
        }

        // Get the end index (C++ line 265)
        let (efound, mut end_idx) = self.find(core, &range.end_key);
        
        // Adjust end_idx based on inclusivity (C++ lines 266-278)
        if is_leaf || (end_idx == nentries && !has_edge) {
            if !efound || !range.end_incl {
                if end_idx == 0 {
                    return (false, start_idx, end_idx);
                }
                end_idx -= 1;
            }
        } else if !range.end_incl && efound {
            if end_idx == 0 {
                return (false, start_idx, end_idx);
            }
            end_idx -= 1;
        }

        // Check if range is valid
        if start_idx > end_idx {
            return (false, start_idx, end_idx);
        }

        (true, start_idx, end_idx)
    }

    // High-level put operations with default implementations
    // (Override only for variants with special needs like PrefixNode)
    
    /// Put a single key-value pair (default implementation)
    /// 
    /// Matches C++ variant_node.hpp::put() (lines 202-237)
    /// Override only if variant needs special logic (e.g., PrefixNode)
    fn put(&self, core: &NodeCore, key: &K, value: &V, put_type: super::detail::btree_req::BtreePutType,
           filter_fn: Option<&PutFilterFn<K, V>>) -> Result<(), super::btree::BtreeError> {
        use super::detail::btree_req::{BtreePutType, PutFilterDecision};
        use super::btree::BtreeError;
        
        debug_assert!(core.is_leaf(), "Put operation on node is supported only for leaf nodes");
        
        // Find the key
        let (found, idx) = self.find(core, key);
        
        // If found and filter provided, apply filter
        if found {
            if let Some(filter) = filter_fn {
                let existing_val = self.get_nth_value(core, idx, /*copy=*/true);
                match filter(key, &existing_val, value) {
                    PutFilterDecision::Keep => return Ok(()),  // Skip this entry
                    PutFilterDecision::Remove => {
                        self.remove(core, idx)?;
                        return Ok(());
                    }
                    PutFilterDecision::Replace => {
                        // Fall through to normal update logic below
                    }
                }
            }
        }
        
        // Dispatch based on put_type
        match put_type {
            BtreePutType::Insert => {
                if found { 
                    return Err(BtreeError::KeyAlreadyExists);
                }
                self.insert(core, idx, key, value)?;
            }
            BtreePutType::Update => {
                if !found { 
                    return Err(BtreeError::KeyNotFound);
                }
                self.update(core, idx, value)?;
            }
            BtreePutType::Upsert => {
                if found {
                    self.update(core, idx, value)?;
                } else {
                    self.insert(core, idx, key, value)?;
                }
            }
        }
        Ok(())
    }

    /// Multi-put for range operations (default implementation)
    /// 
    /// Matches C++ variant_node.hpp::multi_put() (lines 258-291)
    /// Override for variants with special needs (e.g., PrefixNode for interval keys)
    /// 
    /// Returns:
    /// - Ok(()) if all entries updated successfully
    /// - Err(HasMore) if ran out of space (caller should retry)
    /// - Err(KeyNotFound) if no keys in range were found
    /// - Err(Io) for I/O errors
    fn multi_put(&self, core: &NodeCore, range: &super::detail::btree_req::BtreeKeyRange<K>,
                 value: &V, put_type: super::detail::btree_req::BtreePutType,
                 filter_fn: Option<&PutFilterFn<K, V>>,
                 last_failed_key: Option<&mut K>) -> Result<(), super::btree::BtreeError> {
        use super::detail::btree_req::{BtreePutType, PutFilterDecision};
        use super::btree::BtreeError;
        
        // Only UPDATE supported for base implementation
        if put_type != BtreePutType::Update {
            debug_assert!(false, "multi_put base implementation only supports UPDATE");
            return Err(BtreeError::Io(std::io::Error::new(
                std::io::ErrorKind::Unsupported,
                "multi_put base implementation only supports UPDATE"
            )));
        }
        debug_assert!(core.is_leaf(), "Multi put only for leaf nodes");
        
        let (matched, start_idx, end_idx) = self.match_range(core, range);
        if !matched {
            return Err(BtreeError::KeyNotFound);
        }
        
        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap_or(0);
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap_or(0);
        
        // Update all in range (C++ lines 273-289)
        let mut idx = start_idx;
        while idx <= end_idx {
            // Check room
            if !self.has_room_for_put(core, put_type, key_size, val_size) {
                if let Some(failed_key) = last_failed_key {
                    *failed_key = self.get_nth_key(core, idx, /*copy=*/true);
                }
                return Err(BtreeError::HasMore);
            }
            
            // Apply filter if provided
            if let Some(filter) = filter_fn {
                let key = self.get_nth_key(core, idx, /*copy=*/false);
                let existing_val = self.get_nth_value(core, idx, /*copy=*/false);
                match filter(&key, &existing_val, value) {
                    PutFilterDecision::Keep => {
                        idx += 1;
                        continue;
                    }
                    PutFilterDecision::Remove => {
                        self.remove(core, idx)?;
                        // Don't increment idx - next entry shifts down
                        continue;
                    }
                    PutFilterDecision::Replace => {
                        // Fall through to update
                    }
                }
            }
            
            // Update entry
            self.update(core, idx, value)?;
            idx += 1;
        }
        
        Ok(())
    }
  
    /// Multi-get: Extract multiple key-value pairs from leaf node in range (default implementation)
    /// 
    /// Efficiently collects up to `max_count` key-value pairs from this leaf node
    /// that match the given range. Returns pagination status to guide caller's next action.
    /// 
    /// Matches C++ variant_node::multi_get()
    /// Override only if variant needs special range extraction logic
    /// 
    /// # Arguments
    /// * `core` - Node core
    /// * `range` - Key range to match
    /// * `max_count` - Maximum number of entries to extract
    /// * `out_values` - Vector to append results to
    /// * `filter` - Optional filter callback
    /// 
    /// # Returns
    /// * `(count, pagination_status)` where:
    ///   - `count`: Number of items added to out_values
    ///   - `pagination_status`: Indicates if more results exist
    ///     - `Completed`: No more results in range (finished)
    ///     - `Continue`: Stopped due to max_count, more results in this node
    ///     - `Unknown`: Reached end of node, check siblings for more
    fn multi_get(&self, core: &NodeCore, range: &super::detail::btree_req::BtreeKeyRange<K>,
                 max_count: u32, out_values: &mut Vec<(K, V)>, 
                 filter: Option<&GetFilterFn<K, V>>) -> (u32, PaginationStatus) {
        debug_assert!(core.is_leaf(), "multi_get only for leaf nodes");
        let (matched, start_idx, end_idx) = self.match_range(core, range);
        if !matched {
            return (0, PaginationStatus::Completed);
        }
        
        let nentries = core.get_persistent_header().nentries();
        let last_idx_in_node = if nentries > 0 { nentries - 1 } else { 0 };
        
        let mut count = 0u32;
        let mut iter_idx = start_idx;
        
        for idx in start_idx..=end_idx {
            if count >= max_count {
                break;
            }
            
            let key = self.get_nth_key(core, idx, /*copy=*/true);
            let value = self.get_nth_value(core, idx, /*copy=*/true);
            
            // Apply filter if provided
            if let Some(filter_fn) = filter {
                if !filter_fn(&key, &value) {
                    iter_idx = idx + 1;
                    continue;
                }
            }
            
            out_values.push((key, value));
            count += 1;
            iter_idx = idx + 1;
        }
        
        // Determine pagination status
        let status = if end_idx != last_idx_in_node {
            // We didn't reach the end of the node
            if iter_idx > end_idx {
                PaginationStatus::Completed  // Finished iterating the range
            } else {
                PaginationStatus::Continue   // Stopped early due to max_count
            }
        } else {
            // We reached the end of the node
            if count > 0 {
                let last_key = &out_values.last().unwrap().0;
                if last_key >= &range.end_key {
                    PaginationStatus::Completed  // Last key matches/exceeds range end
                } else {
                    PaginationStatus::Unknown    // May have more in siblings
                }
            } else {
                PaginationStatus::Unknown
            }
        };
        
        (count, status)
    }

    /// Multi-remove: Remove multiple entries from leaf node in range (default implementation)
    /// 
    /// Efficiently removes entries from this leaf node that match the given range.
    /// Matches C++ variant_node::multi_remove()
    /// 
    /// # Arguments
    /// * `core` - Node core
    /// * `range` - Key range to match and remove
    /// * `max_count` - Maximum number of entries to remove (for batch limit)
    /// * `filter_fn` - Optional filter function to select which entries to remove
    /// 
    /// # Returns
    /// * `Ok(count)` - Number of entries removed
    /// * `Err(BtreeError)` - If error occurred
    fn multi_remove(&self, core: &NodeCore, range: &super::detail::btree_req::BtreeKeyRange<K>, max_count: u32,
                    filter_fn: Option<&RemoveFilterFn<K, V>>) 
                -> Result<u32, super::btree::BtreeError> {
        debug_assert!(core.is_leaf(), "Multi remove only for leaf nodes");

        let (matched, start_idx, end_idx) = self.match_range(core, range);
        if !matched {
            return Ok(0);  // No matches, return 0
        }

        let mut removed = 0u32;
        let mut idx = start_idx;
        
        // Iterate through range entries
        while idx <= end_idx && removed < max_count {
            // Apply filter if provided
            if let Some(filter) = filter_fn {
                let key = self.get_nth_key(core, idx, /*copy=*/false);
                let value = self.get_nth_value(core, idx, /*copy=*/false);
                if !filter(&key, &value) {
                    idx += 1;  // Skip this entry
                    continue;
                }
            }
            
            // Remove entry
            self.remove(core, idx)?;
            removed += 1;
            // Don't increment idx - entries shift down after removal
        }

        Ok(removed)
    }
}

// Import node variant implementations

/// B-tree node magic number
pub const BTREE_NODE_MAGIC: u8 = 0xab;

/// B-tree node version
pub const BTREE_NODE_VERSION: u8 = 1;

/// Persistent header for COW B-tree node (56 bytes, packed)
#[derive(Debug, Clone, Copy)]
#[repr(C, packed)]
pub struct PersistentHeader {
    pub magic: u8,           // offset=0: magic number for validation (0xab)
    pub version: u8,         // offset=1: version number (1)
    pub checksum: u16,       // offset=2: CRC16 checksum of node data (excluding header)
    flags: u32,              // offset=4: nentries:30 + leaf:1 + node_deleted:1
    pub node_id: BNodeId,    // offset=8: node ID (includes ordinal in upper bits)
    pub next_node: BNodeId,  // offset=16: next sibling node ID (for leaf linked list)
    pub node_gen: u64,       // offset=24: generation counter (incremented on every update)
    pub modified_cp_id: i64, // offset=32: checkpoint ID of last modification
    pub edge_id: BNodeId,    // offset=40: edge node ID (rightmost child for interior nodes)
    pub level: u16,          // offset=48: level in tree (0 = leaf)
    pub node_size: u16,      // offset=50: size of node buffer in bytes
    pub node_variant: u8,    // offset=52: variant of node (simple vs varlen etc)
    pub reserved: [u8; 3],   // offset=53-55: reserved for future use
}

impl PersistentHeader {
    const NENTRIES_MASK: u32 = 0x3FFFFFFF; // 30 bits
    const LEAF_BIT: u32 = 1 << 30;
    const NODE_DELETED_BIT: u32 = 1 << 31;

    pub fn new(node_id: BNodeId, is_leaf: bool, node_size: u16) -> Self {
        let flags = if is_leaf { Self::LEAF_BIT } else { 0 };
        Self {
            magic: BTREE_NODE_MAGIC,
            version: BTREE_NODE_VERSION,
            checksum: 0,
            flags,
            node_id,
            next_node: EMPTY_BNODEID,
            node_gen: 0,
            modified_cp_id: -1,
            edge_id: EMPTY_BNODEID,
            level: if is_leaf { 0 } else { 1 },
            node_size,
            node_variant: 0,
            reserved: [0; 3],
        }
    }

    /// Get number of entries
    pub fn nentries(&self) -> u32 {
        self.flags & Self::NENTRIES_MASK
    }

    /// Set number of entries
    pub fn set_nentries(&mut self, n: u32) {
        debug_assert!(n <= Self::NENTRIES_MASK, "nentries {} exceeds 30-bit limit", n);
        self.flags = (self.flags & !Self::NENTRIES_MASK) | (n & Self::NENTRIES_MASK);
    }

    /// Is this a leaf node?
    pub fn is_leaf(&self) -> bool {
        (self.flags & Self::LEAF_BIT) != 0
    }

    /// Set leaf flag
    pub fn set_leaf(&mut self, is_leaf: bool) {
        if is_leaf {
            self.flags |= Self::LEAF_BIT;
        } else {
            self.flags &= !Self::LEAF_BIT;
        }
    }

    /// Is this node deleted?
    pub fn is_node_deleted(&self) -> bool {
        (self.flags & Self::NODE_DELETED_BIT) != 0
    }

    /// Set node deleted flag
    pub fn set_node_deleted(&mut self, deleted: bool) {
        if deleted {
            self.flags |= Self::NODE_DELETED_BIT;
        } else {
            self.flags &= !Self::NODE_DELETED_BIT;
        }
    }

    /// Validate magic and version
    pub fn validate(&self) -> bool {
        self.magic == BTREE_NODE_MAGIC && self.version == BTREE_NODE_VERSION
    }

    /// Get size of persistent header
    pub const fn size() -> usize {
        std::mem::size_of::<Self>()
    }
}

impl Default for PersistentHeader {
    fn default() -> Self {
        Self::new(EMPTY_BNODEID, false, 0)
    }
}

// Compile-time assertion that PersistentHeader is exactly 56 bytes
const _: () = assert!(std::mem::size_of::<PersistentHeader>() == 56);

//================================================================================
// NodeCore - Physical node with lock and buffer (private)
//================================================================================

/// Core node data (private) - holds lock and buffer
/// This is NOT exposed publicly - only Node guard is public
pub struct NodeCore {
    pub lock: AsyncRwLock<NodeInner>,
    pub(super) phys_buf: Arc<iomgr::IOBuffer>, // Physical buffer with PersistentHeader at offset 0
}

impl NodeCore {
    /// Create a new node with given parameters
    pub fn new(node_id: BNodeId, is_leaf: bool, node_size: u32) -> Self {
        let mut buffer = vec![0u8; node_size as usize];

        // Initialize PersistentHeader at the beginning of buffer
        unsafe {
            let header_ptr = buffer.as_mut_ptr() as *mut PersistentHeader;
            *header_ptr = PersistentHeader::new(node_id, is_leaf, node_size as u16);
        }

        Self {
            lock: AsyncRwLock::new(NodeInner {}),
            phys_buf: Arc::new(iomgr::IOBuffer::from_vec(buffer)),
        }
    }

    /// Create node from existing buffer (loaded from storage)
    pub fn from_buffer(buffer: Vec<u8>) -> Self {
        Self {
            lock: AsyncRwLock::new(NodeInner {}),
            phys_buf: Arc::new(iomgr::IOBuffer::from_vec(buffer)),
        }
    }

    #[inline]
    pub(super) fn get_persistent_header(&self) -> &PersistentHeader {
        unsafe { &*(self.phys_buf.as_ptr() as *const PersistentHeader) }
    }

    #[inline]
    pub(super) fn get_persistent_header_mut(&self) -> &mut PersistentHeader {
        unsafe { &mut *(self.phys_buf.as_ptr() as *mut PersistentHeader) }
    }

    #[inline]
    pub(super) fn has_valid_edge(&self) -> bool {
        self.get_persistent_header().edge_id != EMPTY_BNODEID
    }

    #[inline]
    pub(super) fn set_edge(&self, id: BNodeId) {
        self.get_persistent_header_mut().edge_id = id;
    }

    /// Update edge with value (for interior nodes, V = BNodeId)
    pub(super) fn update_edge<V: BtreeValue>(&self, val: &V) {
        debug_assert!(!self.is_leaf(), "Edge update on leaf node");
        let edge_id = unsafe { *(val as *const V as *const u64) };
        self.set_edge(edge_id);
        self.inc_gen();
    }

    #[inline]
    pub(super) fn invalidate_edge(&self) {
        self.get_persistent_header_mut().edge_id = EMPTY_BNODEID;
    }

    #[inline]
    pub fn node_id(&self) -> BNodeId {
        self.get_persistent_header().node_id
    }

    #[inline]
    pub fn is_leaf(&self) -> bool {
        self.get_persistent_header().is_leaf()
    }

    #[inline]
    pub fn node_size(&self) -> u32 {
        self.get_persistent_header().node_size as u32
    }

    #[inline]
    pub fn get_phys_buf(&self) -> &Arc<iomgr::IOBuffer> {
        &self.phys_buf
    }

    #[inline]
    pub fn get_modified_cp_id(&self) -> i64 {
        self.get_persistent_header().modified_cp_id
    }

    pub fn set_modified_cp_id(&self, cp_id: i64) {
        self.get_persistent_header_mut().modified_cp_id = cp_id;
    }

    #[inline]
    pub fn nentries(&self) -> u32 {
        self.get_persistent_header().nentries()
    }

    pub fn set_nentries(&self, n: u32) {
        self.get_persistent_header_mut().set_nentries(n);
    }

    #[inline]
    pub fn node_gen(&self) -> u64 {
        self.get_persistent_header().node_gen
    }

    pub fn set_node_gen(&self, gen: u64) {
        self.get_persistent_header_mut().node_gen = gen;
    }

    #[inline]
    pub fn level(&self) -> u16 {
        self.get_persistent_header().level
    }

    pub fn set_level(&self, lvl: u16) {
        self.get_persistent_header_mut().level = lvl;
    }

    pub fn inc_gen(&self) -> u64 {
        let header = self.get_persistent_header_mut();
        let current = header.node_gen;
        header.node_gen = current + 1;
        current
    }

    /// Check if node is deleted (used in lock upgrade validation)
    #[inline]
    pub fn is_node_deleted(&self) -> bool {
        self.get_persistent_header().is_node_deleted()
    }

    /// Lock node based on lock type and whether node is leaf
    /// Matches C++ read_and_lock_node(int_lock_type, leaf_lock_type)
    /// Called by Btree layer to lock nodes retrieved from storage
    pub async fn lock(self: &Arc<Self>, lock_type: LockType) -> Node {
        let is_leaf = self.is_leaf();

        // Determine actual lock to acquire based on node type
        let actual_lock = match lock_type {
            LockType::None => panic!("Cannot lock with LockType::None"),
            LockType::Read => LockType::Read,
            LockType::Write => LockType::Write,
            LockType::ReadInteriorWriteLeaf => {
                if is_leaf {
                    LockType::Write
                } else {
                    LockType::Read
                }
            }
        };

        let guard = match actual_lock {
            LockType::Read => {
                let g = self.lock.read_lock().await;
                // SAFETY: Safe because Arc<NodeCore> in Node guard keeps lock alive
                InternalLockGuard::Read(unsafe { std::mem::transmute(g) })
            }
            LockType::Write => {
                let g = self.lock.write_lock().await;
                // SAFETY: Safe because Arc<NodeCore> in Node guard keeps lock alive
                InternalLockGuard::Write(unsafe { std::mem::transmute(g) })
            }
            _ => unreachable!(),
        };

        Node {
            core: Arc::clone(self),
            lock_type: actual_lock,
            _guard: guard,
        }
    }
}

impl std::fmt::Debug for NodeCore {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NodeCore")
            .field("node_id", &self.node_id())
            .field("is_leaf", &self.is_leaf())
            .field("node_size", &self.node_size())
            .field("modified_cp_id", &self.get_modified_cp_id())
            .field("nentries", &self.nentries())
            .field("node_gen", &self.node_gen())
            .field("level", &self.level())
            .finish()
    }
}

//================================================================================
// Node - Public guard interface (used by Btree layer)
//================================================================================

/// Node guard - used by Btree layer (matches C++ BtreeNode interface)
/// This is the ONLY public node type - NodeCore is private
pub struct Node {
    pub core: Arc<NodeCore>,
    pub lock_type: LockType,
    pub _guard: InternalLockGuard,
    // NO variant field - dispatch via node_type in header (zero cost)
}

/// Pointer type for cache storage (unlocked nodes)
pub type NodePtr = Arc<NodeCore>;

impl Node {
    // Delegate to core for read operations (common to all variants)

    ///////// Simple Getters (require read lock) ////////////////
    #[inline]
    pub fn node_id(&self) -> BNodeId {
        self.core.node_id()
    }

    #[inline]
    pub fn is_leaf(&self) -> bool {
        self.core.is_leaf()
    }

    #[inline]
    pub fn nentries(&self) -> u32 {
        self.core.get_persistent_header().nentries()
    }

    /// Total entries including edge
    #[inline]
    pub fn total_entries(&self) -> u32 {
        self.nentries()
    }

    /// Check if node has a valid edge value
    #[inline]
    pub fn has_valid_edge(&self) -> bool {
        self.get_edge_value() != EMPTY_BNODEID
    }

    #[inline]
    pub fn node_gen(&self) -> u64 {
        self.core.node_gen()
    }

    #[inline]
    pub fn level(&self) -> u16 {
        self.core.get_persistent_header().level
    }

    #[inline]
    pub fn node_variant(&self) -> u8 {
        self.core.get_persistent_header().node_variant
    }

    #[inline]
    pub fn node_size(&self) -> u32 {
        self.core.node_size()
    }

    #[inline]
    pub fn get_modified_cp_id(&self) -> i64 {
        self.core.get_modified_cp_id()
    }

    /// Get edge value (rightmost child ID for interior nodes)
    #[inline]
    pub fn get_edge_value(&self) -> BNodeId {
        self.core.get_persistent_header().edge_id
    }

    #[inline]
    pub fn get_phys_buf(&self) -> &Arc<iomgr::IOBuffer> {
        self.core.get_phys_buf()
    }

    /// Get current lock type
    #[inline]
    pub fn lock_type(&self) -> LockType {
        self.lock_type
    }

    /// Get underlying core for upgrade operations
    #[inline]
    pub fn core(&self) -> &Arc<NodeCore> {
        &self.core
    }

    /// Get persistent header for debugging/inspection
    #[inline]
    pub fn get_persistent_header(&self) -> &PersistentHeader {
        self.core.get_persistent_header()
    }

    /// Get next sibling node ID (for leaf linked list)
    #[inline]
    pub fn get_next_node(&self) -> BNodeId {
        self.core.get_persistent_header().next_node
    }

    /// Get the size of node data (excluding persistent header)
    #[inline]
    pub fn node_data_size(&self) -> u32 {
        self.node_size() - std::mem::size_of::<PersistentHeader>() as u32
    }

    /// Check if node is deleted
    #[inline]
    pub fn is_node_deleted(&self) -> bool {
        self.core.get_persistent_header().is_node_deleted()
    }
    
    ///////// Simple Mutators (require write lock) - common to all variants ////////////////
    pub fn set_nentries(&self, n: u32) {
        debug_assert!(self.lock_type == LockType::Write, "Cannot mutate with {:?} lock", self.lock_type);
        self.core.get_persistent_header_mut().set_nentries(n);
    }

    pub fn set_modified_cp_id(&self, cp_id: i64) {
        debug_assert!(self.lock_type == LockType::Write, "Cannot mutate with {:?} lock", self.lock_type);
        self.core.set_modified_cp_id(cp_id);
    }

    pub fn set_level(&self, lvl: u16) {
        debug_assert!(self.lock_type == LockType::Write, "Cannot mutate with {:?} lock", self.lock_type);
        self.core.set_level(lvl);
    }

    pub fn inc_gen(&self) -> u64 {
        debug_assert!(self.lock_type == LockType::Write, "Cannot mutate with {:?} lock", self.lock_type);
        self.core.inc_gen()
    }

    /// Set edge value (rightmost child ID for interior nodes)
    pub fn set_edge_value(&self, id: BNodeId) {
        debug_assert!(self.lock_type == LockType::Write, "Cannot mutate with {:?} lock", self.lock_type);
        self.core.get_persistent_header_mut().edge_id = id;
    }

    /// Set next sibling node ID (for leaf linked list)
    pub fn set_next_node(&self, id: BNodeId) {
        debug_assert!(self.lock_type == LockType::Write, "Cannot mutate with {:?} lock", self.lock_type);
        self.core.get_persistent_header_mut().next_node = id;
    }

    ///////// Node cloning and buffer operations ////////////////
    
    /// Clone this node into a temporary copy (not in storage cache)
    /// 
    /// Creates an exact copy of this node's buffer in temporary memory.
    /// The cloned node is independent and not tracked by storage.
    /// 
    /// 
    /// # Arguments
    /// * `lock_type` - Lock type to acquire on the cloned node
    /// 
    /// # Returns
    /// * Locked temporary Node guard
    pub async fn clone_temp(&self, lock_type: LockType) -> Node {
        // Clone the physical buffer
        let buffer = self.core.get_phys_buf().to_vec();
        let temp_core = Arc::new(NodeCore::from_buffer(buffer));
        temp_core.lock(lock_type).await
    }
    
    /// Overwrite this node's buffer with another node's buffer
    /// 
    /// Performs a complete buffer copy from `other` to `self`.
    /// Used in merge operations to commit temporary changes to actual nodes.
    /// 
    /// Matches C++ `BtreeNode::overwrite()` in btree_node.hpp line 322
    /// 
    /// # Arguments
    /// * `other` - Source node to copy from
    /// 
    /// # Panics
    /// * If node sizes don't match
    pub fn overwrite(&self, other: &Node) {
        debug_assert!(self.lock_type == LockType::Write, "Cannot overwrite with {:?} lock", self.lock_type);
        
        let self_size = self.node_size();
        let other_size = other.node_size();
        
        assert_eq!(self_size, other_size, "Cannot overwrite: node sizes don't match (self={}, other={})",
            self_size, other_size);
        
        // Copy entire physical buffer (C++ line 324: memcpy)
        unsafe {
            let src_ptr = other.core.get_phys_buf().as_ptr();
            let dst_ptr = self.core.get_phys_buf().as_ptr() as *mut u8;
            std::ptr::copy_nonoverlapping(src_ptr, dst_ptr, self_size as usize);
        }
    }

    ///////// Node methods using dispatch (impls high level abstraction on varient-specific NodeOps) ////////////////
    
    //================================================================================
    // Sub section - Methods that can be called on both leaf and interior nodes 
    //================================================================================
  
    /// Find key position in node (binary search)
    ///
    /// Returns (found, index) where:
    /// - found: true if exact match found
    /// - index: insertion position (for leaf nodes) or child index (for interior nodes)
    pub fn find<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, key: &K) -> (bool, u32) {
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/false).find(&self.core, key)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/false).find(&self.core, key)
        }
    }

    /// Find if a given key range is found in the node and return the index range.
    ///
    /// Returns (matched, start_idx, end_idx) where:
    /// - matched: true if the range is found, false otherwise
    /// - start_idx: start index of the range (only valid if matched = true)
    /// - end_idx: end index of the range (only valid if matched = true)
    ///
    /// Note: When matched = false, start_idx and end_idx are undefined and should not be used.
    /// The caller should check the matched flag before using the indices.
    pub fn match_range<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self, 
        range: &super::detail::btree_req::BtreeKeyRange<K>
    ) -> (bool, u32, u32) {
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/false).match_range(&self.core, range)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/false).match_range(&self.core, range)
        }
    }

    /// Get the nth key (auto-dispatches value type based on leaf/interior)
    pub fn get_nth_key<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, idx: u32, copy: bool) -> K {
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/false).get_nth_key(&self.core, idx, copy)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/false).get_nth_key(&self.core, idx, copy)
        }
    }


    /// Get available space in node
    pub fn available_size<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> u32 {
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/false).available_size(&self.core)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/false).available_size(&self.core)
        }
    }

    /// Check if node has room for put operation
    pub fn has_room_for_put<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self, 
        put_type: super::detail::btree_req::BtreePutType, 
        key_size: u32, 
        val_size: u32
    ) -> bool {
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/false).has_room_for_put(&self.core, put_type, key_size, val_size)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/false).has_room_for_put(&self.core, put_type, key_size, val_size)
        }
    }

    /// Move entries from this node to right sibling by size. Returns the number of bytes moved
    pub fn move_out_to_right_by_size<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self, 
        dst_node: &Node, 
        size: u32
    ) -> u32 {
        debug_assert!(self.lock_type == LockType::Write, "Source node must have write lock");
        debug_assert!(dst_node.lock_type == LockType::Write, "Destination node must have write lock");
        
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/true).move_out_to_right_by_size(&self.core, &dst_node.core, size)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true).move_out_to_right_by_size(&self.core, &dst_node.core, size)
        }
    }

    /// Get occupied size in bytes (node_data_size - available_size)
    /// Matches C++ btree_node.hpp line 652
    pub fn occupied_size<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> u32 {
        self.node_data_size() - self.available_size::<K, V>()
    }

    /// Remove all entries from the node
    /// Matches C++ variant_node.hpp::remove_all()
    pub fn remove_all<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) {
        debug_assert!(self.lock_type == LockType::Write, "remove_all requires write lock");      
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/true).remove_all(&self.core);
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true).remove_all(&self.core);
        }
    }

    /// Remove a range of entries by index [start_idx, end_idx] inclusive
    pub fn remove_range<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self, 
        start_idx: u32, 
        end_idx: u32
    ) -> Result<(), super::btree::BtreeError> {
        debug_assert!(self.lock_type == LockType::Write, "remove_range requires write lock");
        if self.is_leaf() {  
            self.get_node_ops::<K, V>(/*wlock_reqd=*/true).remove_range(&self.core, start_idx, end_idx)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true).remove_range(&self.core, start_idx, end_idx)
        }
    }

    /// Move entries from this node to right sibling by entry count
    /// Matches C++ simple_node.hpp::move_out_to_right_by_entries()
    /// Returns the number of entries moved
    pub fn move_out_to_right_by_entries<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self, 
        dst_node: &Node, 
        nentries: u32
    ) -> u32 {
        debug_assert!(self.lock_type == LockType::Write, "Source node must have write lock");
        debug_assert!(dst_node.lock_type == LockType::Write, "Destination node must have write lock");
        
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/true)
                .move_out_to_right_by_entries(&self.core, &dst_node.core, nentries)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true)
                .move_out_to_right_by_entries(&self.core, &dst_node.core, nentries)
        }
    }

    /// Copy entries from src to this node up to a size limit
    /// Matches C++ simple_node.hpp::append_copy_in_upto_size()
    /// 
    /// # Arguments
    /// * `src_node` - Source node to copy from
    /// * `other_cursor` - Index in src to start copying from (updated after copy)
    /// * `upto_size` - Maximum bytes to fill in destination
    /// * `copy_only_if_fits` - If true, only copy if all entries fit; if false, copy what fits
    /// 
    /// # Returns
    /// * `true` if copy succeeded, `false` if no room or doesn't fit
    pub fn append_copy_in_upto_size<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, src_node: &Node, 
                                 other_cursor: &mut u32, upto_size: u32, copy_only_if_fits: bool) -> bool {
        debug_assert!(self.lock_type == LockType::Write, "Destination node must have write lock");
        
        if self.is_leaf() {
            self.get_node_ops::<K, V>(/*wlock_reqd=*/true)
                .append_copy_in_upto_size(&self.core, &src_node.core, other_cursor, upto_size, copy_only_if_fits)
        } else {
            self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true)
                .append_copy_in_upto_size(&self.core, &src_node.core, other_cursor, upto_size, copy_only_if_fits)
        }
    }
    
    /// Get the first key in the node
    pub fn get_first_key<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> K {
        self.get_nth_key::<K, V>(0, /*copy=*/true)
    }

    /// Get the last key in the node
    /// Get the last key in the node (convenience method)
    /// Returns None if node is empty
    pub fn get_last_key<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> Option<K> {
        let nentries = self.total_entries();
        if nentries == 0 {
            return None;
        }
        Some(self.get_nth_key::<K, V>(nentries - 1, /*copy=*/true))
    }

    /// Get the nth value (works for both leaf nodes with V and interior nodes with BNodeId as V)
    pub fn get_nth_value<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, idx: u32, copy: bool) -> V {
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/false);
        ops.get_nth_value(&self.core, idx, copy)
    }

    /// Get the first value in the node
    pub fn get_first_value<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> V {
        self.get_nth_value::<K, V>(0, /*copy=*/true)
    }

    /// Get the last value in the node
    pub fn get_last_value<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> V {
        let nentries = self.total_entries();
        debug_assert!(nentries > 0, "Cannot get last value from empty node");
        self.get_nth_value::<K, V>(nentries - 1, /*copy=*/true)
    }

    //================================================================================
    // Sub section - Methods that can be called on leaf nodes only 
    //================================================================================
  
    /// Insert a key-value pair at the specified index
    pub fn insert<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, idx: u32, key: &K, val: &V) 
        -> Result<(), super::btree::BtreeError> {
        debug_assert!(self.is_leaf(), "insert() is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/true);
        ops.insert(&self.core, idx, key, val)
    }

    /// Update value at index (for parent updates after split)
    pub fn update<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, idx: u32, val: &V) 
        -> Result<(), super::btree::BtreeError> {
        debug_assert!(self.is_leaf(), "update() is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/true);
        ops.update(&self.core, idx, val)
    }
    
    /// Remove the entry at the specified index
    pub fn remove<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, idx: u32) 
        -> Result<(), super::btree::BtreeError> {
        debug_assert!(self.is_leaf(), "remove() is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/true);
        ops.remove(&self.core, idx)
    }

    /// Put a single key-value pair into this node (leaf nodes only)
    /// 
    /// Returns:
    /// - Ok(()) if the put succeeded
    /// - Err(KeyAlreadyExists) if INSERT found duplicate key
    /// - Err(KeyNotFound) if UPDATE didn't find key
    /// - Err(Io) if there was an I/O error
    pub fn put<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self,
        key: &K,
        value: &V,
        put_type: super::detail::btree_req::BtreePutType,
        filter_fn: Option<&PutFilterFn<K, V>>
    ) -> Result<(), super::btree::BtreeError> {
        debug_assert!(self.is_leaf(), "put() is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/true);
        ops.put(&self.core, key, value, put_type, filter_fn)
    }

    /// Multi-put for range operations (leaf nodes only)
    /// 
    /// Returns:
    /// - Ok(()) if all entries updated successfully
    /// - Err(HasMore) if ran out of space (caller should retry)
    /// - Err(KeyNotFound) if no keys in range were found
    /// - Err(Io) for I/O errors
    pub fn multi_put<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self,
        range: &super::detail::btree_req::BtreeKeyRange<K>,
        value: &V,
        put_type: super::detail::btree_req::BtreePutType,
        filter_fn: Option<&PutFilterFn<K, V>>,
        last_failed_key: Option<&mut K>,
    ) -> Result<(), super::btree::BtreeError> {
        debug_assert!(self.is_leaf(), "multi_put is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/true);
        ops.multi_put(&self.core, range, value, put_type, filter_fn, last_failed_key)
    }

    /// Multi-remove for range operations (leaf nodes only)
    /// 
    /// Removes up to max_count entries matching the given range.
    /// 
    /// # Returns
    /// * `Ok(count)` - Number of entries removed
    /// * `Err(BtreeError)` - If error occurred
    pub fn multi_remove<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self,
        range: &super::detail::btree_req::BtreeKeyRange<K>,
        max_count: u32,
        filter_fn: Option<&RemoveFilterFn<K, V>>,
    ) -> Result<u32, super::btree::BtreeError> {
        debug_assert!(self.is_leaf(), "multi_remove is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/true);
        ops.multi_remove(&self.core, range, max_count, filter_fn)
    }

    /// Multi-get: Extract multiple key-value pairs from leaf node in range
    /// 
    /// Efficiently collects up to `max_count` key-value pairs from this leaf node
    /// that match the given range. Corresponds to C++ multi_get().
    /// 
    /// # Arguments
    /// * `range` - Key range to match
    /// * `max_count` - Maximum number of entries to extract
    /// * `out_values` - Vector to append results to
    /// * `filter` - Optional filter callback
    /// 
    /// # Returns
    /// * `(count, pagination_status)` where:
    ///   - `count`: Number of items added
    ///   - `pagination_status`: Completed/Continue/Unknown (see PaginationStatus)
    pub fn multi_get<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self, range: &super::detail::btree_req::BtreeKeyRange<K>, max_count: u32,
        out_values: &mut Vec<(K, V)>, filter: Option<&GetFilterFn<K, V>>) -> (u32, PaginationStatus) {
        debug_assert!(self.is_leaf(), "multi_get is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/false);
        ops.multi_get(&self.core, range, max_count, out_values, filter)
    }

    /// Get all key-value pairs from the node (leaf nodes only)
    pub fn get_all_kvs<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> Vec<(K, V)> {
        debug_assert!(self.is_leaf(), "get_all_kvs is only supported for leaf nodes");
        let ops = self.get_node_ops::<K, V>(/*wlock_reqd=*/false);
        ops.get_all_kvs(&self.core)
    }

    /// Dump node contents as string for debugging
    pub fn dump<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self) -> String {
        let mut s = format!("Node[id={} leaf={} lvl={} entries={}",
                            self.node_id(), self.is_leaf(), self.level(), self.total_entries());
        if !self.is_leaf() {
            let edge = self.get_edge_value();
            s.push_str(&format!(" edge={}", if edge == EMPTY_BNODEID { "EMPTY".to_string() } 
                                            else { edge.to_string() }));
        }
        s.push_str("] Keys: ");
        for i in 0..self.total_entries() {
            if i > 0 { s.push_str(", "); }
            let key: K = self.get_nth_key::<K, V>(i, /*copy=*/true);
            let val: V = self.get_nth_value::<K, V>(i, /*copy=*/true);
            s.push_str(&format!("{:?}→{:?}", key, val));
        }
        s
    }

    //================================================================================
    // Sub section - Methods that can be called on interior nodes only 
    //================================================================================

    /// Insert key-child pair into interior node
    pub fn insert_child<K: BtreeKey + 'static>(&self, idx: u32, key: &K, child_id: &BNodeId) 
        -> Result<(), super::btree::BtreeError> {
        debug_assert!(!self.is_leaf(), "insert_child called on leaf node");
        let ops = self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true);
        ops.insert(&self.core, idx, key, child_id)
    }

    /// Update child ID at index in interior node
    pub fn update_child<K: BtreeKey + 'static>(&self, idx: u32, child_id: &BNodeId) 
        -> Result<(), super::btree::BtreeError> {
        debug_assert!(!self.is_leaf(), "update_child called on leaf node");
        let ops = self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true);
        ops.update(&self.core, idx, child_id)
    }

    /// Update both key and child ID at index in interior node atomically (C++ update())
    pub fn update_child_with_key<K: BtreeKey + 'static>(&self, idx: u32, key: &K, child_id: &BNodeId) 
        -> Result<(), super::btree::BtreeError> {
        debug_assert!(!self.is_leaf(), "update_child_with_key called on leaf node");
        let ops = self.get_node_ops::<K, BNodeId>(/*wlock_reqd=*/true);
        ops.update_with_key(&self.core, idx, key, child_id)
    }

    /// Get child ID at index from interior node
    pub fn get_nth_child_id<K: BtreeKey + 'static>(&self, idx: u32) -> BNodeId {
        debug_assert!(!self.is_leaf(), "get_nth_child_id called on leaf node");
        if idx == self.nentries() {
            self.get_edge_value()
        } else {
            self.get_nth_value::<K, BNodeId>(idx, /*copy=*/true)
        }
    }

    pub fn get_first_child_id<K: BtreeKey + 'static>(&self) -> BNodeId {
        debug_assert!(!self.is_leaf(), "get_first_child_id called on leaf node");
        self.get_nth_child_id::<K>(0)
    }

    pub fn get_last_child_id<K: BtreeKey + 'static>(&self) -> BNodeId {
        debug_assert!(!self.is_leaf(), "get_last_child_id called on leaf node");
        self.get_nth_child_id::<K>(self.nentries() - 1)
    }

    /// Get variant-specific operations with lock validation
    /// 
    /// # Arguments
    /// * `wlock_reqd` - If true, validates that node has write lock
    /// 
    /// # Panics (debug builds)
    /// Panics if wlock_reqd=true but node doesn't have write lock
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>(&self, wlock_reqd: bool) 
        -> &'static dyn NodeOps<K, V> {
        // Validate lock requirements
        if wlock_reqd {
            debug_assert!(
                self.lock_type == LockType::Write,
                "Operation requires write lock, but node has {:?} lock (node_id={})",
                self.lock_type,
                self.node_id()
            );
        }
        
        let node_variant = self.core.get_persistent_header().node_variant;
        match node_variant {
            0 => &SIMPLE_NODE_OPS,
            1 => &VAR_KEY_NODE_OPS,
            _ => panic!("Unknown node variant: {}", node_variant),
        }
    }
}

impl std::fmt::Debug for Node {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Node")
            .field("node_id", &self.node_id())
            .field("is_leaf", &self.is_leaf())
            .field("node_size", &self.node_size())
            .field("lock_type", &self.lock_type)
            .field("nentries", &self.nentries())
            .field("node_gen", &self.node_gen())
            .field("level", &self.level())
            .finish()
    }
}

//================================================================================
// Phase 6: Node Operations (Zero-Sized Types for Dispatch)
//================================================================================

// SimpleNodeOps moved to variant/simple_node.rs

/// Static instances of node operations (zero-sized types)
/// These are used to avoid Arc allocation on every operation
pub static SIMPLE_NODE_OPS: variant::SimpleNodeOps = variant::SimpleNodeOps;
pub static VAR_KEY_NODE_OPS: variant::VarKeyNodeOps = variant::VarKeyNodeOps;
