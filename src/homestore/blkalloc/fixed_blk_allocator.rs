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
    collections::VecDeque,
    sync::{
        atomic::{AtomicI64, Ordering},
        Arc,
    },
    time::SystemTime,
};

use async_trait::async_trait;
use iomgr::iomgr;
use sisl::{reactor_local::ReactorLocal, RcuPtr};

use super::{
    bitmap_blk_allocator::BitmapBlkAllocator,
    blk_allocator::{BlkAllocConfig, BlkAllocator},
    segment_manager::{PortionBase, SegmentManager},
};
use crate::common::{AllocatorId, BlkAllocHints, BlkAllocStatus, BlkCount, BlkId, BlkIds, BlkNum, ChunkNum};

/// State of the FixedBlkAllocator
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum State {
    #[default]
    Recovering,
    Active,
}

/// FixedBlkAllocator - Fast allocator for fixed-size single-block allocations
///
/// Features:
/// - Only allocates blocks of size 1
/// - Caches ALL free blocks per reactor using ReactorLocal<VecDeque>
/// - Does not support temperature-based allocation
/// - Allocates on first-come-first-serve basis (FIFO)
/// - Uses ondisk_allocator for persistent on-disk tracking
/// - ONLY supports persistent mode (non-persistent not supported)
///
/// Recovery workflow:
/// - During recovery: alloc() calls ondisk_allocator.alloc(), free() calls ondisk_allocator.free()
/// - After recovery_completed(): alloc() pulls from free_queues, free() pushes to free_queues
///
/// This is suitable for workloads that need many fixed-size allocations
pub struct FixedBlkAllocator {
    // Ondisk allocator (always present - only persistent mode supported)
    ondisk_allocator: BitmapBlkAllocator,

    // State - RcuPtr for lock-free reads (similar to C++ urcu_scoped_ptr)
    // Readers get consistent snapshot, writer updates atomically
    state: RcuPtr<State>,

    // Direct fields from BitmapBlkAllocator
    name: String,
    blk_size: u32,
    align_size: u32,
    num_blks: BlkNum,
    chunk_id: ChunkNum,
    alloced_blk_count: AtomicI64,
    reactors_count: usize,

    // Shared segment manager
    segment_mgr: Arc<SegmentManager>,

    // Per-reactor free block queues: ReactorLocal<VecDeque> for FIFO allocation
    free_queues: ReactorLocal<VecDeque<BlkNum>>,
}

impl FixedBlkAllocator {
    /// Create a new FixedBlkAllocator
    ///
    /// If buffer is None: Create fresh allocator in Active state
    /// If buffer is Some: Load from buffer and start in Recovering state
    pub fn new(
        cfg: &BlkAllocConfig, buffer: Option<iomgr::IOBuffer>, id: AllocatorId, num_reactors: u32,
    ) -> Result<Self, String> {
        // FixedBlkAllocator only supports persistent mode
        assert!(cfg.persistent, "FixedBlkAllocator only supports persistent mode");

        let num_blks = cfg.capacity;
        let reactors_count = num_reactors.max(1) as usize;

        // If buffer is provided, it means we were asked to load from disk, so we start in Recovering state
        let initial_state = if buffer.is_none() { State::Active } else { State::Recovering };

        // Create BitmapBlkAllocator and get its Arc<SegmentManager>
        type PortionCreator = fn(u32, BlkNum, BlkNum) -> Box<dyn PortionBase>;
        let ondisk_allocator = BitmapBlkAllocator::new(cfg, buffer, id, num_reactors, None::<PortionCreator>)?;
        let segment_mgr = ondisk_allocator.segment_mgr();

        Ok(Self {
            ondisk_allocator,
            state: RcuPtr::new(initial_state),
            name: cfg.unique_name.clone(),
            blk_size: cfg.blk_size,
            align_size: cfg.align_size,
            num_blks,
            chunk_id: id,
            alloced_blk_count: AtomicI64::new(0),
            reactors_count,
            segment_mgr,
            free_queues: ReactorLocal::new(|| VecDeque::new()),
        })
    }

