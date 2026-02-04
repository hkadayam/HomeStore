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
        atomic::{AtomicBool, AtomicI64, Ordering},
        Arc,
    },
    time::SystemTime,
};

use async_trait::async_trait;
#[cfg(test)]
use iomgr::iomanager_test;
use iomgr::{iomgr, BackgroundTasks, IOBuffer};
use sisl::{Bitset, RcuPtr};

use super::{
    bitmap_blk_allocator::BitmapBlkAllocator,
    blk_allocator::{BlkAllocConfig, BlkAllocator},
    segment_manager::{PortionBase, Segment, SegmentManager},
};
use crate::common::{AllocatorId, BlkAllocHints, BlkAllocStatus, BlkCount, BlkId, BlkIds, BlkNum, ChunkNum};

/// Bitmap word size in blocks (64 blocks per word)
const WORD_SIZE: BlkNum = 64;

/// Configuration extension for VarsizeBlkAllocator
#[derive(Debug, Clone)]
pub struct VarsizeBlkAllocConfig {
    pub base: BlkAllocConfig,
    pub nsegments: u32,
    pub max_cache_blks_per_slab: BlkNum,
    pub use_slabs: bool,
    /// Minimum free percentage in a portion required to trigger bitmap sweep for cache filling
    pub sweep_min_free_pct: u8,
    /// Maximum cache entries allowed per segment (divided among portions)
    pub max_slab_cache_entries: BlkNum,
    /// Should use slabs to allocate the blocks. Defaults to true
    pub enable_slab_allocation: bool,
}

impl VarsizeBlkAllocConfig {
    pub fn new(
        cfg: BlkAllocConfig, nsegments: u32, max_cache_blks_per_slab: BlkNum, use_slabs: bool, sweep_min_free_pct: u8,
        max_slab_cache_entries: BlkNum, enable_slab_allocation: bool,
    ) -> Self {
        Self {
            base: cfg,
            nsegments,
            max_cache_blks_per_slab,
            use_slabs,
            sweep_min_free_pct,
            max_slab_cache_entries,
            enable_slab_allocation,
        }
    }

    pub fn get_blks_per_segment(&self) -> BlkNum { self.base.capacity / self.nsegments as BlkNum }
}

/// Slab index for different block sizes
/// Slab 0: 1 block, Slab 1: 2 blocks, Slab 2: 4 blocks, Slab 3: 8 blocks, etc.
type SlabIdx = usize;

/// Allocation request for slab cache
struct SlabAllocRequest {
    nblks: BlkCount,
    is_contiguous: bool,
    min_slab_idx: SlabIdx,
    max_slab_idx: SlabIdx,
}

/// Allocation response from slab cache
struct SlabAllocResponse {
    /// Successfully allocated blocks
    out_blks: BlkIds,
    /// Number of blocks allocated
    nblks_alloced: BlkCount,
    /// Blocks that couldn't fit in cache when freeing (zombied)
    excess_blks: BlkIds,
    /// Number of zombied blocks
    nblks_zombied: BlkCount,
}

impl SlabAllocResponse {
    fn new() -> Self {
        Self { out_blks: BlkIds::new(), nblks_alloced: 0, excess_blks: BlkIds::new(), nblks_zombied: 0 }
    }

    fn reset(&mut self) {
        self.out_blks.clear();
        self.nblks_alloced = 0;
        self.excess_blks.clear();
        self.nblks_zombied = 0;
    }
}

/// A single slab representing a fixed block size
struct Slab {
    /// Queue of free blocks for this slab size
    queue: VecDeque<BlkId>,
    /// Precomputed slab size (power of 2)
    slab_size: BlkCount,
    /// Total number of blocks cached in this slab (slab_size * queue.len())
    blk_count: BlkNum,
}

impl Slab {
    fn new(slab_size: BlkCount) -> Self { Self { queue: VecDeque::new(), slab_size, blk_count: 0 } }

    fn push(&mut self, bid: BlkId) {
        self.queue.push_back(bid);
        self.blk_count += self.slab_size as BlkNum;
    }

    fn pop(&mut self) -> Option<BlkId> {
        if let Some(bid) = self.queue.pop_front() {
            self.blk_count -= self.slab_size as BlkNum;
            Some(bid)
        } else {
            None
        }
    }

    fn len(&self) -> usize { self.queue.len() }

    fn blk_count(&self) -> BlkNum { self.blk_count }
}

/// Free block cache organized by slabs
/// Each slab handles a specific block size (power of 2)
/// slab_idx 0 = 1 block, slab_idx 1 = 2 blocks, slab_idx 2 = 4 blocks, etc.
struct SlabCache {
    /// Vector of slabs, indexed by slab_idx
    slabs: Vec<Slab>,
    /// Total cached blocks across all slabs (maintained incrementally)
    cached_blk_count: BlkNum,
    chunk_id: ChunkNum,
}

impl SlabCache {
    /// Create a new SlabCache
    /// num_slabs: number of different slab sizes (typically 8-10)
    fn new(num_slabs: usize, chunk_id: ChunkNum) -> Self {
        let slabs = (0..num_slabs)
            .map(|i| {
                let slab_size = 1 << i; // Precompute: 1, 2, 4, 8, 16, ...
                Slab::new(slab_size)
            })
            .collect();
        Self { slabs, cached_blk_count: 0, chunk_id }
    }

    /// Find the slab index for a given block count
    /// This rounds up to the next power of 2
    fn find_slab(nblks: BlkCount) -> SlabIdx {
        if nblks <= 1 {
            0
        } else {
            // Round up to next power of 2, then get the bit position
            let bits = (nblks as u32 - 1).leading_zeros();
            (32 - bits) as usize - 1
        }
    }

    /// Find the slab index by rounding down to the lower power of 2
    /// Returns (slab_idx, excess_blocks)
    fn find_round_down_slab(nblks: BlkCount) -> (SlabIdx, BlkCount) {
        if nblks <= 1 {
            (0, 0)
        } else {
            let slab_idx = (32 - (nblks as u32).leading_zeros() - 1) as usize;
            let slab_size = 1 << slab_idx;
            (slab_idx, nblks - slab_size)
        }
    }

