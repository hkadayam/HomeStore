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

use std::{
    sync::atomic::{AtomicBool, AtomicU32, Ordering},
    time::SystemTime,
};

use async_trait::async_trait;
use iomgr::iomgr;

use super::blk_allocator::{BlkAllocConfig, BlkAllocator};
use crate::common::{
    max_blks_per_blkid, AllocatorId, BlkAllocHints, BlkAllocStatus, BlkCount, BlkId, BlkIds, BlkNum, ChunkNum,
};

const APPEND_BLKALLOC_SB_MAGIC: u64 = 0xd0d0d02b;
const APPEND_BLKALLOC_SB_VERSION: u32 = 0x1;

/// Superblock structure for AppendBlkAllocator
#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
pub struct AppendBlkSb {
    magic: u64,
    version: u32,
    allocator_id: AllocatorId,
    freeable_nblks: BlkNum,
    commit_offset: BlkNum,
}

impl Default for AppendBlkSb {
    fn default() -> Self {
        Self {
            magic: APPEND_BLKALLOC_SB_MAGIC,
            version: APPEND_BLKALLOC_SB_VERSION,
            allocator_id: 0,
            freeable_nblks: 0,
            commit_offset: 0,
        }
    }
}

/// AppendBlkAllocator - Sequential block allocator
///
/// The assumption for AppendBlkAllocator:
/// 1. Operations (alloc/free) are being called by multiple threads
/// 2. cp_flush will be triggered in a different thread
///
/// This allocator is suitable for append-only workloads like logging
pub struct AppendBlkAllocator {
    // Configuration
    name: String,
    blk_size: u32,
    align_size: u32,
    num_blks: BlkNum,
    chunk_id: ChunkNum,
    is_persistent: bool,

    // State (atomic for thread-safety)
    last_append_offset: AtomicU32, // Last appended offset in blocks (in-memory)
    freeable_nblks: AtomicU32,     // Count of blocks freed (fragmented)
    commit_offset: AtomicU32,      // Offset in on-disk version
    is_dirty: AtomicBool,          // Dirty flag for CP

    // Superblock (simplified - ignoring metablk integration)
    sb: AppendBlkSb,
}

impl AppendBlkAllocator {
    /// Create a new AppendBlkAllocator
    pub async fn new(cfg: &BlkAllocConfig, need_format: bool, id: AllocatorId) -> Self {
        let mut allocator = Self {
            name: format!("AppendBlkAlloc_chunk_{}", id),
            blk_size: cfg.blk_size,
            align_size: cfg.align_size,
            num_blks: cfg.capacity,
            chunk_id: id,
            is_persistent: cfg.persistent,
            last_append_offset: AtomicU32::new(0),
            freeable_nblks: AtomicU32::new(0),
            commit_offset: AtomicU32::new(0),
            is_dirty: AtomicBool::new(false),
            sb: AppendBlkSb::default(),
        };

        if need_format {
            allocator.freeable_nblks.store(0, Ordering::SeqCst);
            allocator.last_append_offset.store(0, Ordering::SeqCst);
            allocator.commit_offset.store(0, Ordering::SeqCst);
        }

        allocator.sb.allocator_id = id;
        allocator.sb.commit_offset = allocator.last_append_offset.load(Ordering::SeqCst);
        allocator.sb.freeable_nblks = allocator.freeable_nblks.load(Ordering::SeqCst);

        allocator
    }

    /// Get number of fragmented blocks
    pub fn get_defrag_nblks_internal(&self) -> BlkNum { self.freeable_nblks.load(Ordering::Relaxed) }
}

#[async_trait]
impl BlkAllocator for AppendBlkAllocator {
    async fn alloc_contiguous(&self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, Option<BlkId>) {
        // AppendBlkAllocator always allocates contiguous blocks
        let (status, bids) = self.alloc(nblks, hints).await;
        if status == BlkAllocStatus::Success || (status == BlkAllocStatus::Partial && hints.partial_alloc_ok) {
            (status, bids.first().copied())
        } else {
            (status, None)
        }
    }

    fn alloc(&mut self, nblks: BlkCount, hint: &BlkAllocHints, out_bid: &mut BlkId) -> BlkAllocStatus {
        let mut avail_blks = self.available_blks();

        // Account for reserved blocks
        if let Some(reserved) = hint.reserved_blks {
            avail_blks = if avail_blks > reserved { avail_blks - reserved } else { 0 };
        }

        if avail_blks < nblks as BlkNum {
            log::error!(
                "No space left to serve request nblks: {}, available_blks: {}, actual available_blks(exclude reserved blks): {}",
                nblks, self.available_blks(), avail_blks
            );
            *out_bid = BlkId::new(0, 0, self.chunk_id);
            return BlkAllocStatus::SpaceFull;
        } else if nblks > max_blks_per_blkid() as BlkCount {
            log::error!("Can't serve request nblks: {} larger than max_blks_in_op: {}", nblks, max_blks_per_blkid());
            *out_bid = BlkId::new(0, 0, self.chunk_id);
            return BlkAllocStatus::Failed;
        }

        // Allocate contiguously from the append offset
        let blk_num = self.last_append_offset.fetch_add(nblks as BlkNum, Ordering::SeqCst);
        *out_bid = BlkId::new(blk_num, nblks, self.chunk_id);

        BlkAllocStatus::Success
    }

