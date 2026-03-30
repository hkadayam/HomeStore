/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <folly/MPMCQueue.h>

#include <homestore/blk.h>
#include "blk_allocator.h"
#include "blk_cache.h"

namespace homestore {

///
/// SlabCache — per-portion in-memory block cache.
///
/// Holds free BlkIds in power-of-2 sized buckets (slabs):
///   slab 0 = 1-block  entries  (2^0)
///   slab 1 = 2-block  entries  (2^1)
///   ...
///   slab 8 = 256-block entries (2^8)
///
/// Allocation strategy (3-step):
///   1. Exact slab hit  — pop from target slab, return excess remainder to slab
///   2. Break up        — pop from a larger slab, use nblks, return remainder to slab
///   3. Merge down      — combine entries from smaller slabs (non-contiguous only)
///
/// Free: split bid into power-of-2 chunks and push each into the corresponding slab.
/// Thread-safe: slab queues are folly::MPMCQueue — try_alloc and try_free are lock-free.
///
class SlabCache {
public:
    static constexpr slab_idx_t NUM_SLABS = 9; // slabs 0..8  →  1..256 blocks

    SlabCache(blk_num_t max_cached_blks, chunk_num_t chunk_id);

    /// Allocate nblks. Returns SUCCESS on full alloc, PARTIAL on partial (non-contiguous only),
    /// SPACE_FULL on complete miss. Any excess blocks from break-up that couldn't be pushed back into the
    /// slab (queue full) are appended to excess — the caller must free them back to the bitmap.
    BlkAllocStatus try_alloc(blk_count_t nblks, bool is_contiguous, BlkIds& out, BlkIds& excess);

    /// Try to free bid into slab cache. Splits into power-of-2 chunks and pushes into slabs. Returns {SUCCESS, {}}
    /// if fully cached. On first chunk that doesn't fit, stops and returns {PARTIAL or FAILED, remaining_bid}.
    std::pair< BlkAllocStatus, BlkId > try_free(BlkId const& bid);

    bool can_cache_more() const { return cached_blk_count_.load(std::memory_order_relaxed) < max_cached_blks_; }
    bool is_full() const { return cached_blk_count_.load(std::memory_order_relaxed) >= max_cached_blks_; }
    bool needs_refill() const { return cached_blk_count_.load(std::memory_order_relaxed) < (max_cached_blks_ / 4); }
    blk_num_t total_cached_blks() const { return cached_blk_count_.load(std::memory_order_relaxed); }

    /// Round UP nblks to the next power-of-2 slab index.
    static slab_idx_t slab_idx_for(blk_count_t nblks);

    /// Round DOWN: returns (slab_idx, remainder_blks).
    static std::pair< slab_idx_t, blk_count_t > round_down_slab(blk_count_t nblks);

private:
    struct Slab {
        blk_count_t slab_size_{1};
        std::unique_ptr< folly::MPMCQueue< BlkId > > free_blks_; // constructed in SlabCache ctor
    };

    /// Pop from slabs_[idx]; give exactly nblks to out; put (slab_size - nblks) excess back.
    /// Precondition: slab_size >= nblks.
    BlkAllocStatus try_alloc_in_slab(slab_idx_t idx, blk_count_t nblks, BlkIds& out, BlkIds& excess);

    /// Search slabs_[target+1..NUM_SLABS), pop from the first non-empty one.
    BlkAllocStatus break_up(slab_idx_t target_idx, blk_count_t nblks, BlkIds& out, BlkIds& excess);

    /// Accumulate entries from slabs_[target-1..0] until nblks is satisfied (non-contiguous).
    BlkAllocStatus merge_down(slab_idx_t target_idx, blk_count_t nblks, BlkIds& out, BlkIds& excess);

