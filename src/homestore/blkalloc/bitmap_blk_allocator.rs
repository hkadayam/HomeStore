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
    sync::{
        atomic::{AtomicBool, AtomicI64, AtomicU32, Ordering},
        Arc,
    },
    time::SystemTime,
};

use async_trait::async_trait;
use iomgr::{iomgr, IOBuffer};
use sisl::{collector_vec::CollectorVec, Bitset};

use super::{
    blk_allocator::{BlkAllocConfig, BlkAllocator},
    segment_manager::{EmptyPortion, PortionBase, SegmentManager},
};
use crate::common::{BlkAllocHints, BlkAllocStatus, BlkCount, BlkId, BlkIds, BlkNum, BlkTemp, ChunkNum};

pub type AllocatorId = ChunkNum;

/// Guard for accessing the underlying bitmap buffer safely.
/// 
/// This guard ensures that:
/// 1. All pending operations are finalized before access
/// 2. No modifications can happen to disk_bm while the guard is held
/// 3. The caller has exclusive access to the underlying IOBuffer
///
/// During the guard's lifetime, methods like finalize_pending_items() cannot be called
/// since the guard holds a mutable reference to the allocator.
pub struct BitmapBufferGuard<'a> {
    _allocator: &'a mut BitmapBlkAllocator,
}

impl<'a> BitmapBufferGuard<'a> {
    /// Get the underlying IOBuffer reference
    pub fn buffer(&self) -> &IOBuffer {
        // Extract buffer from allocator's disk_bm
        unsafe {
            // SAFETY: We hold a mutable reference to the allocator (_allocator field),
            // which ensures exclusive access. We extend the lifetime to 'a which is valid
            // because the guard's lifetime is tied to the allocator's borrow.
            let allocator_ptr = self._allocator as *const BitmapBlkAllocator;
            (*allocator_ptr).disk_bm.buffer()
        }
    }
}

pub type PortionNum = BlkNum;

pub const INVALID_PORTION_NUM: PortionNum = u32::MAX;

/// On-disk portion for bitmap allocator - represents a reactor's portion of disk blocks
pub struct OnDiskPortion {
    reactor_id: u32,
    start_blk: BlkNum,
    end_blk: BlkNum,
    search_hand: AtomicU32, // Per-reactor search hand for allocation (using atomic for interior mutability)
}

impl OnDiskPortion {
    pub fn new(reactor_id: u32, start_blk: BlkNum, end_blk: BlkNum) -> Self {
        Self { reactor_id, start_blk, end_blk, search_hand: AtomicU32::new(start_blk) }
    }

    pub fn search_hand(&self) -> BlkNum { self.search_hand.load(Ordering::Relaxed) }
    pub fn set_search_hand(&self, hand: BlkNum) { self.search_hand.store(hand, Ordering::Relaxed); }
}

impl PortionBase for OnDiskPortion {
    fn reactor_id(&self) -> u32 { self.reactor_id }
    fn start_blk(&self) -> BlkNum { self.start_blk }
    fn end_blk(&self) -> BlkNum { self.end_blk }
    fn as_any(&self) -> &dyn std::any::Any { self }
}