    /// Reload free blocks from disk bitmap into per-reactor free queues. This runs on each reactor
    /// and loads blocks from its portion.
    ///
    /// Important Note: While reloading the free blocks, it ensures that each reactor gets the
    /// portion corresponding to its ondisk version, however, once system is running it is not
    /// guaranteed or even needed to maintain that. Any blk free operation irrespective of which
    /// reactor it belonged can be called on any reactors and it will enqueue in the calling
    /// reactor's free queue.
    fn reload_per_reactor_q(&self) {
        let my_queue = self.free_queues.get();
        let disk_bm = &self.ondisk_allocator.disk_bm;

        // Iterate through all segments
        for seg_idx in 0..self.segment_mgr.segments_count() {
            if let Some(seg) = self.segment_mgr.get_segment(seg_idx as usize) {
                // Get my portion directly by reactor_id
                let my_portion = seg.my_ondisk_portion();

                // Iterate through blocks in my portion
                for blk_num in my_portion.start_blk()..my_portion.end_blk() {
                    // Check if block is free (not set in disk bitmap)
                    if !disk_bm.is_bits_set(blk_num as u64, 1) {
                        my_queue.push_back(blk_num);
                    }
                }
            }
        }
    }

    /// Test helper: Return current state (for unit tests only)
    #[cfg(test)]
    fn current_state(&self) -> State { *self.state.read() }
}

// Implementation of BlkAllocator trait for FixedBlkAllocator
// This demonstrates composition-based "inheritance" in Rust:
// - Override methods (alloc, free, etc.) provide FixedBlkAllocator-specific behavior
// - Delegate method (reserve) calls base.method()
// - Accessor methods forward to base fields
#[async_trait]
impl BlkAllocator for FixedBlkAllocator {
    async fn alloc_contiguous(&self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, Option<BlkId>) {
        // FixedBlkAllocator always allocates contiguous blocks since it's fixed size
        let (status, bids) = self.alloc(nblks, hints).await;
        if status == BlkAllocStatus::Success || (status == BlkAllocStatus::Partial && hints.partial_alloc_ok) {
            (status, bids.first().copied())
        } else {
            (status, None)
        }
    }

    /// Override: FixedBlkAllocator-specific allocation
    /// During recovery: Allocate directly from disk bitmap
    /// After recovery: Pop from per-reactor free queue (FIFO)
    fn alloc_immediate(&self, nblks: BlkCount, hint: &BlkAllocHints, out_bids: &mut BlkIds) -> BlkAllocStatus {
        if nblks != 1 {
            log::error!("FixedBlkAllocator only supports single block allocation, requested: {}", nblks);
            return BlkAllocStatus::Failed;
        }

        let state_guard = self.state.read();
        let status = if *state_guard == State::Recovering {
            // During recovery: allocate directly from disk bitmap
            self.ondisk_allocator.alloc_immediate(nblks, hint, out_bids)
        } else {
            // After recovery: pop from per-reactor free queue (FIFO)
            let my_queue = self.free_queues.get();
            if let Some(blk_num) = my_queue.pop_front() {
                out_bids.push(BlkId::new(blk_num, 1, self.chunk_id));
                BlkAllocStatus::Success
            } else {
                BlkAllocStatus::SpaceFull
            }
        };

        if status == BlkAllocStatus::Success {
            self.alloced_blk_count.fetch_add(nblks as i64, Ordering::Relaxed);
        }
        status
    }

    /// Allocate nblks asynchronously - tries current reactor first, then others
    async fn alloc(&self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, BlkIds) {
        // Step 1: Try allocating on current reactor first
        let mut out_blkids = BlkIds::new();
        let status = self.alloc_immediate(nblks, hints, &mut out_blkids);
        if status == BlkAllocStatus::Success {
            return (status, out_blkids);
        }
        
        // Step 2: If local allocation failed, try other reactors
        let reactor_count = iomgr().num_reactors();
        let current_reactor = iomgr().current_reactor_id();
        
        // Use current time nanos for pseudo-random starting point
        let nanos = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_nanos();
        let start_reactor = (nanos as usize) % reactor_count;
        
        let self_ptr = self as *const Self as usize;
        
        // Try all other reactors starting from random position
        for i in 0..reactor_count {
            let reactor_id = (start_reactor + i) % reactor_count;
            if reactor_id == current_reactor {
                continue; // Skip current reactor, already tried
            }
            
            let hints_copy = hints.clone();
            let (status, bids) = iomgr()
                .spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                    let allocator = unsafe { &*(self_ptr as *const FixedBlkAllocator) };
                    let mut out_blkids = BlkIds::new();
                    let status = allocator.alloc_immediate(nblks, &hints_copy, &mut out_blkids);
                    (status, out_blkids)
                })
                .await;
            
