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
 */

//! Common B-tree types
//!
//! This module defines common types used across the B-tree implementation:
//! - BtreeError: Error type for btree operations
//! - BtreeConfig: Configuration for btree instances
//! - SyncOverflowStorage: Trait for synchronous overflow storage (cabindb)

use std::io;
use std::ops::Deref;

//================================================================================
// BtreeBuffer - Abstraction over IOBuffer (persistent) and Vec<u8> (inmem)
//================================================================================

/// BtreeBuffer abstracts the underlying buffer storage.
/// - For persistent mode: Uses IOBuffer from iomanager
/// - For inmem mode: Uses Vec<u8> directly
#[cfg(feature = "persistent")]
pub struct BtreeBuffer(iomgr::IOBuffer);

#[cfg(not(feature = "persistent"))]
pub struct BtreeBuffer(Vec<u8>);

impl BtreeBuffer {
    #[cfg(feature = "persistent")]
    pub fn new(size: usize) -> Self {
        BtreeBuffer(iomgr::IOBuffer::new(size))
    }

    #[cfg(not(feature = "persistent"))]
    pub fn new(size: usize) -> Self {
        BtreeBuffer(vec![0u8; size])
    }

    #[cfg(feature = "persistent")]
    pub fn from_vec(buf: Vec<u8>) -> Self {
        BtreeBuffer(iomgr::IOBuffer::from_vec(buf))
    }

    #[cfg(not(feature = "persistent"))]
    pub fn from_vec(buf: Vec<u8>) -> Self {
        BtreeBuffer(buf)
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.0
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        &mut self.0
    }

    pub fn len(&self) -> usize {
        self.0.len()
    }
}

impl Deref for BtreeBuffer {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl AsRef<[u8]> for BtreeBuffer {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

//================================================================================
// BtreeError - Common error type for btree operations
//================================================================================

/// B-tree operation errors (matches C++ BtreeError enum)
#[derive(Debug)]
pub enum BtreeError {
    Retry,            // Lock upgrade failed due to concurrent modification - retry operation
    CpMismatch,       // Checkpoint context mismatch - retry operation from root
    HasMore,          // Range operation has more entries to process, keep going
    KeyNotFound,      // Key not found in the btree
    KeyAlreadyExists, // Key already exists in the btree
    NodeNotFound,     // Node not found in storage (completely internal error)
    Io(io::Error),    // I/O error from storage layer
}

/// Helper macro to create BtreeError::Io from io::ErrorKind and message

/// Returns BtreeError::Io value (for use with ok_or)
#[macro_export]
macro_rules! btree_err_result {
    ($kind:ident, $msg:expr) => {
        BtreeError::Io(io::Error::new(io::ErrorKind::$kind, $msg))
    };
}

#[macro_export]
macro_rules! btree_io_err {
    ($kind:ident, $msg:expr) => {
        Err($crate::btree_err_result!($kind, $msg))
    };
}

//================================================================================
// Btree Configuration
//================================================================================

/// Policy for deciding whether to accept a partial merge result.
///
/// Merge operations attempt to consolidate multiple nodes into fewer nodes by packing entries up to `ideal_fill_size`.
/// When only some nodes can be merged (e.g., due to size constraints), this policy determines whether to commit or
/// reject the partial merge.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MergePolicy {
    /// Never merge nodes. Nodes will only be removed via other mechanisms.
    ///
    /// Use for: Testing, debugging, or when merges are explicitly unwanted
    Never,

    /// Accept any merge that reduces node count.
    ///
    /// This policy maximizes compaction and is suitable for workloads where read amplification (from fragmented nodes)
    /// is more expensive than write amplification (from merge operations).
    ///
    /// Use for: In-memory store, Cold indexes, large datasets, read-heavy workloads
    Aggressive,

    /// Accept merge only if it provides significant compaction benefit.
    ///
    /// Accepts merge if:
    /// - Saves 2+ nodes (for middle merges with 3+ nodes available), OR
    /// - Saves 1+ node when only 2 nodes attempted (edge merges at tree boundaries)
    ///
    /// This policy avoids expensive merge operations when the benefit is marginal, suitable for workloads where write
    /// amplification matters (e.g., cached indexes, frequent checkpointing).
    ///
    /// Use for: Cached indexes, write-heavy workloads, small nodes
    Conservative,
}

impl Default for MergePolicy {
    fn default() -> Self { MergePolicy::Aggressive }
}

/// Btree configuration (matches C++ BtreeConfig)
#[derive(Debug, Clone)]
pub struct BtreeConfig {
    pub btree_name: String,        // Name of the btree
    pub node_size: u32,            // Size of the node. TODO: In future support diff size of leaf/interior
    pub ideal_fill_pct: u8,        // Ideal fill percentage of the node - used during merge
    pub suggested_min_pct: u8,     // Suggested min percentage of the node - used during split
    pub split_pct: u8,             // % in node to split on (say if 90, it will leave left with 90%, right with 10%)
    pub max_merge_nodes: u32,      // Max number of nodes to merge at once
    pub merge_policy: MergePolicy, // Policy for deciding whether to accept a partial merge result
    pub leaf_node_variant: u8,     // Node variant for leaf nodes
    pub int_node_variant: u8,      // Node variant for interior nodes

    // Overflow node configuration
    pub overflow_threshold: u32,      // Max value size to inline in the node
    pub overflow_min_alloc_size: u32, // Whats the multiple of node_size to allocate for overflow storage

    // Prefix compression config (for PrefixCompressNode variant)
    pub expected_prefix_size: u16, // 0 = dynamic, >0 = fixed split point

    // Precomputed values for faster calculations
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
            merge_policy: MergePolicy::Aggressive,
            leaf_node_variant: 0, // Simple node
            int_node_variant: 0,  // Simple node
            btree_name,
            overflow_threshold: 128,      // 128 bytes is a good default for most use cases
            overflow_min_alloc_size: 512, // 512 minimum for persistence alignment
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

    pub fn split_size(&self, filled_size: u32) -> u32 { (filled_size * self.split_pct as u32) / 100 }

    pub fn ideal_fill_size(&self) -> u32 { self.ideal_fill_size }

    pub fn suggested_min_size(&self) -> u32 { self.suggested_min_size }
}