    /// Allocate nblks asynchronously on a random reactor
    async fn async_alloc(&mut self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, BlkIds) {
        let reactor_count = iomgr().num_reactors();
        // Use current time nanos for pseudo-random reactor selection
        let nanos = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos();
        let reactor_id = (nanos as usize) % reactor_count;
        
        let self_ptr = self as *mut Self as usize;
        let hints_copy = hints.clone();
        
        iomgr()
            .spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                let allocator = unsafe { &mut *(self_ptr as *mut AppendBlkAllocator) };
                let mut out_bid = BlkId::default();
                let status = allocator.alloc(nblks, &hints_copy, &mut out_bid);
                let mut out_blkids = BlkIds::new();
                if status == BlkAllocStatus::Success {
                    out_blkids.push(out_bid);
                }
                (status, out_blkids)
            })
            .await
    }

    fn commit(&mut self, blkid: &BlkId) -> BlkAllocStatus {
        debug_assert!(self.is_blk_alloced(blkid, false), "Trying to commit on disk for unallocated blkid={:?}", blkid);

        let new_offset = blkid.blk_num() + blkid.blk_count() as BlkNum;
        let mut cur_offset = self.commit_offset.load(Ordering::SeqCst);
        let mut modified = true;

        loop {
            if cur_offset >= new_offset {
                // Already allocated
                modified = false;
                break;
            }
            match self.commit_offset.compare_exchange_weak(cur_offset, new_offset, Ordering::SeqCst, Ordering::SeqCst) {
                Ok(_) => break,
                Err(x) => cur_offset = x,
            }
        }

        if modified {
            self.is_dirty.store(true, Ordering::SeqCst);
        }
        BlkAllocStatus::Success
    }

    fn free(&mut self, bid: &BlkId) {
        self.freeable_nblks.fetch_add(bid.blk_count() as BlkNum, Ordering::SeqCst);
        self.is_dirty.store(true, Ordering::SeqCst);
    }

    fn available_blks(&self) -> BlkNum { self.get_total_blks() - self.get_used_blks() }

    fn get_defrag_nblks(&self) -> BlkNum { self.freeable_nblks.load(Ordering::Relaxed) }

    fn get_used_blks(&self) -> BlkNum { self.last_append_offset.load(Ordering::Relaxed) }

    async fn is_blk_alloced(&self, in_bid: &BlkId, _is_thread_safe: bool) -> bool { in_bid.blk_num() < self.get_used_blks() }



    fn recovery_completed(&mut self) {
        // No-op for AppendBlkAllocator
    }

    fn reset(&mut self) {
        self.last_append_offset.store(0, Ordering::SeqCst);
        self.freeable_nblks.store(0, Ordering::SeqCst);
        self.commit_offset.store(0, Ordering::SeqCst);
        self.is_dirty.store(true, Ordering::SeqCst);
    }

    fn to_string(&self) -> String {
        format!(
            "{}, last_append_offset: {} fragmented_nblks={}",
            self.get_name(),
            self.last_append_offset.load(Ordering::Relaxed),
            self.get_defrag_nblks()
        )
    }

    fn get_align_size(&self) -> u32 { self.align_size }

    fn get_total_blks(&self) -> BlkNum { self.num_blks }

    fn get_name(&self) -> &str { &self.name }

    fn is_persistent(&self) -> bool { self.is_persistent }

    fn get_blk_size(&self) -> u32 { self.blk_size }

    fn get_status(&self, _log_level: i32) -> String {
        format!(
            r#"{{
    "total_blks": {},
    "next_append_blk_num": {},
    "commit_offset": {},
    "freeable_nblks": {}
}}"#,
            self.get_total_blks(),
            self.last_append_offset.load(Ordering::Relaxed),
            self.commit_offset.load(Ordering::Relaxed),
            self.freeable_nblks.load(Ordering::Relaxed)
        )
    }
}

impl AppendBlkAllocator {
    /// Checkpoint flush - persist state to disk
    pub fn cp_flush(&mut self) {
        // Check if current cp's context has dirty buffer already
        if self.is_dirty.swap(false, Ordering::SeqCst) {
            self.sb.commit_offset = self.commit_offset.load(Ordering::SeqCst);
            self.sb.freeable_nblks = self.freeable_nblks.load(Ordering::SeqCst);

            // Note: Actual write to metablk is ignored as requested
            // Copy values to avoid unaligned references to packed struct
            let commit_offset = self.sb.commit_offset;
            let freeable_nblks = self.sb.freeable_nblks;
            log::debug!(
                "CP flush for {}: commit_offset={}, freeable_nblks={}",
                self.name,
                commit_offset,
                freeable_nblks
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_append_allocator_basic() {
        let cfg = BlkAllocConfig::new(4096, 4096, 1024 * 1024, false, "test".to_string());
        let mut alloc = AppendBlkAllocator::new(&cfg, true, 0);

        // Test allocation
        let mut bid = BlkId::default();
        let status = alloc.alloc(10, &BlkAllocHints::default(), &mut bid);
        assert_eq!(status, BlkAllocStatus::Success);
        assert_eq!(bid.blk_num(), 0);
        assert_eq!(bid.blk_count(), 10);

        // Test sequential allocation
        let mut bid2 = BlkId::default();
        let status = alloc.alloc(5, &BlkAllocHints::default(), &mut bid2);
        assert_eq!(status, BlkAllocStatus::Success);
        assert_eq!(bid2.blk_num(), 10);
        assert_eq!(bid2.blk_count(), 5);
    }

    #[test]
    fn test_append_allocator_free() {
        let cfg = BlkAllocConfig::new(4096, 4096, 1024 * 1024, false, "test".to_string());
        let mut alloc = AppendBlkAllocator::new(&cfg, true, 0);

        let mut bid = BlkId::default();
        alloc.alloc(10, &BlkAllocHints::default(), &mut bid);

        let defrag_before = alloc.get_defrag_nblks();
        alloc.free(&bid);
        let defrag_after = alloc.get_defrag_nblks();

        assert_eq!(defrag_after, defrag_before + 10);
    }
}