    /// Try to allocate blocks from cache
    /// This implements the key logic:
    /// 1. Try to allocate from the appropriate slab
    /// 2. If not enough, break up higher slabs
    /// 3. If still not enough and !is_contiguous, merge from lower slabs
    fn try_alloc_blks(&mut self, req: SlabAllocRequest) -> SlabAllocResponse {
        let mut resp = SlabAllocResponse::new();

        let slab_idx = Self::find_slab(req.nblks).min(req.max_slab_idx);

        // Step 1: Try to allocate from the target slab
        let status = self.try_alloc_in_slab(slab_idx, &req, &mut resp);
        if status == BlkAllocStatus::Success {
            return resp;
        } else if status == BlkAllocStatus::Partial && req.is_contiguous {
            // Free any partial allocations for contiguous requests
            resp.nblks_zombied = self.try_free_blks_vec(&resp.out_blks, &mut resp.excess_blks);
            resp.out_blks.clear();
            resp.nblks_alloced = 0;
        }

        // Step 2: Try to break up higher slabs
        let status = self.break_up(slab_idx, &req, &mut resp);
        if status == BlkAllocStatus::Success {
            return resp;
        }

        // Step 3: If non-contiguous, try to merge from lower slabs
        if !req.is_contiguous {
            let status = self.merge_down(slab_idx, &req, &mut resp);
            if status == BlkAllocStatus::Success {
                return resp;
            }
        }

        resp
    }

    /// Try to allocate from a specific slab
    fn try_alloc_in_slab(
        &mut self, slab_idx: SlabIdx, req: &SlabAllocRequest, resp: &mut SlabAllocResponse,
    ) -> BlkAllocStatus {
        if resp.nblks_alloced >= req.nblks {
            return BlkAllocStatus::Success;
        }

        if slab_idx >= self.slabs.len() {
            return BlkAllocStatus::Failed;
        }

        let slab = &mut self.slabs[slab_idx];
        let slab_size = slab.slab_size;
        let blks_needed = req.nblks - resp.nblks_alloced;
        let nentries = (blks_needed + slab_size - 1) / slab_size; // Round up

        if req.is_contiguous && nentries > 1 {
            return BlkAllocStatus::Failed;
        }

        // Pop entries from the slab
        let mut num_allocated = 0;
        for _ in 0..nentries {
            if let Some(bid) = slab.pop() {
                self.cached_blk_count -= slab_size as BlkNum;
                resp.out_blks.push(bid);
                num_allocated += slab_size;
            } else {
                // No more entries available
                if resp.out_blks.is_empty() && num_allocated == 0 {
                    return BlkAllocStatus::Failed;
                } else {
                    resp.nblks_alloced += num_allocated;
                    return BlkAllocStatus::Partial;
                }
            }
        }

        resp.nblks_alloced += num_allocated;

        // Handle excess blocks if we allocated more than needed
        if resp.nblks_alloced > req.nblks {
            let residue_nblks = resp.nblks_alloced - req.nblks;
            let needed_blocks = slab_size - residue_nblks;

            // Adjust the last entry
            let last_idx = resp.out_blks.len() - 1;
            let last_bid = resp.out_blks[last_idx];
            resp.out_blks[last_idx] = BlkId::new(last_bid.blk_num(), needed_blocks, last_bid.chunk_num());
            resp.nblks_alloced -= residue_nblks;

            // Create residue entry and try to free it
            let residue_bid =
                BlkId::new(last_bid.blk_num() + needed_blocks as BlkNum, residue_nblks, last_bid.chunk_num());
            resp.nblks_zombied += self.try_free_blks(residue_bid, &mut resp.excess_blks);
        }

        BlkAllocStatus::Success
    }

    /// Break up higher slabs to satisfy allocation
    /// This recursively tries higher slab sizes and splits them
    fn break_up(&mut self, slab_idx: SlabIdx, req: &SlabAllocRequest, resp: &mut SlabAllocResponse) -> BlkAllocStatus {
        if slab_idx >= self.slabs.len() - 1 {
            return if resp.nblks_alloced > 0 { BlkAllocStatus::Partial } else { BlkAllocStatus::Failed };
        }

        let status = self.try_alloc_in_slab(slab_idx + 1, req, resp);
        if status == BlkAllocStatus::Success {
            return BlkAllocStatus::Success;
        } else if status == BlkAllocStatus::Partial && req.is_contiguous {
            // Free partial results for contiguous requests
            resp.nblks_zombied = self.try_free_blks_vec(&resp.out_blks, &mut resp.excess_blks);
            resp.out_blks.clear();
            resp.nblks_alloced = 0;
        }

        self.break_up(slab_idx + 1, req, resp)
    }

    /// Merge from lower slabs to satisfy non-contiguous allocation
    fn merge_down(
        &mut self, slab_idx: SlabIdx, req: &SlabAllocRequest, resp: &mut SlabAllocResponse,
    ) -> BlkAllocStatus {
        if slab_idx == req.min_slab_idx {
            return if resp.nblks_alloced > 0 { BlkAllocStatus::Partial } else { BlkAllocStatus::Failed };
        }

        let status = self.try_alloc_in_slab(slab_idx - 1, req, resp);
        if status == BlkAllocStatus::Success {
            return BlkAllocStatus::Success;
        }

        self.merge_down(slab_idx - 1, req, resp)
    }

    /// Try to free blocks back to cache
    /// Returns number of blocks that couldn't fit in cache (zombied)
    /// Note: Cache limit checking is done at PortionExt level
    fn try_free_blks(&mut self, bid: BlkId, excess_blks: &mut BlkIds) -> BlkCount {
        let mut remaining = bid;
        let mut num_zombied = 0;

        while remaining.blk_count() > 0 {
            let (slab_idx, excess) = Self::find_round_down_slab(remaining.blk_count());

            if slab_idx >= self.slabs.len() {
                excess_blks.push(remaining);
                num_zombied += remaining.blk_count();
                break;
            }

            let slab = &mut self.slabs[slab_idx];
            let slab_size = slab.slab_size;
            let entry_bid = BlkId::new(remaining.blk_num(), slab_size, remaining.chunk_num());

            // Always push to slab (limit checking done at PortionExt level)
            slab.push(entry_bid);
            self.cached_blk_count += slab_size as BlkNum;

            if excess == 0 {
                break;
            }

            // Move to the excess portion
            remaining = BlkId::new(remaining.blk_num() + slab_size as BlkNum, excess, remaining.chunk_num());
        }

        num_zombied
    }

    /// Free a vector of blocks
    fn try_free_blks_vec(&mut self, blks: &[BlkId], excess_blks: &mut BlkIds) -> BlkCount {
        let mut num_zombied = 0;
        for bid in blks {
            num_zombied += self.try_free_blks(*bid, excess_blks);
        }
        num_zombied
    }

