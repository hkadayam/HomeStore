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
 ************************************************************************ */

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
    pub fn new(size: usize) -> Self { BtreeBuffer(iomgr::IOBuffer::new(size)) }

    #[cfg(not(feature = "persistent"))]
    pub fn new(size: usize) -> Self { BtreeBuffer(vec![0u8; size]) }

    #[cfg(feature = "persistent")]
    pub fn from_vec(buf: Vec<u8>) -> Self { BtreeBuffer(iomgr::IOBuffer::from_vec(buf)) }

    #[cfg(not(feature = "persistent"))]
    pub fn from_vec(buf: Vec<u8>) -> Self { BtreeBuffer(buf) }

    pub fn as_slice(&self) -> &[u8] { &self.0 }

    pub fn as_mut_slice(&mut self) -> &mut [u8] { &mut self.0 }

    pub fn len(&self) -> usize { self.0.len() }
}

impl Deref for BtreeBuffer {
    type Target = [u8];

    fn deref(&self) -> &Self::Target { &self.0 }
}

impl AsRef<[u8]> for BtreeBuffer {
    fn as_ref(&self) -> &[u8] { &self.0 }
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
    pub overflow_min_alloc_size: u32, // Whats the multiple of node_size to allocate for overflow storage
    pub inline_value_size: u32,       /* Threshold for inlining values (vs overflow). Clamped: sizeof(BNodeId) <= x
                                       * <= node_size/32 */

    // Prefix compression config (for PrefixCompressNode variant)
    pub expected_prefix_size: u16, // 0 = dynamic, >0 = fixed split point

    pub is_single_threaded: bool, // Is the btree single threaded?

    // Precomputed values for faster calculations
    pub suggested_min_size: u32,
    pub ideal_fill_size: u32,
    pub max_key_size: u32, // Maximum key size that guarantees min 2 entries per node
}

impl BtreeConfig {
    pub fn new(node_size: u32, btree_name: String) -> Self {
        let inline_value_size = Self::clamp_inline_value_size(128, node_size);
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
            overflow_min_alloc_size: 512, // 512 minimum for persistence alignment
            inline_value_size,
            suggested_min_size: 0,
            ideal_fill_size: 0,
            max_key_size: 0, // Computed in finalize()
            is_single_threaded: false,
        };
        config.finalize();
        config
    }

    /// Clamp inline_value_size to safe bounds
    fn clamp_inline_value_size(requested: u32, node_size: u32) -> u32 {
        const MIN_INLINE: u32 = std::mem::size_of::<super::btree_node::BNodeId>() as u32; // 8 bytes
        let max_inline = node_size / 32; // Conservative max (e.g., 128 for 4KB nodes)

        requested.clamp(MIN_INLINE, max_inline)
    }

    /// Suggest inline value size with automatic clamping
    /// This is a hint for performance optimization - values can exceed this via overflow
    pub fn suggest_inline_value_size(&mut self, size: u32) {
        self.inline_value_size = Self::clamp_inline_value_size(size, self.node_size);
        self.finalize(); // Recalculate max_key_size
    }

    fn finalize(&mut self) {
        let node_header_size = std::mem::size_of::<super::btree_node::PersistentHeader>() as u32;
        let usable_size = self.node_size - node_header_size;
        self.ideal_fill_size = (usable_size * self.ideal_fill_pct as u32) / 100;
        self.suggested_min_size = (usable_size * self.suggested_min_pct as u32) / 100;

        // Calculate max_key_size based on node variant
        self.max_key_size = self.calculate_max_key_size();
    }

    /// Calculate maximum key size that guarantees at least 2 entries per node
    fn calculate_max_key_size(&self) -> u32 {
        use super::variant::{SimpleNodeOps, VarNodeOps, prefix_compress_node::PrefixCompressNodeOps};

        // Get overhead from the variant
        let (header_size, per_entry_overhead) = match self.leaf_node_variant {
            0 => SimpleNodeOps::get_overhead_size(),
            1 | 2 | 3 => VarNodeOps::<()>::get_overhead_size(),
            4 => PrefixCompressNodeOps::get_overhead_size(),
            _ => (std::mem::size_of::<super::btree_node::PersistentHeader>() as u32, 0),
        };

        let usable_size = self.node_size.saturating_sub(header_size);
        let min_entries = 2; // Minimum for splits to work

        // Space per entry: per_entry_overhead + key_size + value_size
        // Total: min_entries * (per_entry_overhead + key_size + inline_value_size) <= usable_size
        let space_per_entry = usable_size / min_entries;

        space_per_entry.saturating_sub(per_entry_overhead).saturating_sub(self.inline_value_size)
    }

    pub fn split_size(&self, filled_size: u32) -> u32 { (filled_size * self.split_pct as u32) / 100 }

    pub fn ideal_fill_size(&self) -> u32 { self.ideal_fill_size }

    pub fn suggested_min_size(&self) -> u32 { self.suggested_min_size }

    pub fn max_key_size(&self) -> u32 { self.max_key_size }
}
