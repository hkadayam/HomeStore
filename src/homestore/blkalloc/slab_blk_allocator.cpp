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
#include <optional>

#include <fmt/format.h>
#include "sisl/logging/logging.h"
#include "sisl/fds/rcu.h"

#include "slab_blk_allocator.h"

namespace homestore {
namespace blkalloc {

SlabBlkAllocator::SlabBlkAllocator(SlabBlkAllocConfig const& cfg, std::optional< sisl::IoBufShared > buf,
                                   chunk_num_t chunk_id) :
        BlkAllocator{cfg, chunk_id},
        cfg_{cfg},
        seg_mgr_{cfg_.capacity_, cfg_.num_segments_, cfg_.blks_per_portion_,
                 (cfg_.alloc_mode == AllocMode::CompactAlloc) ? cfg_.blks_per_portion_
                                                              : cfg_.max_cache_blks_per_portion(),
                 chunk_id},
        metrics_{cfg_.unique_name_.c_str()} {
    BLKALLOC_LOG(INFO, "creating allocator mode={} capacity={} persistent={} use_slab_cache={}",
                 (cfg_.alloc_mode == AllocMode::CompactAlloc) ? "CompactAlloc" : "ExpandedAlloc", cfg_.capacity_,
                 cfg_.persistent_, cfg_.use_slab_cache_);

    if (cfg_.alloc_mode == AllocMode::CompactAlloc) {
        if (cfg_.persistent_) {
            const bool is_recovery = buf.has_value();
            ondisk_bm_ = std::make_unique< BitmapBlkAllocator >(cfg_, seg_mgr_, chunk_id, std::move(buf));
            if (is_recovery) {
                alloced_blk_count_.store(to_i64(ondisk_bm_->get_used_blks()), std::memory_order_relaxed);
            }
        }
        load();
        return;
    }

    // ExpandedAlloc path
    inmem_bm_ = std::make_unique< BitmapBlkAllocator >(cfg_, seg_mgr_, chunk_id);
    if (cfg_.persistent_) {
        const bool is_recovery = buf.has_value();
        ondisk_bm_ = std::make_unique< BitmapBlkAllocator >(cfg_, seg_mgr_, chunk_id, std::move(buf));
        if (is_recovery) {
            alloced_blk_count_.store(to_i64(ondisk_bm_->get_used_blks()), std::memory_order_relaxed);
            inmem_bm_->copy_from(*ondisk_bm_);
            BLKALLOC_LOG(INFO, "loaded bitmap total_blks={} used_blks={}", num_blks_, get_used_blks());
            recovering_ = new bool(true);
        }
    }

    // Register with the module-scoped sweep service. The service is allowed to fire refill tasks
    // immediately, but recovery_completed() is what triggers the first proactive refill — until then,
    // the periodic ticker may already enqueue if needs_refill() is true (harmless: refill writes only
    // into inmem_bm_, and any reserved blocks have been committed via commit() before we get here).
    sweep_handle_ = sweep_service().register_allocator(
        seg_mgr_, [this](InmemPortion& portion) { fill_cache_for_portion(portion); }, name_);
}

SlabBlkAllocator::~SlabBlkAllocator() {
    // Drop the handle first — its destructor sets alive_=false and waits for in-flight refill workers
    // targeting this allocator's portions to drain. Only then is it safe to destroy inmem_bm_/seg_mgr_.
    sweep_handle_.reset();
    // Clean up in case recovery_completed() was never called (e.g. error path).
    delete recovering_;
}

// ---- CompactAlloc load ----

void SlabBlkAllocator::load() {
    if (cfg_.alloc_mode != AllocMode::CompactAlloc) {
        return;
    }
    BLKALLOC_LOG(INFO, "load: populating slab caches for CompactAlloc, persistent={}", cfg_.persistent_);

    if (cfg_.persistent_) {
        // Scan ondisk_bm_ for free blocks and load all of them into slab caches.
        // Return num_consumed=0 so the ondisk bitmap is never modified — only commit() may set bits there.
        // keep_on_going=true always since CompactAlloc has no inmem_bm_ fallback; all free blocks must be in slab.
        for (auto& seg : seg_mgr_.segments()) {
            for (auto& p_ptr : seg.portions_) {
                InmemPortion& portion = *p_ptr;
                ondisk_bm_->scan_free_blks(portion, [&portion](BlkId const& bid) -> std::pair< bool, blk_count_t > {
                    portion.slab_cache_.try_free(bid);
                    // Always set num_consumed to be 0, because ondisk_bm should never set bits directly.
                    return {true, /*num_consumed=*/0};
                });
            }
        }
    } else {
        // All blocks are free: place each block in the highest slab; break_up() splits on demand.
        const blk_count_t max_slab_size = static_cast< blk_count_t >(1) << (SlabCache::NUM_SLABS - 1);
        for (auto& seg : seg_mgr_.segments()) {
            for (auto& p_ptr : seg.portions_) {
                InmemPortion& portion = *p_ptr;
                for (blk_num_t b = portion.start_blk_; b < portion.end_blk_; b += max_slab_size) {
                    const blk_count_t count =
                        static_cast< blk_count_t >(std::min< blk_num_t >(max_slab_size, portion.end_blk_ - b));
                    portion.slab_cache_.try_free(BlkId{b, count, chunk_id_});
                }
            }
        }
    }
}

// ---- sweep callback ----

// Refill the slab cache for one portion from inmem_bm_. Consumed blocks are marked (num_consumed > 0)
// so the bitmap tracks them as in-cache and they won't be double-allocated.
// Invoked by blkalloc::SweepService workers on this allocator's sweep_handle_.
void SlabBlkAllocator::fill_cache_for_portion(InmemPortion& portion) {
    if (!cfg_.use_slab_cache_) return;
    inmem_bm_->scan_free_blks(portion, [&portion](BlkId const& bid) -> std::pair< bool, blk_count_t > {
        auto [status, remaining] = portion.slab_cache_.try_free(bid);
        const blk_count_t consumed = bid.blk_count() - remaining.blk_count();
        return {consumed > 0, consumed};
    });
}

// ---- alloc ----

BlkAllocStatus SlabBlkAllocator::alloc_contiguous(BlkId& out_blkid) {
    blk_alloc_hints hints;
    hints.is_contiguous = true;

    BlkIds out_blkids;
    auto const status = alloc(1, hints, out_blkids);
    if (status == BlkAllocStatus::SUCCESS) {
        out_blkid = out_blkids.front();
        BLKALLOC_LOG(DEBUG, "alloc_contiguous: blk_num={} nblks={} chunk={}", out_blkid.blk_num(),
                     out_blkid.blk_count(), out_blkid.chunk_num());
    }
    return status;
}

BlkAllocStatus SlabBlkAllocator::alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkIds& out_blkids) {
    COUNTER_INCREMENT(metrics_, num_alloc, 1);

    // Slab cache path — used by both CompactAlloc (slab is the only source) and ExpandedAlloc with
    // use_slab_cache_ (slab as fast cache backed by bitmap). The slab try_alloc is lock-free (MPMC).
    // On miss the only difference is what we can refill from:
    //   - ExpandedAlloc: fill_cache_for_portion() scans inmem_bm_ under portion lock, refills slab.
    //   - CompactAlloc: no bitmap — retry across portions to ride out the transient gap from concurrent
    //     break_up (pop-and-split of a large slab entry is not atomic).
    blk_count_t slab_got{0}; // tracks blocks already obtained from slab PARTIAL results (non-contiguous)

    static constexpr blk_count_t max_slab_blks = static_cast< blk_count_t >(1) << (SlabCache::NUM_SLABS - 1);
    const bool slab_can_satisfy = !hints.is_contiguous || nblks <= max_slab_blks;

    if (slab_can_satisfy && (cfg_.alloc_mode == AllocMode::CompactAlloc || cfg_.use_slab_cache_)) {
        const auto max_attempts =
            (cfg_.alloc_mode == AllocMode::CompactAlloc) ? HS_DYNAMIC_CONFIG(blkallocator.max_slab_alloc_attempt) : 1u;

        // Excess collects blocks that couldn't be pushed back to slab during break-up / merge-down.
        // For ExpandedAlloc these are freed back to the bitmap below.
        BlkIds excess;
        for (uint32_t retry{0}; retry < max_attempts; ++retry) {
            SegmentManager::Segment& seg = seg_mgr_.select_segment(hints);
            const auto num_portions = seg.portions_.size();
            for (size_t p{0}; p < num_portions; ++p) {
                InmemPortion& portion = seg_mgr_.next_alloc_portion(seg);
                const blk_count_t remaining = nblks - slab_got;

                BlkAllocStatus status =
                    portion.slab_cache_.try_alloc(remaining, hints.is_contiguous, out_blkids, excess);

                // On cache miss with a bitmap backing: refill the slab from bitmap and retry once.
                if (status != BlkAllocStatus::SUCCESS && status != BlkAllocStatus::PARTIAL && inmem_bm_) {
                    COUNTER_INCREMENT(metrics_, num_retries, 1);
                    fill_cache_for_portion(portion);
                    status = portion.slab_cache_.try_alloc(remaining, hints.is_contiguous, out_blkids, excess);
                }

                if (status == BlkAllocStatus::SUCCESS) {
                    if (inmem_bm_ && sweep_handle_ && portion.slab_cache_.needs_refill()) {
                        sweep_service().request_refill(*sweep_handle_, portion);
                    }
                    // Return excess blocks (from break-up that couldn't fit back into slab) to bitmap.
                    if (inmem_bm_ && !excess.empty()) {
                        BLKALLOC_LOG(DEBUG, "alloc nblks={}: returning {} excess blkids to bitmap", nblks,
                                     excess.size());
                        for (auto const& ebid : excess) {
                            inmem_bm_->free(ebid);
                        }
                    }
                    alloced_blk_count_.fetch_add(nblks, std::memory_order_relaxed);
                    BLKALLOC_LOG(DEBUG, "alloc nblks={}: SUCCESS from slab portion=[{},{}), used_blks={}", nblks,
                                 portion.start_blk_, portion.end_blk_, get_used_blks());
                    return BlkAllocStatus::SUCCESS;
                }

                // PARTIAL from merge_down: keep the blocks we got and reduce the ask for the next portion.
                if (status == BlkAllocStatus::PARTIAL) {
                    blk_count_t got_this_round{0};
                    for (auto const& bid : out_blkids) {
                        got_this_round += bid.blk_count();
                    }
                    slab_got = got_this_round;
                    BLKALLOC_LOG(DEBUG, "alloc nblks={}: PARTIAL from slab, got {} so far", nblks, slab_got);
                }
            }
        }

        // Return any accumulated excess back to bitmap.
        if (inmem_bm_ && !excess.empty()) {
            BLKALLOC_LOG(DEBUG, "alloc nblks={}: slab miss, returning {} excess blkids to bitmap", nblks,
                         excess.size());
            for (auto const& ebid : excess) {
                inmem_bm_->free(ebid);
            }
        }

        if (cfg_.alloc_mode == AllocMode::CompactAlloc) {
            // CompactAlloc has no bitmap fallback. If we got partial results, free them back to slab.
            for (auto const& bid : out_blkids) {
                auto& portion = seg_mgr_.blkid_to_portion(bid.blk_num());
                portion.slab_cache_.try_free(bid);
            }
            out_blkids.clear();
            BLKALLOC_LOG(DEBUG, "alloc nblks={}: CompactAlloc SPACE_FULL after {} attempts", nblks, max_attempts);
            COUNTER_INCREMENT(metrics_, num_alloc_failure, 1);
            return BlkAllocStatus::SPACE_FULL;
        }
    }

    // Direct bitmap scan for remaining blocks (nblks - slab_got).
    const blk_count_t bitmap_need = nblks - slab_got;
    if (bitmap_need > 0) {
        COUNTER_INCREMENT(metrics_, num_blks_alloc_direct, 1);
        const BlkAllocStatus status = inmem_bm_->alloc(bitmap_need, hints, out_blkids);

        if (status == BlkAllocStatus::SUCCESS) {
            alloced_blk_count_.fetch_add(nblks, std::memory_order_relaxed);
            return BlkAllocStatus::SUCCESS;
        }

        if (status == BlkAllocStatus::PARTIAL) {
            // Got some from bitmap but not all — count total across slab + bitmap results.
            blk_count_t total_got{0};
            for (auto const& bid : out_blkids) {
                total_got += bid.blk_count();
            }
            alloced_blk_count_.fetch_add(total_got, std::memory_order_relaxed);
            return BlkAllocStatus::PARTIAL;
        }

        // Bitmap also failed. If we have slab partial results, return those as PARTIAL.
        if (slab_got > 0) {
            alloced_blk_count_.fetch_add(slab_got, std::memory_order_relaxed);
            return BlkAllocStatus::PARTIAL;
        }

        BLKALLOC_LOG(DEBUG, "alloc nblks={}: bitmap direct SPACE_FULL, used_blks={} available={}", nblks,
                     get_used_blks(), available_blks());
        COUNTER_INCREMENT(metrics_, num_alloc_failure, 1);
        return status;
    }

    // slab_got == nblks: slab PARTIAL results across portions fully satisfied the request.
    alloced_blk_count_.fetch_add(nblks, std::memory_order_relaxed);
    return BlkAllocStatus::SUCCESS;
}

// ---- free ----

void SlabBlkAllocator::free(BlkId const& bid) {
    BLKALLOC_LOG(DEBUG, "free bid=[blk={} count={} chunk={}]", bid.blk_num(), bid.blk_count(), bid.chunk_num());

    if (cfg_.alloc_mode == AllocMode::CompactAlloc) {
        // CompactAlloc: slab is the only source — try_free always succeeds (queues sized for all blocks).
        auto& portion = seg_mgr_.blkid_to_portion(bid.blk_num());
        portion.slab_cache_.try_free(bid);
    } else if (cfg_.use_slab_cache_) {
        // ExpandedAlloc with slab: try slab first, spill any remainder that didn't fit to bitmap.
        auto& portion = seg_mgr_.blkid_to_portion(bid.blk_num());
        auto [status, remaining] = portion.slab_cache_.try_free(bid);
        if (status != BlkAllocStatus::SUCCESS) {
            BLKALLOC_LOG(DEBUG, "free: slab try_free {}, remaining=[blk={} count={}] → bitmap", status,
                         remaining.blk_num(), remaining.blk_count());
            inmem_bm_->free(remaining);
        }
    } else {
        // ExpandedAlloc without slab: straight to bitmap.
        inmem_bm_->free(bid);
    }

    if (ondisk_bm_) {
        ondisk_bm_->free(bid);
    }

    alloced_blk_count_.fetch_sub(bid.blk_count(), std::memory_order_relaxed);
}

// ---- commit / persist / recovery ----

BlkAllocStatus SlabBlkAllocator::commit(BlkId const& bid) {
    BLKALLOC_LOG(DEBUG, "commit: blk_num={} nblks={} chunk={}", bid.blk_num(), bid.blk_count(), bid.chunk_num());
    if (inmem_bm_) {
        if (!ondisk_bm_) {
            // Non-persistent: inmem_bm_ is the sole source of truth, always mark committed blocks.
            return inmem_bm_->commit(bid);
        }
        sisl::Rcu::read_guard guard;
        const bool is_recovering = (sisl::Rcu::dereference(recovering_) != nullptr);
        if (is_recovering) {
            inmem_bm_->commit(bid);
        } else {
            BLKALLOC_DBG_ASSERT(inmem_bm_->is_blk_alloced(bid, true),
                                "commit() called on bid not already set in inmem_bm");
        }
    }
    return (ondisk_bm_) ? ondisk_bm_->commit(bid) : BlkAllocStatus::SUCCESS;
}

BlkAllocator::BufferGuard SlabBlkAllocator::acquire_buffer() {
    if (!ondisk_bm_) {
        return make_buffer_guard({}, []() {});
    }
    return ondisk_bm_->acquire_buffer();
}

void SlabBlkAllocator::recovery_completed() {
    bool* old = sisl::Rcu::xchg_pointer(&recovering_, static_cast< bool* >(nullptr));
    sisl::Rcu::synchronize();
    delete old;

    // Trigger immediate refill of every portion below threshold instead of waiting for the next periodic
    // tick. All requests are LOW priority — the periodic ticker would do the same eventually.
    if (!sweep_handle_) return;
    for (auto& seg : seg_mgr_.segments()) {
        for (auto& p_ptr : seg.portions_) {
            InmemPortion& portion = *p_ptr;
            if (portion.slab_cache_.needs_refill()) {
                sweep_service().request_refill(*sweep_handle_, portion);
            }
        }
    }
}

// ---- query ----

bool SlabBlkAllocator::is_blk_alloced(BlkId const& b, bool use_lock) const {
    if (inmem_bm_) {
        return inmem_bm_->is_blk_alloced(b, use_lock);
    }
    return ondisk_bm_ ? ondisk_bm_->is_blk_alloced(b, use_lock) : true;
}

bool SlabBlkAllocator::is_blk_alloced_on_disk(BlkId const& b, bool use_lock) const {
    if (!ondisk_bm_) {
        return true;
    }
    return ondisk_bm_->is_blk_alloced(b, use_lock);
}

blk_num_t SlabBlkAllocator::available_blks() const {
    const auto used = alloced_blk_count_.load(std::memory_order_acquire);
    return (used >= 0 && static_cast< blk_num_t >(used) <= num_blks_) ? (num_blks_ - static_cast< blk_num_t >(used))
                                                                      : 0;
}

blk_num_t SlabBlkAllocator::get_used_blks() const {
    const auto used = alloced_blk_count_.load(std::memory_order_acquire);
    return (used >= 0) ? static_cast< blk_num_t >(used) : 0;
}

std::string SlabBlkAllocator::to_string() const {
    return fmt::format("SlabBlkAllocator name={} total_blks={} used={} available={}", name_, num_blks_, get_used_blks(),
                       available_blks());
}

nlohmann::json SlabBlkAllocator::get_status(int) const {
    return nlohmann::json{};
}

} // namespace blkalloc
} // namespace homestore