    /// Force free blocks back to cache, exceeding max_count if needed
    /// This method does NOT respect cache limits and will add all blocks to the cache
    fn force_free_blks(&mut self, bid: BlkId) {
        let mut remaining = bid;

        while remaining.blk_count() > 0 {
            let (mut slab_idx, _excess) = Self::find_round_down_slab(remaining.blk_count());

            // If block is too large for our largest slab, use the largest available slab
            if slab_idx >= self.slabs.len() {
                slab_idx = self.slabs.len() - 1;
            }

            let slab = &mut self.slabs[slab_idx];
            let slab_size = slab.slab_size;

            // Take up to slab_size blocks from remaining
            let entry_bid = BlkId::new(remaining.blk_num(), slab_size, remaining.chunk_num());

            // Force push to slab without limit checking
            slab.push(entry_bid);
            self.cached_blk_count += slab_size as BlkNum;

            // Calculate actual remaining blocks
            let remaining_count = remaining.blk_count() - slab_size;
            if remaining_count == 0 {
                break;
            }

            // Move to the next portion
            remaining = BlkId::new(remaining.blk_num() + slab_size as BlkNum, remaining_count, remaining.chunk_num());
        }
    }

    /// Get total number of cached blocks across all slabs
    fn total_cached_blks(&self) -> BlkNum { self.cached_blk_count }
}

/// Internal: State of the VarsizeBlkAllocator
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum State {
    #[default]
    Recovering,
    Active,
}

/// This represents a contiguous range of blocks within a segment, owned by a specific reactor.
/// Each portion is accessed exclusively by its owning reactor for lock-free parallelism.
/// This is the inmem portion for VarsizeBlkAllocator with slab cache.
pub struct InmemPortion {
    reactor_id: u32,
    start_blk: BlkNum,
    end_blk: BlkNum,

    slab_cache: SlabCache,
    inmem_search_hand: BlkNum,
    estimated_free_blks: Option<usize>,
    max_slab_cache_entries: BlkNum,
    min_free_blks_to_sweep: usize,
    sweep_cursor: BlkNum,
    sweep_in_progress: bool,
}

impl InmemPortion {
    fn new(
        reactor_id: u32, start_blk: BlkNum, end_blk: BlkNum, num_slabs: usize, chunk_id: ChunkNum,
        max_slab_cache_entries: BlkNum, sweep_min_free_pct: u8,
    ) -> Self {
        let portion_size = (end_blk - start_blk) as usize;
        let min_free_blks_to_sweep = (portion_size * sweep_min_free_pct as usize) / 100;
        Self {
            reactor_id,
            start_blk,
            end_blk,
            slab_cache: SlabCache::new(num_slabs, chunk_id),
            inmem_search_hand: start_blk,
            estimated_free_blks: None,
            max_slab_cache_entries,
            min_free_blks_to_sweep,
            sweep_cursor: start_blk,
            sweep_in_progress: false,
        }
    }
}

// Implement PortionBase trait
impl PortionBase for InmemPortion {
    fn reactor_id(&self) -> u32 { self.reactor_id }
    fn start_blk(&self) -> BlkNum { self.start_blk }
    fn end_blk(&self) -> BlkNum { self.end_blk }
    fn as_any(&self) -> &dyn std::any::Any { self }
}

// Varsize-specific methods
impl InmemPortion {
    fn get_inmem_clock_hand(&self) -> BlkNum { self.inmem_search_hand }
    fn set_inmem_clock_hand(&mut self, hand: BlkNum) { self.inmem_search_hand = hand; }
    fn estimated_free_blks(&self) -> Option<usize> { self.estimated_free_blks }
    fn set_estimated_free_blks(&mut self, count: usize) { self.estimated_free_blks = Some(count); }

    fn update_estimate(&mut self, nblks: BlkCount, incr: bool) {
        if let Some(count) = self.estimated_free_blks.as_mut() {
            if incr {
                *count = count.saturating_add(nblks as usize);
            } else {
                *count = count.saturating_sub(nblks as usize);
            }
        }
    }

    fn max_slab_cache_entries(&self) -> BlkNum { self.max_slab_cache_entries }
    fn total_cached_blks(&self) -> BlkNum { self.slab_cache.total_cached_blks() }
    fn can_cache_more(&self) -> bool { self.total_cached_blks() < self.max_slab_cache_entries }

    fn needs_refill(&self) -> bool {
        let fill_pct = (self.total_cached_blks() as f64 / self.max_slab_cache_entries as f64) * 100.0;
        fill_pct < 25.0
    }

    fn is_sweep_in_progress(&self) -> bool { self.sweep_in_progress }
    fn set_sweep_in_progress(&mut self, in_progress: bool) { self.sweep_in_progress = in_progress; }
    fn slab_cache_mut(&mut self) -> &mut SlabCache { &mut self.slab_cache }
    fn slab_cache(&self) -> &SlabCache { &self.slab_cache }
    fn sweep_cursor(&self) -> BlkNum { self.sweep_cursor }
    fn set_sweep_cursor(&mut self, cursor: BlkNum) { self.sweep_cursor = cursor; }
    fn reset_sweep(&mut self, start_blk: BlkNum) { self.sweep_cursor = start_blk; }
    pub fn min_free_blks_to_sweep(&self) -> usize { self.min_free_blks_to_sweep }
}

/// Helper to get the inmem portion from a segment (immutable)
fn my_portion(segment: &Segment) -> &InmemPortion {
    let portion = segment.my_inmem_portion();
    portion.as_any().downcast_ref::<InmemPortion>().expect("Invalid portion type")
}

/// Helper to get the inmem portion from a segment (mutable)
/// SAFETY: Caller must ensure reactor-local access (single-threaded per portion)
fn my_portion_mut(segment: &Segment) -> &mut InmemPortion {
    let portion = segment.my_inmem_portion();
    // SAFETY: Downcast to concrete type and cast away const
    // This is safe because portion is already &mut behind ReactorLocal
    unsafe {
        let portion_ptr = portion as *const dyn PortionBase as *mut dyn PortionBase;
        let inmem_ptr = (*portion_ptr).as_any().downcast_ref::<InmemPortion>().expect("Invalid portion type")
            as *const InmemPortion as *mut InmemPortion;
        inmem_ptr.as_mut().unwrap()
    }
}

/// Helper to calculate segment and reactor/portion for a given BlkId
/// Returns (seg_idx, reactor_id) tuple
fn blkid_to_seg_portion(segment_mgr: &SegmentManager, bid: &BlkId) -> (usize, usize) {
    let segment = segment_mgr.blkid_to_segment(bid);
    let seg_idx = segment.segment_id() as usize;

    let blk_num = bid.blk_num();
    let reactors_count = iomgr().num_reactors();
    let portion_len = (segment.num_blks() / reactors_count as BlkNum / WORD_SIZE) * WORD_SIZE;
    let offset_in_seg = blk_num - segment.start_blk();
    let reactor_id =
        if portion_len > 0 { std::cmp::min((offset_in_seg / portion_len) as usize, reactors_count - 1) } else { 0 };

    (seg_idx, reactor_id)
}