            if status == BlkAllocStatus::Success {
                return (status, bids);
            }
        }
        
        // All reactors exhausted
        (BlkAllocStatus::SpaceFull, BlkIds::new())
    }

    fn schedule_commit(&self, bid: &BlkId) -> BlkAllocStatus { self.ondisk_allocator.schedule_commit(bid) }

    /// FixedBlkAllocator free will push to front of per-reactor free queue (FIFO). It does not
    /// maintain portion boundary and thus a reactor might get a blk from a portion that is not
    /// technically assigned to that reactor in persistent portion of allocation. This is fine and
    /// still thread safe because ondisk allocator will queue the free and actually free up in
    /// correct reactor.
    fn schedule_free(&self, bid: &BlkId) {
        debug_assert_eq!(bid.blk_count(), 1, "FixedBlkAllocator only supports single block, got: {}", bid.blk_count());
        self.ondisk_allocator.schedule_free(bid);

        let state_guard = self.state.read();
        if *state_guard == State::Active {
            // In active state: push to front of per-reactor free queue (FIFO)
            let my_queue = self.free_queues.get();
            my_queue.push_front(bid.blk_num());
        }
        
        self.alloced_blk_count.fetch_sub(bid.blk_count() as i64, Ordering::Relaxed);
    }

    /// Free a single block asynchronously and wait for completion
    async fn free(&self, bid: &BlkId) {
        let mut bids = BlkIds::new();
        bids.push(*bid);
        self.free_batch(&bids).await;
    }

    /// Free multiple blocks asynchronously across appropriate reactors. Use this method if you are
    /// in async function and you want these blks to be again allocable after calling await.
    async fn free_batch(&self, bids: &BlkIds) {
        // Step 1: Call ondisk allocator's free_batch
        self.ondisk_allocator.free_batch(bids).await;

        // Step 2: Check if active and split BlkIds equally across all reactors
        {
            let state_guard = self.state.read();
            if *state_guard != State::Active {
                return;
            }
        }

        // Step 3: If active, push to free_queues on each reactor
        let parts: Vec<Vec<BlkId>> =
            bids.iter().enumerate().fold(vec![Vec::new(); self.reactors_count], |mut acc, (i, bid)| {
                let reactor_id = i % self.reactors_count;
                acc[reactor_id].push(*bid);
                acc
            });

        // Step 4: Spawn on each reactor and await completion
        let self_ptr_addr = self as *const FixedBlkAllocator as usize;
        for (reactor_id, reactor_bids) in parts.into_iter().enumerate() {
            if reactor_bids.is_empty() {
                continue;
            }
            iomgr()
                .spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                    unsafe {
                        let allocator = &*(self_ptr_addr as *const FixedBlkAllocator);
                        let my_queue = allocator.free_queues.get();
                        for bid in reactor_bids {
                            debug_assert_eq!(bid.blk_count(), 1, "FixedBlkAllocator only supports single block");
                            my_queue.push_front(bid.blk_num());
                        }
                    }
                })
                .await;
        }

        // Step 5: Update alloced_blk_count (always, regardless of state)
        self.alloced_blk_count.fetch_sub(bids.len() as i64, Ordering::Relaxed);
    }

    fn available_blks(&self) -> BlkNum {
        self.get_total_blks() - self.get_used_blks()
    }

    fn get_defrag_nblks(&self) -> BlkNum {
        // Fixed allocator doesn't fragment since all blocks are same size
        0
    }

    fn get_used_blks(&self) -> BlkNum { self.alloced_blk_count.load(Ordering::Relaxed) as BlkNum }

    async fn is_blk_alloced(&self, b: &BlkId, is_thread_safe: bool) -> bool {
        self.ondisk_allocator.is_blk_alloced(b, is_thread_safe).await
    }

    async fn recover(&mut self) {
        // Initialize alloced_blk_count from ondisk allocator
        let used_blks = self.ondisk_allocator.get_used_blks();
        self.alloced_blk_count.store(used_blks as i64, Ordering::Relaxed);

        // Spawn reload_reactor() on each reactor to populate free queues from disk bitmap
        for reactor_id in 0..self.reactors_count {
            let self_ptr_addr = self as *const FixedBlkAllocator as usize;
            iomgr().spawn_detached(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                unsafe {
                    let allocator = &*(self_ptr_addr as *const FixedBlkAllocator);
                    allocator.reload_per_reactor_q();
                }
            });
        }

        // Move to active state (atomic update via RCU)
        self.state.update(State::Active);
    }

    fn to_string(&self) -> String {
        format!("{}: Total Blks={}, Available Blks={}", self.name, self.get_total_blks(), self.available_blks())
    }

    fn get_align_size(&self) -> u32 { self.align_size }

    fn get_total_blks(&self) -> BlkNum { self.num_blks }

    fn get_name(&self) -> &str { &self.name }

    fn get_blk_size(&self) -> u32 { self.blk_size }

    fn get_status(&self, _log_level: i32) -> String {
        format!(
            r#"{{
    "total_blks": {},
    "available_blks": {},
    "used_blks": {}
}}"#,
            self.get_total_blks(),
            self.available_blks(),
            self.get_used_blks()
        )
    }
}

