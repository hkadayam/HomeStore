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
use super::blk_allocator::BlkAllocator;

/// Empty Block Allocator - all operations are unimplemented
///
/// This allocator is used for VDevs with `BlkAllocatorType::None`,
/// typically for log streams where block allocation is managed
/// by higher-level components (e.g., SimpleLogStreamVdev).
pub struct EmptyBlkAllocator;

impl EmptyBlkAllocator {
    /// Create a new EmptyBlkAllocator
    pub fn new() -> Self {
        Self
    }
}

#[async_trait]
impl BlkAllocator for EmptyBlkAllocator {
    async fn recover(&mut self) {
        // No-op for empty allocator
    }

    async fn shutdown(&self) {
        // No-op for empty allocator
    }

    fn alloc_immediate(&self, _nblks: BlkCount, _hints: &BlkAllocHints, _out_blkids: &mut BlkIds) -> BlkAllocStatus {
        panic!("EmptyBlkAllocator: alloc_immediate should not be called - allocation is managed externally");
    }

    async fn alloc_contiguous(&self, _nblks: BlkCount, _hints: &BlkAllocHints) -> (BlkAllocStatus, Option<BlkId>) {
        panic!("EmptyBlkAllocator: alloc_contiguous should not be called - allocation is managed externally");
    }

    async fn alloc(&self, _nblks: BlkCount, _hints: &BlkAllocHints) -> (BlkAllocStatus, BlkIds) {
        panic!("EmptyBlkAllocator: alloc should not be called - allocation is managed externally");
    }

    fn schedule_commit(&self, _bid: &BlkId) -> BlkAllocStatus {
        // No-op for empty allocator (logs don't need commit)
        BlkAllocStatus::Success
    }

    fn schedule_free(&self, _id: &BlkId) {
        // No-op for empty allocator
    }

    async fn free(&self, _bid: &BlkId) {
        // No-op for empty allocator
    }

    async fn free_batch(&self, _bids: &BlkIds) {
        // No-op for empty allocator
    }

    fn available_blks(&self) -> BlkNum {
        // Return 0 - no blocks tracked
        0
    }

    fn get_defrag_nblks(&self) -> BlkNum {
        // No fragmentation in empty allocator
        0
    }

    fn get_used_blks(&self) -> BlkNum {
        // No blocks used
        0
    }

    async fn is_blk_alloced(&self, _b: &BlkId, _is_thread_safe: bool) -> bool {
        // Always return false - no blocks are tracked
        false
    }

    fn to_string(&self) -> String {
        "EmptyBlkAllocator (no-op)".to_string()
    }

    fn get_align_size(&self) -> u32 {
        0
    }

    fn get_total_blks(&self) -> BlkNum {
        0
    }

    fn get_name(&self) -> &str {
        "empty"
    }

    fn get_blk_size(&self) -> u32 {
        0
    }

    fn get_status(&self, _log_level: i32) -> String {
        "EmptyBlkAllocator: no-op allocator".to_string()
    }
}