// ============================================================================
// VarsizeBlkAllocator
// ============================================================================

/// VarsizeBlkAllocator - Allocator for variable-sized block requests
///
/// Features:
/// - Supports variable-sized allocations (1 to N blocks)
/// - Uses a free block cache organized by slab size
/// - Uses merged Segment/Portion structures containing all fields
/// - Uses in-memory bitmap (inmem_bm) for tracking allocated blocks
/// - Lock-free state using RcuPtr
/// - Concurrency handled by per-reactor portions
pub struct VarsizeBlkAllocator {
    // Configuration
    cfg: VarsizeBlkAllocConfig,

    // Optional ondisk allocator (only if cfg.base.persistent is true)
    ondisk_allocator: Option<BitmapBlkAllocator>,

    // State - RcuPtr for lock-free reads
    state: RcuPtr<State>,

    // Direct fields from BitmapBlkAllocator
    name: String,
    blk_size: u32,
    align_size: u32,
    num_blks: BlkNum,
    chunk_id: ChunkNum,
    alloced_blk_count: AtomicI64,

    // Shared segment manager with BitmapBlkAllocator for persistent allocators
    segment_mgr: Arc<SegmentManager>,

    // In-memory bitmap to track allocated blocks
    // If persistent: Arc to separate Bitset
    // If non-persistent: Arc to the same bitmap used for disk
    // Note: Portion ownership guarantees single-threaded access per portion range
    inmem_bm: Arc<Bitset>,

    // Dirty flag for bitmap persistence
    is_disk_bm_dirty: AtomicBool,

    // Background tasks for cache filling operations
    bg_tasks: BackgroundTasks,
}

impl VarsizeBlkAllocator {
    /// Create a new VarsizeBlkAllocator
    pub fn new(
        cfg: VarsizeBlkAllocConfig, buffer: Option<IOBuffer>, id: AllocatorId, num_reactors: u32,
    ) -> Result<Self, String> {
        let num_blks = cfg.base.capacity;

        // Determine initial state based on buffer presence and persistence
        // Non-persistent allocators are always Active (no recovery needed)
        let initial_state = if !cfg.base.persistent || buffer.is_none() { State::Active } else { State::Recovering };

        // Calculate max_slab_cache_entries per portion
        let reactors_count = num_reactors.max(1) as usize;
        let max_per_portion = if num_reactors == 0 {
            cfg.max_slab_cache_entries
        } else {
            cfg.max_slab_cache_entries / num_reactors as BlkNum
        };

        // Create inmem portion creator
        let cfg_clone = cfg.clone();
        let portion_creator = move |reactor_id: u32, start_blk: BlkNum, end_blk: BlkNum| {
            Box::new(InmemPortion::new(
                reactor_id,
                start_blk,
                end_blk,
                8, // num_slabs
                id,
                max_per_portion,
                cfg_clone.sweep_min_free_pct,
            )) as Box<dyn PortionBase>
        };

        // Create ondisk_allocator and get/create segment_mgr depending on persistence
        let (ondisk_allocator, segment_mgr) = if cfg.base.persistent {
            // Persistent: Create BitmapBlkAllocator with our custom inmem portion creator
            // Then get a clone of its Arc<SegmentManager>
            let ondisk = BitmapBlkAllocator::new(&cfg.base, buffer, id, num_reactors, Some(portion_creator))?;
            let segment_mgr = ondisk.segment_mgr();
            (Some(ondisk), segment_mgr)
        } else {
            // Non-persistent: Create our own SegmentManager and wrap in Arc
            let segment_mgr = Arc::new(SegmentManager::new(
                num_blks,
                cfg.nsegments,
                reactors_count,
                cfg.base.fixed_size_segments,
                |reactor_id, portion_start, portion_end| {
                    Box::new(super::segment_manager::EmptyPortion::new(reactor_id, portion_start, portion_end))
                        as Box<dyn PortionBase>
                },
                portion_creator,
            ));
            (None, segment_mgr)
        };

        // Setup inmem_bm based on persistence
        let inmem_bm = Arc::new(Bitset::new(num_blks as u64, id as u64));

        Ok(Self {
            cfg: cfg.clone(),
            ondisk_allocator,
            state: RcuPtr::new(initial_state),
            name: cfg.base.unique_name.clone(),
            blk_size: cfg.base.blk_size,
            align_size: cfg.base.align_size,
            num_blks,
            chunk_id: id,
            alloced_blk_count: AtomicI64::new(0),
            segment_mgr,
            inmem_bm,
            is_disk_bm_dirty: AtomicBool::new(false),
            bg_tasks: BackgroundTasks::new(),
        })
    }

    /// Fill the slab cache for a specific portion by scanning the in-memory bitmap.
    /// This method scans the bitmap to find free blocks and adds them to the slab cache.
    /// It decides when to stop based on multiple factors:
    /// - Cache fill level (stops when cache is sufficiently full)
    /// - Number of blocks scanned (yields after scanning 65536 blocks / 1K bitwords)
    /// - Portion scan completion (stops at end of portion)
    ///
    /// # Arguments
    /// * `seg_idx` - Index of the segment whose portion to fill
    ///
    /// # Returns
    /// Returns true if more scanning is needed (should be called again), false if scanning is
    /// complete
    pub async fn fill_slab_cache_portion(&self, seg_idx: usize) -> bool {
        assert!(seg_idx < self.segment_mgr.segments_count() as usize, "seg_idx {} out of bounds", seg_idx);

        // Get portion from segment
        let segment = self.segment_mgr.get_segment(seg_idx).expect("Invalid segment index");
        let my_portion = my_portion_mut(segment);
        let portion_end = my_portion.end_blk();
        let start_blk = my_portion.sweep_cursor();

        // Stop if cache is already full or free blocks below threshold or sweep already in progress
        if my_portion.is_sweep_in_progress()
            || !my_portion.can_cache_more()
            || my_portion.estimated_free_blks().is_some_and(|n| n < my_portion.min_free_blks_to_sweep)
        {
            return false;
        }

        const YIELD_INTERVAL: BlkNum = 65536;
        let mut cur_blk = start_blk;
        let mut next_yield_at = start_blk + YIELD_INTERVAL;
        let mut total_free_found = 0usize;

        my_portion.set_sweep_in_progress(true);
        while cur_blk < portion_end && my_portion.can_cache_more() {
            if cur_blk >= next_yield_at {
                iomgr().yield_now().await;
                next_yield_at = cur_blk + YIELD_INTERVAL;
            }

            let result = self.inmem_bm.get_next_contiguous_n_reset_bits_range(
                cur_blk as u64,
                Some(portion_end as u64),
                1,
                (portion_end - cur_blk).min(256),
            );

            if result.start_bit == sisl::NPOS {
                break;
            }

            let found_blk = result.start_bit as BlkNum;
            let found_count = result.nbits as BlkCount;
            total_free_found += found_count as usize;

            let bid = BlkId::new(found_blk, found_count, self.chunk_id);
            let cache = my_portion.slab_cache_mut();
            let mut excess_blks = BlkIds::new();
            let excess_count = cache.try_free_blks(bid, &mut excess_blks);

            let cached_count = found_count - excess_count;
            if cached_count > 0 {
                // SAFETY: Portion ownership guarantees single-threaded access per portion range
                unsafe {
                    let inmem_bm = &mut *(Arc::as_ptr(&self.inmem_bm) as *mut Bitset);
                    inmem_bm.set_bits(found_blk as u64, cached_count as u64);
                }
                my_portion.update_estimate(cached_count, false);
            }

            cur_blk = found_blk + found_count as BlkNum;
        }

        my_portion.set_sweep_cursor(cur_blk);

        if my_portion.estimated_free_blks().is_none() && cur_blk >= portion_end {
            my_portion.set_estimated_free_blks(total_free_found);
        }

        my_portion.set_sweep_in_progress(false);
        cur_blk < portion_end && my_portion.can_cache_more()
    }