// Helper methods moved to impl block above

// TODO: Update tests to use BlkIds instead of BlkId
#[cfg(disabled_test)]
mod tests {
    use super::*;
    use crate::{
        blkalloc::blk_allocator::BlkAllocConfig,
        common::{BlkAllocStatus, BlkId, BlkIds, BlkNum},
    };

    fn make_cfg(nblks: BlkNum) -> BlkAllocConfig {
        let size_bytes = nblks as u64 * 4096u64;
        BlkAllocConfig::new(4096, 4096, size_bytes, false, "fixed_test".to_string(), 1)
    }

    #[tokio::test]
    async fn state_transitions_recovery_to_active() {
        let cfg = make_cfg(16);
        let mut a = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");
        assert_eq!(a.current_state(), State::Recovering);
        a.load_finalize().await;
        assert_eq!(a.current_state(), State::Active);
    }

    #[tokio::test]
    async fn alloc_and_free_under_active() {
        let cfg = make_cfg(4);
        let mut a = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");
        a.load_finalize().await;
        let mut out = BlkId::new(0, 0, 0);
        assert_eq!(a.alloc_contiguous(&mut out), BlkAllocStatus::Success);
        assert_eq!(out.blk_count(), 1);
        let used1 = a.get_used_blks();
        a.free(&out);
        let used2 = a.get_used_blks();
        assert!(used2 <= used1);
    }

    #[test]
    fn fixed_allocator_basic() {
        let cfg = BlkAllocConfig::new(4096, 4096, 1024 * 1024, false, "test".to_string(), 1);
        let mut alloc = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");
        let mut bid = BlkId::default();
        let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
        assert_eq!(status, BlkAllocStatus::Success);
        assert_eq!(bid.blk_count(), 1);
        let mut bid2 = BlkId::default();
        let status = alloc.alloc(5, &BlkAllocHints::default(), &mut bid2);
        assert_eq!(status, BlkAllocStatus::Failed);
    }

    #[test]
    fn fixed_allocator_free_and_reuse() {
        let cfg = BlkAllocConfig::new(4096, 4096, 4096 * 10, false, "test".to_string(), 1);
        let mut alloc = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");
        let mut bids = Vec::new();
        for _ in 0..10 {
            let mut bid = BlkId::default();
            let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
            assert_eq!(status, BlkAllocStatus::Success);
            bids.push(bid);
        }
        let mut bid = BlkId::default();
        let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
        assert_eq!(status, BlkAllocStatus::SpaceFull);
        alloc.free(&bids[0]);
        let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
        assert_eq!(status, BlkAllocStatus::Success);
    }

    #[tokio::test]
    async fn test_persistent_alloc_free_serialize() {
        // Create persistent allocator with 100 blocks
        let cfg = BlkAllocConfig::new(4096, 4096, 4096 * 100, true, "test_persistent".to_string(), 1);
        let mut alloc = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");

        // Move to active state
        alloc.load_finalize().await;

        // Allocate some blocks
        let mut allocated_bids = Vec::new();
        for i in 0..10 {
            let mut bid = BlkId::default();
            let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
            assert_eq!(status, BlkAllocStatus::Success, "Failed to allocate block {}", i);
            allocated_bids.push(bid);
        }

        let used_after_alloc = alloc.get_used_blks();
        assert_eq!(used_after_alloc, 10, "Should have 10 blocks allocated");

        // Free some blocks
        alloc.free(&allocated_bids[2]);
        alloc.free(&allocated_bids[5]);
        alloc.free(&allocated_bids[7]);

        let used_after_free = alloc.get_used_blks();
        assert_eq!(used_after_free, 7, "Should have 7 blocks allocated after freeing 3");

        // Acquire buffer guard - this should finalize pending operations
        let guard = alloc.base.acquire_underlying_buffer().await;
        let buffer = guard.buffer().expect("Should have a buffer for persistent allocator");

        // Verify we can read the buffer
        assert!(buffer.len() > 0, "Buffer should have data");

        // Buffer is now serialized and ready for checkpoint
        let serialized_len = buffer.len();
        println!("Serialized bitmap size: {} bytes", serialized_len);

        drop(guard);

        // After releasing guard, we can continue operations
        let mut bid = BlkId::default();
        let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
        assert_eq!(status, BlkAllocStatus::Success, "Should be able to allocate after releasing guard");
    }