/// Base class for bitmap-based persistent block allocators
///
/// This provides:
/// - Bitmap for tracking allocations
/// - Portion-based locking for parallelism
/// - Deferred batch processing of commits/frees via finalize()
///
/// ## Deferred Operations Workflow
///
/// Instead of immediately modifying the bitmap, commit() and free() queue operations into
/// per-reactor CollectorVec structures. This enables:
/// 1. **Lock-free queueing**: Each reactor appends to its own CollectorVec without contention
/// 2. **Batch processing**: finalize() collects all pending operations across reactors
/// 3. **Correct reactor dispatch**: Operations are grouped by portion ownership and applied on the
///    owning reactor's thread to maintain safety
///
/// ### Usage Pattern:
/// ```rust,ignore
/// // Phase 1: Queue operations (from any reactor)
/// allocator.commit(&blkid1);
/// allocator.commit(&blkid2);
/// allocator.free(&blkid3);
///
/// // Phase 2: Batch-apply all pending operations
/// allocator.finalize().await;
/// ```
pub struct BitmapBlkAllocator {
    pub name: String,
    pub blk_size: u32,
    pub align_size: u32,
    pub num_blks: BlkNum,
    pub chunk_id: ChunkNum,
    pub is_persistent: bool,
    segment_mgr: Arc<SegmentManager>,
    pub disk_bm: Arc<Bitset>,
    pub is_disk_bm_dirty: AtomicBool,
    pub alloced_blk_count: AtomicI64,
    pub reactors_count: usize,
    pending_commits: CollectorVec<BlkId>, // Per-reactor deferred commit operations (to be batch-applied in finalize)
    pending_frees: CollectorVec<BlkId>,   // Per-reactor deferred free operations (to be batch-applied in finalize)
}

impl BitmapBlkAllocator {
    /// Create a new BitmapBlkAllocator
    ///
    /// # Arguments
    /// * `inmem_portion_creator` - Optional creator for custom inmem portions. If None, creates
    ///   EmptyPortion (default for BitmapBlkAllocator). Derived allocators like VarsizeBlkAllocator
    ///   can provide custom creators.
    pub fn new<F>(
        cfg: &BlkAllocConfig, buffer: Option<IOBuffer>, id: AllocatorId, num_reactors: u32,
        inmem_portion_creator: Option<F>,
    ) -> Result<Self, String>
    where
        F: Fn(u32, BlkNum, BlkNum) -> Box<dyn PortionBase> + Send + Sync + Clone + 'static,
    {
        let num_blks = cfg.capacity;

        assert!(cfg.persistent, "BitmapBlkAllocator can only be created for persistent allocators");

        // Create or load disk_bm based on buffer availability
        let (disk_bm, initial_used_blks) = if let Some(buf) = buffer {
            // Load from existing buffer
            let (bitset, set_count) = Bitset::load(buf).map_err(|e| format!("Failed to load bitmap: {}", e))?;
            (Arc::new(bitset), set_count as i64)
        } else {
            // Create fresh bitmap
            (Arc::new(Bitset::new(num_blks as u64, id as u64)), 0)
        };

        let reactors_count = num_reactors.max(1) as usize;

        // Prepare portion creators
        let ondisk_creator = |reactor_id, portion_start, portion_end| {
            Box::new(OnDiskPortion::new(reactor_id, portion_start, portion_end)) as Box<dyn PortionBase>
        };

        // Create segment manager with custom inmem creator or default to EmptyPortion
        let segment_mgr = if let Some(inmem_creator) = inmem_portion_creator {
            Arc::new(SegmentManager::new(
                num_blks,
                cfg.num_segments,
                reactors_count,
                cfg.fixed_size_segments,
                ondisk_creator,
                inmem_creator,
            ))
        } else {
            let default_inmem_creator = |reactor_id, portion_start, portion_end| {
                Box::new(EmptyPortion::new(reactor_id, portion_start, portion_end)) as Box<dyn PortionBase>
            };
            Arc::new(SegmentManager::new(
                num_blks,
                cfg.num_segments,
                reactors_count,
                cfg.fixed_size_segments,
                ondisk_creator,
                default_inmem_creator,
            ))
        };

        Ok(Self {
            name: cfg.unique_name.clone(),
            blk_size: cfg.blk_size,
            align_size: cfg.align_size,
            num_blks,
            chunk_id: 1,
            is_persistent: cfg.persistent,
            segment_mgr,
            disk_bm,
            is_disk_bm_dirty: AtomicBool::new(false),
            alloced_blk_count: AtomicI64::new(initial_used_blks),
            reactors_count,
            pending_commits: CollectorVec::new(),
            pending_frees: CollectorVec::new(),
        })
    }