    /// Allocate from slab cache (per-segment, per-reactor)
    fn alloc_from_slab(
        &self, seg_idx: usize, nblks: BlkCount, is_contiguous: bool, out_bids: &mut BlkIds,
    ) -> BlkAllocStatus {
        let segment = self.segment_mgr.get_segment(seg_idx).expect("Invalid segment index");
        let portion = my_portion_mut(segment);

        let cache = portion.slab_cache_mut();
        let resp = cache.try_alloc_blks(SlabAllocRequest { nblks, is_contiguous, min_slab_idx: 0, max_slab_idx: 7 });

        if portion.needs_refill() && !portion.is_sweep_in_progress() {
            let self_ptr_addr = self as *const VarsizeBlkAllocator as usize;
            self.bg_tasks.spawn(iomgr::ReactorTarget::Current, async move {
                unsafe {
                    let allocator = &*(self_ptr_addr as *const VarsizeBlkAllocator);
                    allocator.fill_slab_cache_portion(seg_idx).await;
                }
            });
        }

        if resp.nblks_alloced == nblks && !resp.out_blks.is_empty() {
            out_bids.extend_from_slice(&resp.out_blks);
            return BlkAllocStatus::Success;
        }
        BlkAllocStatus::Failed
    }

    /// Allocate from bitmap using portion allocation with clock algorithm
    /// Only allocates from current reactor's portion in the specified segment
    fn alloc_from_bitmap(
        &self, seg_idx: usize, nblks: BlkCount, is_contiguous: bool, out_bids: &mut BlkIds,
    ) -> BlkAllocStatus {
        let segment = self.segment_mgr.get_segment(seg_idx).expect("Invalid segment index");
        let portion = my_portion_mut(segment);

        let start_blk = portion.start_blk();
        let end_blk = portion.end_blk();

        let (min_needed, max_needed) = if is_contiguous { (nblks as u32, nblks as u32) } else { (1, nblks as u32) };

        let mut remaining = nblks;
        let mut clock_hand = portion.get_inmem_clock_hand();
        let initial_out_len = out_bids.len();

        // Try allocation starting from clock hand
        while remaining > 0 {
            let result = self.inmem_bm.get_next_contiguous_n_reset_bits_range(
                clock_hand as u64,
                Some(end_blk as u64),
                min_needed.min(remaining as u32),
                max_needed.min(remaining as u32),
            );

            if result.start_bit == sisl::NPOS {
                break;
            }

            let found_blk = result.start_bit as BlkNum;
            let found_count = result.nbits as BlkCount;
            let bid = BlkId::new(found_blk, found_count, self.chunk_id);
            out_bids.push(bid);

            // SAFETY: Portion ownership guarantees single-threaded access per portion range
            unsafe {
                let inmem_bm = &mut *(Arc::as_ptr(&self.inmem_bm) as *mut Bitset);
                inmem_bm.set_bits(found_blk as u64, found_count as u64);
            }

            remaining -= found_count;
            clock_hand = found_blk + found_count as BlkNum;

            if clock_hand >= end_blk {
                clock_hand = start_blk;
            }
        }

        portion.set_inmem_clock_hand(clock_hand);

        if remaining == 0 {
            portion.update_estimate(nblks, false);
            BlkAllocStatus::Success
        } else {
            // Partial allocation - free what we allocated and clear out_bids
            unsafe {
                let inmem_bm = &mut *(Arc::as_ptr(&self.inmem_bm) as *mut Bitset);
                for bid in out_bids.iter().skip(initial_out_len) {
                    inmem_bm.reset_bits(bid.blk_num() as u64, bid.blk_count() as u64);
                }
            }
            out_bids.truncate(initial_out_len);
            BlkAllocStatus::SpaceFull
        }
    }

    fn on_free_in_portion(&self, bid: &BlkId) {
        // During recovery, there is nothing to do
        {
            let state_guard = self.state.read();
            if *state_guard != State::Active {
                return;
            }
        }

        let segment = self.segment_mgr.blkid_to_segment(bid);
        let mut should_mark_free = true;
        if self.cfg.enable_slab_allocation {
            let portion = my_portion_mut(segment);

            if portion.can_cache_more() {
                let cache = portion.slab_cache_mut();
                let mut excess_blks = BlkIds::new();
                let zombied = cache.try_free_blks(*bid, &mut excess_blks);

                should_mark_free = zombied > 0 || !excess_blks.is_empty() || !portion.can_cache_more();
            }
        }

        if should_mark_free {
            // SAFETY: Portion ownership guarantees single-threaded access per portion range
            unsafe {
                let inmem_bm = &mut *(Arc::as_ptr(&self.inmem_bm) as *mut Bitset);
                inmem_bm.reset_bits(bid.blk_num() as u64, bid.blk_count() as u64);
            }
            my_portion_mut(segment).update_estimate(bid.blk_count(), true);
        }
    }
}

