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

use async_trait::async_trait;

use crate::common::{BlkAllocHints, BlkAllocStatus, BlkCount, BlkId, BlkIds, BlkNum};

// Re-export segment manager types for convenience
pub use super::segment_manager::{EmptyPortion, PortionBase, PortionFactory, Segment, SegmentManager};

/// Configuration for block allocator
#[derive(Debug, Clone)]
pub struct BlkAllocConfig {
    pub blk_size: u32,
    pub align_size: u32,
    pub capacity: BlkNum, // Total number of blocks
    pub persistent: bool,
    pub unique_name: String,
    pub num_segments: u32,
    pub fixed_size_segments: bool, // If true, all segments (except possibly last) have equal size
}

impl BlkAllocConfig {
    pub fn new(blk_size: u32, align_size: u32, size: u64, persistent: bool, name: String, num_segments: u32) -> Self {
        let capacity = (size / blk_size as u64) as BlkNum;

        Self { blk_size, align_size, capacity, persistent, unique_name: name, num_segments: num_segments.max(1), fixed_size_segments: true }
    }

    pub fn to_string(&self) -> String {
        format!(
            "BlkSize={} TotalBlks={} Segments={} persistent={}",
            self.blk_size, self.capacity, self.num_segments, self.persistent
        )
    }
}

/// Base trait for block allocators
#[async_trait]
pub trait BlkAllocator: Send + Sync {
    /// Finalize recovering from persistent storage - called at the end of recovery
    ///
    /// This method:
    /// 1. Finalizes any pending operations (reserves/frees)
    /// 2. Reloads allocator-specific state (e.g., free queues, caches)
    /// 3. Moves to Active state
    ///
    /// Default implementation does nothing for non-persistent allocators
    async fn recover(&mut self) {}

    /// Shutdown the allocator gracefully, waiting for all background tasks to complete.
    ///
    /// **IMPORTANT**: This MUST be called before dropping the allocator to ensure all
    /// background tasks (cache fills, async frees, etc.) complete. Background tasks may
    /// hold raw pointers to the allocator, so dropping without shutdown causes undefined behavior.
    ///
    /// Default implementation does nothing for allocators without background tasks.
    async fn shutdown(&self) {}

    /// Allocate nblks immediately on the current reactor - returns collection of BlkIds
    fn alloc_immediate(&self, nblks: BlkCount, hints: &BlkAllocHints, out_blkids: &mut BlkIds) -> BlkAllocStatus;

    /// Allocate a single contiguous block asynchronously
    /// This picks a random reactor and runs the allocation there, returning a single BlkId
    async fn alloc_contiguous(&self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, Option<BlkId>);

    /// Allocate nblks asynchronously on a random reactor
    /// This picks a random reactor and runs the allocation there, returning the result
    async fn alloc(&self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, BlkIds);

    /// Schedule commit of blocks on disk (for persistence) - fire-and-forget
    fn schedule_commit(&self, bid: &BlkId) -> BlkAllocStatus;

    /// Schedule free of allocated blocks (fire-and-forget, does not wait for completion)
    fn schedule_free(&self, id: &BlkId);

    /// Free a single block asynchronously and wait for completion
    async fn free(&self, bid: &BlkId);

    /// Free multiple blocks asynchronously across appropriate reactors and wait for completion
    async fn free_batch(&self, bids: &BlkIds);

    /// Get number of available blocks
    fn available_blks(&self) -> BlkNum;

    /// Get number of fragmented blocks
    fn get_defrag_nblks(&self) -> BlkNum;

    /// Get number of used blocks
    fn get_used_blks(&self) -> BlkNum;

    /// Check if block is allocated
    /// If is_thread_safe is true, runs on the appropriate reactor
    async fn is_blk_alloced(&self, b: &BlkId, is_thread_safe: bool) -> bool;

    /// Convert to string representation
    fn to_string(&self) -> String;

    /// Get alignment size
    fn get_align_size(&self) -> u32;

    /// Get total blocks
    fn get_total_blks(&self) -> BlkNum;

    /// Get allocator name
    fn get_name(&self) -> &str;

    /// Get block size
    fn get_blk_size(&self) -> u32;

    /// Get status as JSON (we'll use a simple string for now)
    fn get_status(&self, log_level: i32) -> String;
}