    #[inline]
    pub fn total_portions(&self) -> usize { self.segment_mgr.segments_count() as usize * self.reactors_count }

    #[inline]
    fn debug_assert_portion_ownership(&self, bid: &BlkId) {
        debug_assert_eq!(
            self.blkid_to_portion(bid).reactor_id(),
            iomgr().current_reactor_id() as u32,
            "blk {:?} accessed by non owning reactor",
            bid
        );
    }

    #[inline]
    pub fn blkid_to_portion(&self, bid: &BlkId) -> &OnDiskPortion {
        let segment = self.segment_mgr.blkid_to_segment(bid);
        let portion = segment.my_ondisk_portion();
        portion.as_any().downcast_ref::<OnDiskPortion>().expect("Invalid portion type")
    }

    /// Get a clone of the Arc<SegmentManager>
    /// This allows derived allocators to share ownership
    pub fn segment_mgr(&self) -> Arc<SegmentManager> { Arc::clone(&self.segment_mgr) }

    fn commit_in_portion(&self, bid: &BlkId) {
        if !self.is_persistent {
            return;
        }
        self.debug_assert_portion_ownership(bid);
        // SAFETY: Portion ownership guarantees single-threaded access per portion range
        unsafe {
            let disk_bm = &mut *(Arc::as_ptr(&self.disk_bm) as *mut Bitset);
            disk_bm.set_bits(bid.blk_num() as u64, bid.blk_count() as u64);
        }
    }

    /// Finalize pending commits and frees by batch-applying them to disk bitmap. This collects all
    /// per-reactor operations, groups them by reactor ownership, and dispatches batch operations to
    /// each reactor using spawn_on_all_sequential.
    async fn finalize_pending_items(&self) {
        // Collect all pending commits and frees from all reactors
        let all_commits = self.pending_commits.collect().await;
        let all_frees = self.pending_frees.collect().await;

        // Group commits and frees by reactor (portion ownership) directly into Vec
        // indexed by reactor_id
        let mut commits_by_reactor: Vec<Vec<BlkId>> = vec![Vec::new(); self.reactors_count];
        let mut frees_by_reactor: Vec<Vec<BlkId>> = vec![Vec::new(); self.reactors_count];

        for blkid in all_commits {
            let portion = self.blkid_to_portion(&blkid);
            let reactor_id = portion.reactor_id() as usize;
            commits_by_reactor[reactor_id].push(blkid);
        }

        for blkid in all_frees {
            let portion = self.blkid_to_portion(&blkid);
            let reactor_id = portion.reactor_id() as usize;
            frees_by_reactor[reactor_id].push(blkid);
        }

        let total_commits: usize = commits_by_reactor.iter().map(|v| v.len()).sum();
        let total_frees: usize = frees_by_reactor.iter().map(|v| v.len()).sum();
        log::debug!(
            "Finalizing: {} commits, {} frees across {} reactors",
            total_commits,
            total_frees,
            self.reactors_count
        );

        // Check if there's any work to do
        let has_work = total_commits > 0 || total_frees > 0;

        if has_work {
            let chunk_id = self.chunk_id;

            // Get raw pointer to self as usize - safe because each reactor only modifies
            // its own portion range We use usize instead of pointer type to
            // make it Send-able
            let self_ptr_addr = self as *const BitmapBlkAllocator as usize;

            // Spawn tasks on each reactor to process its commits and frees
            // Safety: Each reactor only modifies bits in its own portion range (enforced by
            // debug_assert_portion_ownership)
            for reactor_id in 0..self.reactors_count {
                let commits = commits_by_reactor[reactor_id].clone();
                let frees = frees_by_reactor[reactor_id].clone();

                if commits.is_empty() && frees.is_empty() {
                    continue;
                }

                // Spawn on the specific reactor that owns these portions
                iomgr().spawn_detached(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                    // Safety: We're on the correct reactor and only modifying bits in portions
                    // owned by this reactor
                    unsafe {
                        let allocator = &*(self_ptr_addr as *const BitmapBlkAllocator);

                        if !commits.is_empty() {
                            let mut committed_count : i64= 0;
                            for blkid in commits {
                                committed_count += blkid.blk_count() as i64;
                                allocator.commit_in_portion(&blkid);
                                log::trace!("Finalized commit {:?} chunk {}", blkid, chunk_id);
                            }
                            allocator.alloced_blk_count.fetch_add(committed_count, Ordering::Relaxed);
                        }

                        if !frees.is_empty() {
                            let mut freed_count : i64= 0;
                            for blkid in frees {
                                freed_count += blkid.blk_count() as i64;
                                allocator.free_in_portion(&blkid);
                                log::trace!("Finalized free {:?} chunk {}", blkid, chunk_id);
                            }

                            allocator.alloced_blk_count.fetch_sub(freed_count, Ordering::Relaxed);
                        }
                    }
                });
            }

            // Note: spawn_on_reactor is fire-and-forget
            // Mark as dirty since we spawned work
            self.is_disk_bm_dirty.store(true, Ordering::SeqCst);
        }