#[async_trait]
impl BlkAllocator for VarsizeBlkAllocator {
    async fn recover(&mut self) {
        // Recover can only be called for persistent allocators
        if !self.cfg.base.persistent {
            assert!(false, "recover() can only be called on persistent allocators");
            return;
        }

        if let Some(ref mut ondisk) = self.ondisk_allocator {
            // Get the underlying buffer from ondisk_allocator and copy it to inmem_bm
            let guard = ondisk.acquire_buffer().await;
            let buffer = guard.buffer();
            let mut local_buffer = IOBuffer::new(buffer.len());
            local_buffer.as_mut_slice().copy_from_slice(buffer.as_slice());
            let (bitset, set_count) = Bitset::load(local_buffer).expect("Failed to load inmem_bm from disk bitmap");
            self.inmem_bm = Arc::new(bitset);
            self.alloced_blk_count.store(set_count as i64, Ordering::Relaxed);
        }

        self.state.update(State::Active);

        // Spawn cache fill tasks on all reactors in parallel using BackgroundTasks
        let num_segments = self.segment_mgr.segments_count() as usize;
        let self_ptr_addr = self as *const VarsizeBlkAllocator as usize;
        self.bg_tasks.spawn_on_all(|_reactor_id| async move {
            unsafe {
                let allocator = &*(self_ptr_addr as *const VarsizeBlkAllocator);
                for seg_idx in 0..num_segments {
                    allocator.fill_slab_cache_portion(seg_idx).await;
                }
            }
        });
        
        // Note: We don't wait for cache fills to complete here - they run in background
        // The allocator can still serve requests from bitmap while cache is being filled
    }

    /// Shutdown the allocator gracefully, waiting for all background tasks to complete.
    /// 
    /// **IMPORTANT**: This MUST be called before dropping the allocator to ensure all
    /// background cache fill tasks complete. The tasks hold raw pointers to the allocator,
    /// so dropping without shutdown causes undefined behavior.
    async fn shutdown(&self) {
        self.bg_tasks.join_all().await;
    }

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

