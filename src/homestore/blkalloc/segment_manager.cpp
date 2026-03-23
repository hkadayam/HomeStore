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
#include <algorithm>

#include "sisl/fds/bitword.h"
#include "common/homestore_assert.hpp"
#include "segment_manager.h"

namespace homestore {

// ==================== SlabCache ====================
SlabCache::SlabCache(blk_num_t max_cached_blks, chunk_num_t chunk_id) :
        max_cached_blks_{max_cached_blks}, chunk_id_{chunk_id} {
    for (slab_idx_t i{0}; i < NUM_SLABS; ++i) {
        slabs_[i].slab_size_ = static_cast< blk_count_t >(1) << i;
        const size_t cap = std::max< size_t >(1, max_cached_blks / slabs_[i].slab_size_);
        slabs_[i].free_blks_ = std::make_unique< folly::MPMCQueue< BlkId > >(cap);
    }
}

slab_idx_t SlabCache::slab_idx_for(blk_count_t nblks) {
    if (nblks == 0)
        return 0;
    if (sisl_unlikely(nblks >= slab_tbl_size)) {
        return static_cast< slab_idx_t >((nblks > 1) ? sisl::logBase2(static_cast< blk_count_t >(nblks - 1)) + 1 : 0);
    }
    return nblks_to_slab_tbl[nblks];
}

std::pair< slab_idx_t, blk_count_t > SlabCache::round_down_slab(blk_count_t nblks) {
    if (sisl_unlikely(nblks >= slab_tbl_size)) {
        const auto s{slab_idx_for(nblks + 1) - 1};
        return {s, nblks - (1u << s)};
    }
    return nblks_to_round_down_slab_tbl[nblks];
}

void SlabCache::free_blk(BlkId const& bid) {
    blk_num_t blknum = bid.blk_num();
    blk_count_t remain = bid.blk_count();

    while (remain > 0) {
        const auto [slab_idx, excess] = round_down_slab(remain);
        if (slab_idx >= NUM_SLABS)
            break;
        const blk_count_t slab_size = slabs_[slab_idx].slab_size_;
        if (slabs_[slab_idx].free_blks_->writeIfNotFull(BlkId{blknum, slab_size, chunk_id_})) {
            cached_blk_count_.fetch_add(slab_size, std::memory_order_relaxed);
        }
        // Blocks that don't fit (queue full) are silently dropped.
        // For ExpandedAlloc: inmem_bm_ sweep will recapture them on the next fill pass.
        // For CompactAlloc: queue is sized for all blocks; this should not occur.
        blknum += slab_size;
        remain = excess;
    }
}

BlkAllocStatus SlabCache::try_alloc_in_slab(slab_idx_t idx, blk_count_t nblks, BlkIds& out) {
    if (idx >= NUM_SLABS)
        return BlkAllocStatus::SPACE_FULL;

    BlkId entry;
    if (!slabs_[idx].free_blks_->readIfNotEmpty(entry))
        return BlkAllocStatus::SPACE_FULL;

    const blk_count_t slab_size = slabs_[idx].slab_size_;
    cached_blk_count_.fetch_sub(slab_size, std::memory_order_relaxed);

    // Precondition: slab_size >= nblks (ensured by callers using round-up slab index)
    out.push_back(BlkId{entry.blk_num(), nblks, chunk_id_});
    if (slab_size > nblks) {
        // Return the trailing excess back into the cache
        free_blk(BlkId{entry.blk_num() + nblks, slab_size - nblks, chunk_id_});
    }
    return BlkAllocStatus::SUCCESS;
}

BlkAllocStatus SlabCache::break_up(slab_idx_t target_idx, blk_count_t nblks, BlkIds& out) {
    for (slab_idx_t idx = target_idx + 1; idx < NUM_SLABS; ++idx) {
        const auto st = try_alloc_in_slab(idx, nblks, out);
        if (st == BlkAllocStatus::SUCCESS) {
            return st;
        }
    }
    return BlkAllocStatus::SPACE_FULL;
}

BlkAllocStatus SlabCache::merge_down(slab_idx_t target_idx, blk_count_t nblks, BlkIds& out) {
    if (target_idx == 0)
        return BlkAllocStatus::SPACE_FULL;

    blk_count_t remain = nblks;
    for (slab_idx_t idx = target_idx - 1; remain > 0; --idx) {
        auto& slab = slabs_[idx];
        BlkId entry;
        while (remain > 0 && slab.free_blks_->readIfNotEmpty(entry)) {
            const blk_count_t slab_size = slab.slab_size_;
            cached_blk_count_.fetch_sub(slab_size, std::memory_order_relaxed);

            if (slab_size >= remain) {
                out.push_back(BlkId{entry.blk_num(), remain, chunk_id_});
                if (slab_size > remain) {
                    free_blk(BlkId{entry.blk_num() + remain, slab_size - remain, chunk_id_});
                }
                remain = 0;
            } else {
                out.push_back(BlkId{entry.blk_num(), slab_size, chunk_id_});
                remain -= slab_size;
            }
        }
        if (idx == 0)
            break;
    }

    if (remain == 0)
        return BlkAllocStatus::SUCCESS;
    return (remain < nblks) ? BlkAllocStatus::PARTIAL : BlkAllocStatus::SPACE_FULL;
}

BlkAllocStatus SlabCache::try_alloc(blk_count_t nblks, bool is_contiguous, BlkIds& out) {
    const slab_idx_t target_idx = std::min(slab_idx_for(nblks), static_cast< slab_idx_t >(NUM_SLABS - 1));

    // Step 1: Exact slab hit
    BlkAllocStatus st = try_alloc_in_slab(target_idx, nblks, out);
    if (st == BlkAllocStatus::SUCCESS)
        return st;

    // Step 2: Break up a larger slab
    st = break_up(target_idx, nblks, out);
    if (st == BlkAllocStatus::SUCCESS)
        return st;

    // Step 3: Merge smaller slabs (non-contiguous only)
    if (!is_contiguous) {
        st = merge_down(target_idx, nblks, out);
        if (st == BlkAllocStatus::SUCCESS || st == BlkAllocStatus::PARTIAL)
            return st;
    }

    return BlkAllocStatus::SPACE_FULL;
}

// ==================== SegmentManager ====================

SegmentManager::SegmentManager(blk_num_t num_blks, uint32_t num_segments, blk_num_t blks_per_portion,
                               blk_num_t max_cache_blks_per_portion, chunk_num_t chunk_id) :
        blks_per_portion_{blks_per_portion} {
    HS_REL_ASSERT_GT(num_segments, 0u);
    HS_REL_ASSERT_GT(blks_per_portion, 0u);

    // Align segment size so portions don't straddle a Bitset word boundary
    const blk_num_t portions_per_seg = std::max< blk_num_t >((num_blks / num_segments) / blks_per_portion, 1u);
    blks_per_segment_ = portions_per_seg * blks_per_portion;

    segments_.reserve(num_segments);
    blk_num_t seg_start = 0;
    for (uint32_t sid{0}; sid < num_segments; ++sid) {
        Segment seg;
        seg.seg_id_ = sid;
        seg.start_blk_ = seg_start;
        // Last segment absorbs any rounding remainder
        seg.num_blks_ = (sid + 1 < num_segments) ? blks_per_segment_ : (num_blks - seg_start);

        const blk_num_t this_portions = (seg.num_blks_ + blks_per_portion_ - 1) / blks_per_portion_;
        seg.portions_.reserve(this_portions);
        blk_num_t p_start = seg_start;
        for (blk_num_t pid{0}; pid < this_portions; ++pid) {
            const blk_num_t p_end = std::min(p_start + blks_per_portion_, seg_start + seg.num_blks_);
            seg.portions_.emplace_back(
                std::make_unique< InmemPortion >(p_start, p_end, max_cache_blks_per_portion, chunk_id));
            p_start = p_end;
        }

        seg_start += seg.num_blks_;
        segments_.push_back(std::move(seg));
    }
}

SegmentManager::Segment& SegmentManager::select_segment(blk_alloc_hints const& hints) {
    if (hints.preferred_seg_id && *hints.preferred_seg_id < segments_.size()) {
        return segments_[*hints.preferred_seg_id];
    }
    const auto idx = rr_seg_.fetch_add(1, std::memory_order_relaxed) % segments_.size();
    return segments_[idx];
}

uint32_t SegmentManager::portion_idx_for(blk_num_t blknum, blk_num_t seg_start_blk) const {
    const auto& seg = blkid_to_segment(blknum);
    const auto raw = (blknum - seg_start_blk) / blks_per_portion_;
    return to_u32(std::min< blk_num_t >(raw, seg.portions_.size() - 1));
}

SegmentManager::Segment& SegmentManager::blkid_to_segment(blk_num_t blknum) {
    const auto seg_idx = std::min< size_t >(blknum / blks_per_segment_, segments_.size() - 1);
    return segments_[seg_idx];
}

SegmentManager::Segment const& SegmentManager::blkid_to_segment(blk_num_t blknum) const {
    const auto seg_idx = std::min< size_t >(blknum / blks_per_segment_, segments_.size() - 1);
    return segments_[seg_idx];
}

InmemPortion& SegmentManager::blkid_to_portion(blk_num_t blknum) {
    auto& seg = blkid_to_segment(blknum);
    return *seg.portions_[portion_idx_for(blknum, seg.start_blk_)];
}

InmemPortion const& SegmentManager::blkid_to_portion(blk_num_t blknum) const {
    auto const& seg = blkid_to_segment(blknum);
    return *seg.portions_[portion_idx_for(blknum, seg.start_blk_)];
}

InmemPortion& SegmentManager::next_alloc_portion(Segment& seg) {
    const auto idx = seg.clock_hand_ % seg.portions_.size();
    ++seg.clock_hand_;
    return *seg.portions_[idx];
}

} // namespace homestore