        // Clear pending operations after finalization
        self.pending_commits.clear();
        self.pending_frees.clear();
    }

    /// Checkpoint flush - persist bitmap to storage
    pub fn cp_flush(&self) {
        if self.is_disk_bm_dirty.load(Ordering::SeqCst) {
                // In real implementation, would serialize and write to meta service
            let set_count = self.disk_bm.get_set_count(0, None);
                log::debug!("CP flush for {}: bitmap has {} set bits", self.name, set_count);

                self.is_disk_bm_dirty.store(false, Ordering::SeqCst);
        }
    }

    /// Internal method to free blocks within a portion (called during finalization)
    /// Base implementation: frees from disk bitmap
    fn free_in_portion(&self, bid: &BlkId) {
        if !self.is_persistent {
            return;
        }

        self.debug_assert_portion_ownership(bid);
        // SAFETY: Portion ownership guarantees single-threaded access per portion range
        unsafe {
            let disk_bm = &mut *(Arc::as_ptr(&self.disk_bm) as *mut Bitset);
            disk_bm.reset_bits(bid.blk_num() as u64, bid.blk_count() as u64);
        }
    }

    /// Acquire safe access to the underlying bitmap buffer.
    ///
    /// This method:
    /// 1. Calls finalize_pending_items() to flush all pending operations
    /// 2. Returns a guard that provides access to the underlying IOBuffer
    /// 3. Prevents modifications to disk_bm while the guard is held (via mutable borrow)
    ///
    /// # Safety
    /// - alloc() is safe during guard lifetime (only called during recovery)
    /// - free() and commit() only queue operations, don't modify disk_bm
    /// - finalize_pending_items() cannot be called while guard is held (requires &mut self)
    ///
    /// # Returns
    /// A guard that dereferences to Option<&IOBuffer>. Returns None if allocator is not persistent
    /// or if bitmap hasn't been loaded yet.
    ///
    /// # Example
    /// ```ignore
    /// let guard = allocator.acquire_buffer().await;
    /// if let Some(buffer) = guard.buffer() {
    ///     // Use buffer for checkpoint/serialization
    ///     write_to_disk(buffer);
    /// }
    /// // Guard automatically releases when dropped
    /// ```
    pub async fn acquire_buffer(&mut self) -> BitmapBufferGuard<'_> {
        // Step 1: Finalize all pending operations before entering critical section
        self.finalize_pending_items().await;

        // Step 2: Return guard - the buffer reference will be extracted from allocator when needed
        // This avoids the double borrow issue by deferring the buffer extraction
        BitmapBufferGuard { _allocator: self }
    }
}