    #[tokio::test]
    async fn test_recovery_workflow() {
        // Phase 1: Create fresh allocator and do some operations
        let cfg = BlkAllocConfig::new(4096, 4096, 4096 * 50, true, "test_recovery".to_string(), 1);
        let mut alloc = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");

        // Move to active state
        alloc.load_finalize().await;

        // Allocate blocks
        let mut allocated_bids = Vec::new();
        for i in 0..20 {
            let mut bid = BlkId::default();
            let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
            assert_eq!(status, BlkAllocStatus::Success, "Failed to allocate block {}", i);
            allocated_bids.push(bid);
        }

        // Free some blocks
        for i in [3, 7, 11, 15].iter() {
            alloc.free(&allocated_bids[*i]);
        }

        let used_before_cp = alloc.get_used_blks();
        assert_eq!(used_before_cp, 16, "Should have 16 blocks in use");

        // Checkpoint: acquire buffer and save it
        let guard = alloc.base.acquire_underlying_buffer().await;
        let buffer = guard.buffer().expect("Should have buffer");

        // Clone the buffer for recovery simulation
        let mut saved_buffer = iomgr::IOBuffer::new(buffer.len());
        saved_buffer.as_mut_slice().copy_from_slice(buffer.as_slice());
        drop(guard);

        // Phase 2: Simulate restart - create new allocator from saved state
        let mut alloc2 = FixedBlkAllocator::new(&cfg, Some(saved_buffer), 0, 1).expect("Failed to create allocator");
        assert_eq!(alloc2.current_state(), State::Recovering, "New allocator should start in Recovering state");

        // During recovery, reserve and free specific blocks
        let recovery_bid = BlkId::new(25, 1, 0);
        let status = alloc2.reserve(&recovery_bid);
        assert_eq!(status, BlkAllocStatus::Success, "Should be able to reserve during recovery");

        let free_bid = BlkId::new(30, 1, 0);
        alloc2.free(&free_bid);

        // Complete recovery - this finalizes pending ops and reloads free queues
        alloc2.load_finalize().await;
        assert_eq!(alloc2.current_state(), State::Active, "Should be in Active state after recovery");

        // Now allocations should come from free queues
        let mut new_bid = BlkId::default();
        let status = alloc2.alloc(1, &BlkAllocHints::default(), &mut new_bid);
        assert_eq!(status, BlkAllocStatus::Success, "Should be able to allocate after recovery");

        // Verify state is consistent
        let used_after_recovery = alloc2.get_used_blks();
        println!("Blocks used after recovery: {}", used_after_recovery);
    }

    #[tokio::test]
    async fn test_operations_during_buffer_acquisition() {
        let cfg = BlkAllocConfig::new(4096, 4096, 4096 * 80, true, "test_guard_ops".to_string(), 1);
        let mut alloc = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");

        // Move to active state
        alloc.load_finalize().await;

        // Phase 1: Allocate blocks
        let mut phase1_bids = Vec::new();
        for i in 0..15 {
            let mut bid = BlkId::default();
            let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
            assert_eq!(status, BlkAllocStatus::Success, "Phase 1: Failed to allocate block {}", i);
            phase1_bids.push(bid);
        }

        // Phase 2: Free some blocks
        for i in [2, 5, 9, 12].iter() {
            alloc.free(&phase1_bids[*i]);
        }

        let used_before_guard = alloc.get_used_blks();
        assert_eq!(used_before_guard, 11, "Should have 11 blocks after phase 1 & 2");

        // Phase 3: Acquire guard - this calls finalize_pending_items()
        {
            let guard = alloc.base.acquire_underlying_buffer().await;

            // During guard lifetime, verify buffer is accessible
            let buffer = guard.buffer().expect("Should have buffer");
            assert!(buffer.len() > 0, "Buffer should have data");

            // Note: We cannot call finalize_pending_items() here because guard holds &mut self
            // But we can still queue operations via reserve/free (during recovery)

            println!("Buffer acquired, size: {} bytes", buffer.len());
        } // Guard dropped here

        // Phase 4: After guard is released, allocate more blocks
        let mut phase4_bids = Vec::new();
        for i in 0..10 {
            let mut bid = BlkId::default();
            let status = alloc.alloc(1, &BlkAllocHints::default(), &mut bid);
            assert_eq!(status, BlkAllocStatus::Success, "Phase 4: Failed to allocate block {}", i);
            phase4_bids.push(bid);
        }

        let used_after_guard = alloc.get_used_blks();
        assert_eq!(used_after_guard, 21, "Should have 21 blocks after phase 4");

        // Phase 5: Free some phase 4 blocks
        for i in [1, 4, 7].iter() {
            alloc.free(&phase4_bids[*i]);
        }

        let used_final = alloc.get_used_blks();
        assert_eq!(used_final, 18, "Should have 18 blocks after final frees");

        // Phase 6: Acquire guard again and verify buffer is accessible
        let guard = alloc.base.acquire_underlying_buffer().await;
        let buffer = guard.buffer().expect("Should have buffer");

        println!("Final buffer size: {} bytes, used blocks: {}", buffer.len(), used_final);

        // Verify we can checkpoint multiple times
        assert!(buffer.len() > 0, "Buffer should be valid for checkpoint");
    }