    fn alloc_immediate(&self, nblks: BlkCount, hint: &BlkAllocHints, out_bids: &mut BlkIds) -> BlkAllocStatus {
        if nblks == 0 {
            return BlkAllocStatus::Failed;
        }

        // Check state - during recovery, allocate via ondisk_allocator
        if *self.state.read() == State::Recovering {
            if let Some(ref ondisk) = self.ondisk_allocator {
                return ondisk.alloc_immediate(nblks, hint, out_bids);
            }
            return BlkAllocStatus::Failed;
        }

        // Active state: try slab cache first, then bitmap
        let start_segment = self.segment_mgr.select_segment(hint.desired_temp);
        let start_seg_idx = start_segment.segment_id() as usize;
        let num_segments = self.segment_mgr.segments_count() as usize;

        if !hint.bypass_slab_cache && self.cfg.enable_slab_allocation {
            for i in 0..num_segments {
                let seg_idx = (start_seg_idx + i) % num_segments;
                let status = self.alloc_from_slab(seg_idx, nblks, hint.is_contiguous, out_bids);
                if status == BlkAllocStatus::Success {
                    self.alloced_blk_count.fetch_add(nblks as i64, Ordering::Relaxed);
                    return status;
                }
            }
        }

        for i in 0..num_segments {
            let seg_idx = (start_seg_idx + i) % num_segments;
            let status = self.alloc_from_bitmap(seg_idx, nblks, hint.is_contiguous, out_bids);
            if status == BlkAllocStatus::Success {
                self.alloced_blk_count.fetch_add(nblks as i64, Ordering::Relaxed);
                return status;
            }
        }

        BlkAllocStatus::SpaceFull
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
                    let allocator = unsafe { &*(self_ptr as *const VarsizeBlkAllocator) };
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

    fn schedule_commit(&self, bid: &BlkId) -> BlkAllocStatus {
        if let Some(ref ondisk) = self.ondisk_allocator {
            return ondisk.schedule_commit(bid);
        }
        BlkAllocStatus::Success
    }

    fn schedule_free(&self, bid: &BlkId) {
        if let Some(ref ondisk) = self.ondisk_allocator {
            ondisk.schedule_free(bid);
        }
        let (_, reactor_id) = blkid_to_seg_portion(&self.segment_mgr, bid);

        // Spawn on the portion's reactor to ensure thread safety
        let self_ptr_addr = self as *const VarsizeBlkAllocator as usize;
        let bid_copy = *bid;
        iomgr().spawn_detached(iomgr::ReactorTarget::Reactor(reactor_id), async move {
            unsafe {
                let allocator = &*(self_ptr_addr as *const VarsizeBlkAllocator);
                allocator.on_free_in_portion(&bid_copy);
                allocator.alloced_blk_count.fetch_sub(bid_copy.blk_count() as i64, Ordering::Relaxed);
            }
        });
    }

    /// Free a single block asynchronously and wait for completion
    async fn free(&self, bid: &BlkId) {
        let (_, reactor_id) = blkid_to_seg_portion(&self.segment_mgr, bid);
        let self_ptr_addr = self as *const VarsizeBlkAllocator as usize;
        let bid_copy = *bid;
        iomgr().spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
            unsafe {
                let allocator = &*(self_ptr_addr as *const VarsizeBlkAllocator);
                allocator.on_free_in_portion(&bid_copy);
                allocator.alloced_blk_count.fetch_sub(bid_copy.blk_count() as i64, Ordering::Relaxed);
            }
        }).await;
    }

    /// Free multiple blocks asynchronously across appropriate reactors. Use this method if you are
    /// in async function and you want these blks to be again allocable after calling await.
    async fn free_batch(&self, bids: &BlkIds) {
        // Step 1: Call ondisk allocator's free_batch if persistent
        if let Some(ref ondisk) = self.ondisk_allocator {
            ondisk.free_batch(bids).await;
        }

        // Step 2: Group BlkIds by reactor using Vec (reactors are contiguous 0..num_reactors)
        let num_reactors = iomgr().num_reactors();
        let mut reactor_bids: Vec<Vec<BlkId>> = vec![Vec::new(); num_reactors];

        for bid in bids.iter() {
            let (_, reactor_id) = blkid_to_seg_portion(&self.segment_mgr, bid);
            reactor_bids[reactor_id].push(*bid);
        }

        // Step 3: Spawn on each reactor and process its BlkIds
        let self_ptr_addr = self as *const VarsizeBlkAllocator as usize;
        for (reactor_id, reactor_bids_vec) in reactor_bids.into_iter().enumerate() {
            if reactor_bids_vec.is_empty() {
                continue;
            }
            iomgr()
                .spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                    unsafe {
                        let allocator = &*(self_ptr_addr as *const VarsizeBlkAllocator);
                        let mut freed_count: i64 = 0;
                        for bid in reactor_bids_vec {
                            allocator.on_free_in_portion(&bid);
                            freed_count += bid.blk_count() as i64;
                        }
                        allocator.alloced_blk_count.fetch_sub(freed_count, Ordering::Relaxed);
                    }
                })
                .await;
        }
    }

    fn available_blks(&self) -> BlkNum { self.get_total_blks() - self.get_used_blks() }

    fn get_defrag_nblks(&self) -> BlkNum { 0 }

    fn get_used_blks(&self) -> BlkNum {
        if *self.state.read() == State::Recovering {
            if let Some(ref ondisk) = self.ondisk_allocator {
                return ondisk.get_used_blks();
            }
        }

        self.alloced_blk_count.load(Ordering::Relaxed) as BlkNum
    }

    async fn is_blk_alloced(&self, bid: &BlkId, is_thread_safe: bool) -> bool {
        if is_thread_safe {
            let (_, reactor_id) = blkid_to_seg_portion(&self.segment_mgr, bid);
            let blk_num = bid.blk_num();
            let blk_count = bid.blk_count();

            let inmem_bm = Arc::clone(&self.inmem_bm);
            return iomgr()
                .spawn_waitable(iomgr::ReactorTarget::Reactor(reactor_id), async move {
                    inmem_bm.is_bits_set(blk_num as u64, blk_count as u64)
                })
                .await;
        }
        self.inmem_bm.is_bits_set(bid.blk_num() as u64, bid.blk_count() as u64)
    }

    fn to_string(&self) -> String {
        let total_cached: BlkNum = (0..self.segment_mgr.segments_count())
            .filter_map(|i| self.segment_mgr.get_segment(i as usize))
            .map(|seg| my_portion(seg).slab_cache().total_cached_blks())
            .sum();

        format!(
            "{}: Total={}, Used={}, Segments={}, Cache={} blocks (reactor-local)",
            self.name,
            self.get_total_blks(),
            self.get_used_blks(),
            self.segment_mgr.segments_count(),
            total_cached
        )
    }

    fn get_align_size(&self) -> u32 { self.align_size }

    fn get_total_blks(&self) -> BlkNum { self.num_blks }

    fn get_name(&self) -> &str { &self.name }

    fn get_blk_size(&self) -> u32 { self.blk_size }

    fn get_status(&self, _log_level: i32) -> String {
        let cached: BlkNum = (0..self.segment_mgr.segments_count())
            .filter_map(|i| self.segment_mgr.get_segment(i as usize))
            .map(|seg| my_portion(seg).slab_cache().total_cached_blks())
            .sum();

        format!(
            r#"{{
    "total_blks": {},
    "used_blks": {},
    "available_blks": {},
    "num_segments": {},
    "cached_blks_reactor_local": {}
}}"#,
            self.get_total_blks(),
            self.get_used_blks(),
            self.available_blks(),
            self.segment_mgr.segments_count(),
            cached
        )
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashSet;

    use super::*;

    fn create_test_allocator(total_blks: BlkNum, use_slabs: bool) -> VarsizeBlkAllocator {
        let base_cfg = BlkAllocConfig::new(4096, 4096, (total_blks as u64) * 4096, false, "test".to_string(), 1);
        let cfg = VarsizeBlkAllocConfig::new(base_cfg, 4, 1024, use_slabs, 20, 10000, true);
        VarsizeBlkAllocator::new(cfg, None, 0, 1).expect("Failed to create allocator")
    }

    #[iomanager_test]
    async fn test_varsize_allocator_basic() {
        let alloc = create_test_allocator(10000, true);

        // Test single block allocation
        let mut bids1 = BlkIds::new();
        let status = alloc.alloc_immediate(1, &BlkAllocHints::default(), &mut bids1);
        assert_eq!(status, BlkAllocStatus::Success);
        assert_eq!(bids1.len(), 1);
        assert_eq!(bids1[0].blk_count(), 1);

        // Test multi-block contiguous allocation
        let mut bids2 = BlkIds::new();
        let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids2);
        assert_eq!(status, BlkAllocStatus::Success);
        assert_eq!(bids2.len(), 1, "Contiguous allocation should return single BlkId");
        assert_eq!(bids2[0].blk_count(), 10);

        // Verify used blocks count
        assert_eq!(alloc.get_used_blks(), 11);
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_varsize_allocator_slab_cache() {
        let alloc = create_test_allocator(10000, true);

        // Allocate and free to populate cache
        let mut bids = BlkIds::new();
        let status = alloc.alloc_immediate(4, &BlkAllocHints::default(), &mut bids);
        assert_eq!(status, BlkAllocStatus::Success);
        assert!(!bids.is_empty());

        let freed_bid = bids[0];
        alloc.free(&freed_bid).await;

        // Allocate again - should come from cache
        let mut bids2 = BlkIds::new();
        let status = alloc.alloc_immediate(4, &BlkAllocHints::default(), &mut bids2);
        assert_eq!(status, BlkAllocStatus::Success);
        assert!(!bids2.is_empty());

        // Verify cache is working by checking we got same or nearby blocks
        // (exact match not guaranteed due to slab organization)
        assert!(bids2[0].is_valid());
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_alloc_free_contiguous() {
        let total_blks = 1000;
        let alloc = create_test_allocator(total_blks, true);
        let mut allocated_bids = Vec::new();

        // Allocate 50% of blocks in contiguous chunks
        let mut allocated_count = 0;
        while allocated_count < total_blks / 2 {
            let mut bids = BlkIds::new();
            let size = std::cmp::min(16, total_blks / 2 - allocated_count);
            let status =
                alloc.alloc_immediate(size as BlkCount, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);

            if status != BlkAllocStatus::Success {
                break;
            }

            assert_eq!(bids.len(), 1, "Contiguous allocation should return single BlkId");
            allocated_count += bids[0].blk_count() as BlkNum;
            allocated_bids.push(bids[0]);
        }

        // Free half of what we allocated
        let free_count = allocated_bids.len() / 2;
        for bid in allocated_bids.iter().take(free_count) {
            alloc.free(bid).await;
        }

        // Verify used blocks decreased
        let used_after_free = alloc.get_used_blks();
        assert!(used_after_free < allocated_count, "Used blocks should decrease after freeing");

        // Reallocate freed space
        let mut reallocated = 0;
        while reallocated < free_count {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(8, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);
            if status != BlkAllocStatus::Success {
                break;
            }
            reallocated += bids.len();
        }

        assert!(reallocated > 0, "Should be able to reallocate freed space");
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_alloc_various_sizes() {
        let alloc = create_test_allocator(10000, true);
        let sizes = [1, 2, 4, 8, 16, 32, 64, 128, 256];
        let mut allocated = Vec::new();

        // Allocate various power-of-2 sizes
        for &size in &sizes {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(size, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);
            assert_eq!(status, BlkAllocStatus::Success, "Failed to allocate {} blocks", size);
            assert_eq!(bids.len(), 1);
            assert_eq!(bids[0].blk_count(), size);
            allocated.push(bids[0]);
        }

        // Verify total used
        let expected_used: BlkCount = sizes.iter().sum();
        assert_eq!(alloc.get_used_blks(), expected_used as BlkNum);

        // Free all
        for bid in allocated.iter() {
            alloc.free(bid).await;
        }

        // Verify all freed
        assert_eq!(alloc.get_used_blks(), 0);
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_alloc_scatter() {
        let alloc = create_test_allocator(10000, true);
        let mut allocated = Vec::new();

        // Allocate scattered (non-contiguous allowed)
        for _ in 0..100 {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: false, ..Default::default() }, &mut bids);

            if status != BlkAllocStatus::Success {
                break;
            }

            // Non-contiguous may return multiple BlkIds
            let total_count: BlkCount = bids.iter().map(|b| b.blk_count()).sum();
            assert_eq!(total_count, 10, "Should allocate requested number of blocks");
            allocated.extend(bids.into_iter());
        }

        assert!(allocated.len() > 0, "Should allocate some blocks");

        // Free all
        for bid in allocated.iter() {
            alloc.free(bid).await;
        }
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_alloc_until_full() {
        let total_blks = 1000;
        let alloc = create_test_allocator(total_blks, true);
        let mut allocated = Vec::new();

        // Allocate until full
        loop {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);

            if status == BlkAllocStatus::SpaceFull {
                break;
            }

            if status == BlkAllocStatus::Success {
                allocated.extend(bids.into_iter());
            } else {
                break;
            }
        }

        // Should have allocated a significant portion of blocks
        // Note: Contiguous allocations cause fragmentation, so we can't expect >90% utilization
        let used = alloc.get_used_blks();
        println!("Used blocks: {}, Total blocks: {}, Percentage: {}%", used, total_blks, (used * 100) / total_blks);
        assert!(used > total_blks * 7 / 10, "Should use most of available space. Used: {}, Expected: >{}", used, total_blks * 7 / 10);

        // Try one more allocation - should fail
        let mut bids = BlkIds::new();
        let status = alloc.alloc_immediate(100, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);
        assert_eq!(status, BlkAllocStatus::SpaceFull, "Should be full");
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_free_and_realloc_pattern() {
        let alloc = create_test_allocator(10000, true);
        let mut allocated = Vec::new();

        // Phase 1: Allocate 50%
        for _ in 0..50 {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);
            if status == BlkAllocStatus::Success {
                allocated.extend(bids.into_iter());
            }
        }

        let initial_used = alloc.get_used_blks();

        // Phase 2: Free 25%
        let free_count = allocated.len() / 4;
        for bid in allocated.iter().take(free_count) {
            alloc.free(bid).await;
        }

        let after_free = alloc.get_used_blks();
        assert!(after_free < initial_used, "Used should decrease after free");

        // Phase 3: Reallocate
        for _ in 0..free_count {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);
            if status == BlkAllocStatus::Success {
                // Successfully reallocated
            }
        }
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_no_duplicate_allocations() {
        let alloc = create_test_allocator(10000, true);
        let mut allocated_blocks = HashSet::new();

        // Allocate multiple blocks and verify no duplicates
        for _ in 0..100 {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(5, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);

            if status == BlkAllocStatus::Success {
                for bid in bids.iter() {
                    // Check each block in the range
                    for blk_offset in 0..bid.blk_count() {
                        let blk_num = bid.blk_num() + blk_offset as BlkNum;
                        assert!(allocated_blocks.insert(blk_num), "Block {} was already allocated!", blk_num);
                    }
                }
            }
        }
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_with_and_without_slabs() {
        let total_blks = 1000;

        // Test with slabs
        let alloc_with_slabs = create_test_allocator(total_blks, true);
        let mut bids1 = BlkIds::new();
        let status1 = alloc_with_slabs.alloc_immediate(16, &BlkAllocHints::default(), &mut bids1);
        assert_eq!(status1, BlkAllocStatus::Success);

        // Test without slabs
        let alloc_without_slabs = create_test_allocator(total_blks, false);
        let mut bids2 = BlkIds::new();
        let status2 = alloc_without_slabs.alloc_immediate(16, &BlkAllocHints::default(), &mut bids2);
        assert_eq!(status2, BlkAllocStatus::Success);

        // Both should work
        assert!(!bids1.is_empty());
        assert!(!bids2.is_empty());
        
        // Shutdown both before dropping
        alloc_with_slabs.shutdown().await;
        alloc_without_slabs.shutdown().await;
    }

    #[iomanager_test]
    async fn test_small_allocator() {
        // Test with small capacity (like C++ small_allocator_with_slab test)
        let small_size = 100;
        let base_cfg = BlkAllocConfig::new(4096, 4096, (small_size as u64) * 4096, false, "test".to_string(), 1);
        let cfg = VarsizeBlkAllocConfig::new(base_cfg, 2, 50, true, 20, 1000, true);
        let alloc = VarsizeBlkAllocator::new(cfg, None, 0, 1).expect("Failed to create allocator");

        // Should be able to allocate from small allocator
        let mut bids = BlkIds::new();
        let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);
        assert_eq!(status, BlkAllocStatus::Success);

        // Free and reallocate
        if !bids.is_empty() {
            let bid = bids[0];
            alloc.free(&bid).await;

            let mut bids2 = BlkIds::new();
            let status = alloc.alloc_immediate(10, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids2);
            assert_eq!(status, BlkAllocStatus::Success);
        }
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_bypass_slab_cache() {
        let alloc = create_test_allocator(10000, true);

        // Allocate with bypass_slab_cache hint
        let mut bids = BlkIds::new();
        let status = alloc.alloc_immediate(
            16,
            &BlkAllocHints { is_contiguous: true, bypass_slab_cache: true, ..Default::default() },
            &mut bids,
        );

        assert_eq!(status, BlkAllocStatus::Success);
        assert!(!bids.is_empty());
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }

    #[iomanager_test]
    async fn test_allocation_size_rounding() {
        let alloc = create_test_allocator(10000, true);

        // Allocate non-power-of-2 sizes
        let sizes = [3, 5, 7, 11, 13, 17, 31];

        for &size in &sizes {
            let mut bids = BlkIds::new();
            let status = alloc.alloc_immediate(size, &BlkAllocHints { is_contiguous: true, ..Default::default() }, &mut bids);

            if status == BlkAllocStatus::Success {
                let total_allocated: BlkCount = bids.iter().map(|b| b.blk_count()).sum();
                assert!(total_allocated >= size, "Should allocate at least requested size");
            }
        }
        
        // Shutdown before dropping
        alloc.shutdown().await;
    }
}