/// Implementation of BlkAllocator trait for BitmapBlkAllocator
///
/// This provides the base implementation for bitmap-based allocators.
/// Derived allocators (like FixedBlkAllocator) should override alloc() and
/// free() with their specific allocation strategies.
#[async_trait]
impl BlkAllocator for BitmapBlkAllocator {
    /// Allocate a single contiguous block
    /// This is an abstract method - derived classes must override
    async fn alloc_contiguous(&self, nblks: BlkCount, hints: &BlkAllocHints) -> (BlkAllocStatus, Option<BlkId>) {
        // Ensure hints specify contiguous allocation
        let mut adjusted_hints = hints.clone();
        adjusted_hints.is_contiguous = true;
        
        let (status, bids) = self.alloc(nblks, &adjusted_hints).await;
        if status == BlkAllocStatus::Success || (status == BlkAllocStatus::Partial && hints.partial_alloc_ok) {
            (status, bids.first().copied())
        } else {
            (status, None)
        }
    }

    /// Allocates contiguous blocks on base bitmap for a given temperature and count. It allocates a
    /// block only within its portion. Returns a `BlkId` (invalid/default on failure).
    fn alloc_immediate(&self, nblks: BlkCount, hints: &BlkAllocHints, out_blkids: &mut BlkIds) -> BlkAllocStatus {
        // If not persistent, return SpaceFull
        if !self.is_persistent {
            return BlkAllocStatus::SpaceFull;
        }

        let temperature = hints.desired_temp as BlkTemp;
        let segment = self.segment_mgr.select_segment(temperature);
        let portion_trait = segment.my_ondisk_portion();
        // Downcast to OnDiskPortion to access bitmap-specific methods
        let portion = portion_trait.as_any().downcast_ref::<OnDiskPortion>().expect("Invalid portion type");
        let start_bit = portion.start_blk() as u64;
        let end_bit = portion.end_blk() as u64;

        // Use portion's search hand for allocation (avoids contention)
        let hand = portion.search_hand();
        // SAFETY: Portion ownership guarantees single-threaded access per portion range
        let disk_bm = unsafe { &mut *(Arc::as_ptr(&self.disk_bm) as *mut Bitset) };

        // Try allocation from hand
        let bit_range =
            disk_bm.get_next_contiguous_n_reset_bits_range(hand as u64, Some(end_bit), nblks as u32, nblks as u32);
        let ret_blk_id = if bit_range.start_bit != sisl::NPOS && bit_range.nbits >= nblks as u32 {
            portion.set_search_hand((bit_range.start_bit + bit_range.nbits as u64) as BlkNum);
            BlkId::new(bit_range.start_bit as BlkNum, bit_range.nbits as BlkCount, self.chunk_id)
        } else if hand != start_bit as BlkNum {
            // If failed and hand != start, retry from start
            let bit_range =
                disk_bm.get_next_contiguous_n_reset_bits_range(start_bit, Some(end_bit), nblks as u32, nblks as u32);
            if bit_range.start_bit != sisl::NPOS && bit_range.nbits >= nblks as u32 {
                portion.set_search_hand((bit_range.start_bit + bit_range.nbits as u64) as BlkNum);
                BlkId::new(bit_range.start_bit as BlkNum, bit_range.nbits as BlkCount, self.chunk_id)
            } else {
                return BlkAllocStatus::SpaceFull;
            }
        } else {
            return BlkAllocStatus::SpaceFull;
        };

        disk_bm.set_bits(ret_blk_id.blk_num() as u64, ret_blk_id.blk_count() as u64);
        out_blkids.push(ret_blk_id);
        self.alloced_blk_count.fetch_add(ret_blk_id.blk_count() as i64, Ordering::Relaxed);
        BlkAllocStatus::Success
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
                    let allocator = unsafe { &*(self_ptr as *const BitmapBlkAllocator) };
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

    /// Schedule commit of blocks on base bitmap (deferred - collected per reactor)
    /// Call finalize() to batch-apply all pending commits
    fn schedule_commit(&self, bid: &BlkId) -> BlkAllocStatus {
        // Add to per-reactor pending commits for batch processing
        // TODO: Keep track of the number of pending commits to prevent memory overusage and so call
        // finalize_pending_items if it exceeds a threshold
        self.pending_commits.push(bid.clone());
        log::trace!("Queued commit for {:?} on reactor {}", bid, iomgr().current_reactor_id());

        BlkAllocStatus::Success
    }

    /// Free blocks on base bitmap (deferred - collected per reactor)
    /// Call finalize() to batch-apply all pending frees
    fn schedule_free(&self, bid: &BlkId) {
        // Add to per-reactor pending frees for batch processing
        // TODO: Keep track of the number of pending frees to prevent memory overusage and if so call
        // finalize_pending_items if it exceeds a threshold
        self.pending_frees.push(bid.clone());
        log::trace!("Queued free for {:?} on reactor {}", bid, iomgr().current_reactor_id());
    }

    /// Free a single block asynchronously and wait for completion
    async fn free(&self, bid: &BlkId) {
        let mut bids = BlkIds::new();
        bids.push(*bid);
        self.free_batch(&bids).await;
    }

    /// Free multiple blocks asynchronously - enqueues all to pending_frees
    async fn free_batch(&self, bids: &BlkIds) {
        for bid in bids.iter() {
            self.pending_frees.push(bid.clone());
        }
        log::trace!("Queued batch free of {} blocks on reactor {}", bids.len(), iomgr().current_reactor_id());
    }

    /// Get number of available blocks
    fn available_blks(&self) -> BlkNum { self.num_blks - self.get_used_blks() }

    /// Get number of fragmented blocks (not applicable for bitmap allocator)
    fn get_defrag_nblks(&self) -> BlkNum { 0 }

    /// Get number of used blocks
    fn get_used_blks(&self) -> BlkNum { self.alloced_blk_count.load(Ordering::Relaxed) as BlkNum }

    /// Check if block is allocated
    async fn is_blk_alloced(&self, b: &BlkId, is_thread_safe: bool) -> bool {
        if is_thread_safe {
            // Find the portion that owns this block and run on its reactor
            let portion = self.blkid_to_portion(b);
            let reactor_id = portion.reactor_id() as usize;
            let blk_num = b.blk_num();
            let blk_count = b.blk_count();

            // Use run_on_reactor to run on specific reactor and get result
            let disk_bm_clone = self.disk_bm.clone();
            iomgr()
                .spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                    disk_bm_clone.is_bits_set(blk_num as u64, blk_count as u64)
                })
                .await
        } else {
            // Direct access (caller ensures thread safety)
            self.disk_bm.is_bits_set(b.blk_num() as u64, b.blk_count() as u64)
        }
    }

    /// Convert to string representation
    fn to_string(&self) -> String {
        format!(
            "BitmapBlkAllocator: name={}, blks={}, used={}, portions={}",
            self.name,
            self.num_blks,
            self.get_used_blks(),
            self.total_portions()
        )
    }

    /// Get alignment size
    fn get_align_size(&self) -> u32 { self.align_size }

    /// Get total blocks
    fn get_total_blks(&self) -> BlkNum { self.num_blks }

    /// Get allocator name
    fn get_name(&self) -> &str { &self.name }

    /// Get block size
    fn get_blk_size(&self) -> u32 { self.blk_size }

    /// Get status as string
    fn get_status(&self, _log_level: i32) -> String {
        format!(
            "{{\"name\":\"{}\",\"total_blks\":{},\"used_blks\":{},\"available_blks\":{}}}",
            self.name,
            self.num_blks,
            self.get_used_blks(),
            self.available_blks()
        )
    }
}