    #[tokio::test]
    async fn test_recovery_with_mixed_operations() {
        // Test scenario: alloc -> free -> serialize -> load -> operations during recovery ->
        // recovery_completed -> alloc -> free -> verify

        let cfg = BlkAllocConfig::new(4096, 4096, 4096 * 60, true, "test_mixed".to_string(), 1);

        // === Part 1: Initial operations ===
        let mut alloc = FixedBlkAllocator::new(&cfg, None, 0, 1).expect("Failed to create allocator");
        alloc.load_finalize().await;

        // Allocate 25 blocks
        let mut bids = Vec::new();
        for i in 0..25 {
            let mut bid = BlkId::default();
            assert_eq!(
                alloc.alloc(1, &BlkAllocHints::default(), &mut bid),
                BlkAllocStatus::Success,
                "Initial alloc {}",
                i
            );
            bids.push(bid);
        }

        // Free blocks: 5, 10, 15, 20
        for idx in [5, 10, 15, 20].iter() {
            alloc.free(&bids[*idx]);
        }

        assert_eq!(alloc.get_used_blks(), 21, "After initial ops");

        // Save block numbers for later use
        let blk3 = bids[3].blk_num();
        let blk8 = bids[8].blk_num();

        // Serialize
        let guard = alloc.base.acquire_underlying_buffer().await;
        let buffer = guard.buffer().expect("Buffer");
        let mut saved = iomgr::IOBuffer::new(buffer.len());
        saved.as_mut_slice().copy_from_slice(buffer.as_slice());
        drop(guard);
        drop(alloc);

        // === Part 2: Recovery with operations ===
        let mut alloc2 = FixedBlkAllocator::new(&cfg, Some(saved), 0, 1).expect("Failed to create allocator");
        assert_eq!(alloc2.current_state(), State::Recovering);

        // During recovery: reserve specific blocks
        assert_eq!(alloc2.reserve(&BlkId::new(40, 1, 0)), BlkAllocStatus::Success);
        assert_eq!(alloc2.reserve(&BlkId::new(41, 1, 0)), BlkAllocStatus::Success);

        // During recovery: free specific blocks (these queue in pending_frees)
        alloc2.free(&BlkId::new(blk3, 1, 0));
        alloc2.free(&BlkId::new(blk8, 1, 0));

        // Complete recovery
        alloc2.load_finalize().await;
        assert_eq!(alloc2.current_state(), State::Active);

        // === Part 3: Post-recovery operations ===
        // Allocate more blocks
        let mut post_bids = Vec::new();
        for i in 0..5 {
            let mut bid = BlkId::default();
            assert_eq!(
                alloc2.alloc(1, &BlkAllocHints::default(), &mut bid),
                BlkAllocStatus::Success,
                "Post-recovery alloc {}",
                i
            );
            post_bids.push(bid);
        }

        // Free some post-recovery blocks
        alloc2.free(&post_bids[1]);
        alloc2.free(&post_bids[3]);

        // === Part 4: Final verification ===
        let guard = alloc2.base.acquire_underlying_buffer().await;
        let buffer = guard.buffer().expect("Final buffer");

        // Verify buffer is valid
        assert!(buffer.len() > 0);

        let final_used = alloc2.get_used_blks();
        println!("Final used blocks: {}", final_used);

        // Verify we can continue operations after final checkpoint
        assert!(final_used > 0, "Should have allocated blocks");
    }
}