    std::array< Slab, NUM_SLABS > slabs_;
    std::atomic< blk_num_t > cached_blk_count_{0};
    blk_num_t max_cached_blks_;
    chunk_num_t chunk_id_;
};

///
/// InmemPortion — one lock-granularity slice of the in-memory bitmap with an embedded slab cache.
///
/// Replaces the old BlkAllocPortion which only had a mutex + temperature.
/// Now additionally owns:
///   slab_cache_          — fast alloc/free without bitmap scanning
///   sweep_cursor_        — position for the next bitmap fill sweep
///   estimated_free_blks_ — cached estimate of free blocks in this range
///
/// Any thread may access any InmemPortion — no thread-ownership.
/// Slab operations (slab_cache_.try_alloc, slab_cache_.try_free) are fully lock-free (MPMC).
/// Callers must hold mtx_ (via portion_lock()) only when scanning or modifying the bitmap.
///
struct InmemPortion {
    blk_num_t start_blk_{0};
    blk_num_t end_blk_{0}; // exclusive
    SlabCache slab_cache_;
    blk_num_t sweep_cursor_{0};
    bool sweep_in_progress_{false};
    std::optional< blk_num_t > estimated_free_blks_;
    mutable std::mutex mtx_;

    InmemPortion(blk_num_t start, blk_num_t end, blk_num_t max_cached_blks, chunk_num_t chunk_id) :
            start_blk_{start},
            end_blk_{end},
            slab_cache_{max_cached_blks, chunk_id},
            sweep_cursor_{start} {}

    InmemPortion(InmemPortion const&) = delete;
    InmemPortion(InmemPortion&&) = delete;
    InmemPortion& operator=(InmemPortion const&) = delete;
    InmemPortion& operator=(InmemPortion&&) = delete;

    blk_num_t len() const { return end_blk_ - start_blk_; }
    bool contains(blk_num_t blknum) const { return blknum >= start_blk_ && blknum < end_blk_; }
    auto portion_lock() const { return std::scoped_lock< std::mutex >(mtx_); }
};

///
/// SegmentManager — owns all segments and their portions.
///
/// Layout:
///   Total blocks  →  num_segments_ Segments
///                 →  each Segment has num_portions_per_seg_ InmemPortions
///
/// Segments and their portions are accessible by any thread; all coordination is
/// done through InmemPortion::mtx_.
///
/// Segment selection for new allocations: round-robin via rr_seg_.
/// Block → segment:  blknum / blks_per_segment_
/// Block → portion:  (blknum - seg.start_blk_) / blks_per_portion_
///
class SegmentManager {
public:
    struct Segment {
        uint32_t seg_id_{0};
        blk_num_t start_blk_{0};
        blk_num_t num_blks_{0};
        blk_num_t clock_hand_{0}; // cycles through portions for allocation
        std::vector< unique< InmemPortion > > portions_;
    };

    SegmentManager(blk_num_t num_blks, uint32_t num_segments, blk_num_t blks_per_portion,
                   blk_num_t max_cache_blks_per_portion, chunk_num_t chunk_id);

    /// Select a segment for a new allocation. If hints.preferred_seg_id is set and valid, that segment
    /// is returned directly; otherwise falls back to round-robin.
    Segment& select_segment(blk_alloc_hints const& hints = {});

    /// Map blk number → owning Segment.
    Segment& blkid_to_segment(blk_num_t blknum);
    Segment const& blkid_to_segment(blk_num_t blknum) const;

    /// Map blk number → InmemPortion by blk range.
    InmemPortion& blkid_to_portion(blk_num_t blknum);
    InmemPortion const& blkid_to_portion(blk_num_t blknum) const;

    /// Select the next InmemPortion from seg for allocation (advances clock_hand_).
    InmemPortion& next_alloc_portion(Segment& seg);

    std::vector< Segment >& segments() { return segments_; }
    std::vector< Segment > const& segments() const { return segments_; }

    uint32_t num_segments() const { return static_cast< uint32_t >(segments_.size()); }
    blk_num_t blks_per_segment() const { return blks_per_segment_; }
    blk_num_t blks_per_portion() const { return blks_per_portion_; }

private:
    uint32_t portion_idx_for(blk_num_t blknum, blk_num_t seg_start_blk) const;

    std::vector< Segment > segments_;
    blk_num_t blks_per_segment_;
    blk_num_t blks_per_portion_;
    std::atomic< uint32_t > rr_seg_{0};
};

} // namespace homestore
