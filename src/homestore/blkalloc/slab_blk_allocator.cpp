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
#include <sisl/logging/logging.h>
#include <sisl/fds/thread_factory.h>
#include <urcu.h>

#include "slab_blk_allocator.h"

namespace homestore {

SlabBlkAllocator::SlabBlkAllocator(SlabBlkAllocConfig const& cfg, std::optional< sisl::ByteArray > buf,
                                         chunk_num_t chunk_id) :
        BlkAllocator{cfg, chunk_id},
        cfg_{cfg},
        seg_mgr_{cfg_.capacity_, cfg_.num_segments_, cfg_.blks_per_portion_,
                 (cfg_.alloc_mode == AllocMode::CompactAlloc) ? cfg_.blks_per_portion_
                                                              : cfg_.max_cache_blks_per_portion(),
                 chunk_id},
        metrics_{cfg_.unique_name_.c_str()} {
    if (cfg_.alloc_mode == AllocMode::CompactAlloc) {
        if (cfg_.persistent_) {
            const bool is_recovery = buf.has_value();
            ondisk_bm_ = std::make_unique< BitmapBlkAllocator >(cfg_, seg_mgr_, /*inject_slab_on_free=*/false,
                                                                chunk_id, std::move(buf));
            if (is_recovery) {
                alloced_blk_count_.store(to_i64(ondisk_bm_->get_used_blks()), std::memory_order_relaxed);
            }
        }
        load();
        return;
    }

    // ExpandedAlloc path
    inmem_bm_ = std::make_unique< BitmapBlkAllocator >(cfg_, seg_mgr_, cfg_.use_slab_cache_, chunk_id);
    if (cfg_.persistent_) {
        const bool is_recovery = buf.has_value();
        ondisk_bm_ = std::make_unique< BitmapBlkAllocator >(cfg_, seg_mgr_, /*inject_slab_on_free=*/false, chunk_id,
                                                            std::move(buf));
        if (is_recovery) {
            alloced_blk_count_.store(to_i64(ondisk_bm_->get_used_blks()), std::memory_order_relaxed);
            inmem_bm_->copy_from(*ondisk_bm_);
            BLKALLOC_LOG(INFO, "loaded bitmap total_blks={} used_blks={}", num_blks_, get_used_blks());
            recovering_ = new bool(true);
        }
    }

    sweep_thread_ = sisl::named_thread("blkalloc_sweep_" + name_, [this]() { sweep_worker(); });
    request_sweep();
}

SlabBlkAllocator::~SlabBlkAllocator() {
    if (sweep_thread_.joinable()) {
        {
            std::unique_lock< std::mutex > lk{sweep_mutex_};
            sweep_stop_ = true;
            sweep_cv_.notify_all();
        }
        sweep_thread_.join();
    }
    // Clean up in case recovery_completed() was never called (e.g. error path).
    delete recovering_;
}

// ---- CompactAlloc load ----

void SlabBlkAllocator::load() {
    if (cfg_.alloc_mode != AllocMode::CompactAlloc) { return; }

    if (cfg_.persistent_) {
        // Scan ondisk_bm_ to find which blocks are free, push only those into slab caches.
        for (auto& seg : seg_mgr_.segments()) {
            for (auto& p_ptr : seg.portions_) {
                InmemPortion& portion = *p_ptr;
                ondisk_bm_->scan_free_blks(portion, [&portion](BlkId const& bid) -> bool {
                    portion.slab_cache_.free_blk(bid);
                    return true; // keep scanning — entire portion fits in slab
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
                    portion.slab_cache_.free_blk(BlkId{b, count, chunk_id_});
                }
            }
        }
    }
}

// ---- sweep ----

void SlabBlkAllocator::sweep_worker() {
    while (true) {
        {
            std::unique_lock< std::mutex > lk{sweep_mutex_};
            sweep_cv_.wait_for(
                lk, std::chrono::milliseconds(HS_DYNAMIC_CONFIG(blkallocator.free_blk_cache_refill_frequency_ms)),
                [this]() { return sweep_requested_ || sweep_stop_; });
            if (sweep_stop_)
                break;
            sweep_requested_ = false;
        }

        if (!cfg_.use_slab_cache_) {
            continue;
        }

        blk_num_t blks_added{0};
        for (auto& seg : seg_mgr_.segments()) {
            for (auto& p_ptr : seg.portions_) {
                InmemPortion& portion = *p_ptr;
                if (portion.slab_cache_.needs_refill()) {
                    fill_cache_for_portion(portion);
                    blks_added += portion.slab_cache_.total_cached_blks();
                }
            }
        }

        {
            std::unique_lock< std::mutex > lk{sweep_mutex_};
            sweep_blks_added_ = blks_added;
            sweep_cv_.notify_all();
        }
    }
}

// Uses BitmapBlkAllocator::scan_free_blks() which acquires the portion lock internally,
// scans inmem_bm_ for free bits, marks them as in-cache, and calls the producer lambda
// to inject each range into the slab cache.
void SlabBlkAllocator::fill_cache_for_portion(InmemPortion& portion) {
    inmem_bm_->scan_free_blks(portion, [&portion](BlkId const& bid) -> bool {
        portion.slab_cache_.free_blk(bid);
        return !portion.slab_cache_.is_full();
    });
}

void SlabBlkAllocator::request_sweep(blk_count_t wait_for_blks) {
    {
        std::unique_lock< std::mutex > lk{sweep_mutex_};
        sweep_requested_ = true;
        sweep_cv_.notify_all();
    }
    if (wait_for_blks > 0) {
        std::unique_lock< std::mutex > lk{sweep_mutex_};
        sweep_cv_.wait(lk, [&]() { return sweep_blks_added_ >= wait_for_blks || !sweep_requested_ || sweep_stop_; });
    }
}

// ---- alloc ----

BlkAllocStatus SlabBlkAllocator::alloc_contiguous(BlkId& out_blkid) {
    blk_alloc_hints hints;
    hints.is_contiguous = true;
    return alloc(1, hints, out_blkid);
}

BlkAllocStatus SlabBlkAllocator::alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkId& out_blkid) {
    COUNTER_INCREMENT(metrics_, num_alloc, 1);

    MultiBlkId& mout = r_cast< MultiBlkId& >(out_blkid);
    mout = MultiBlkId{};

    // CompactAlloc: slab is the only allocator — no bitmap fallback.
    if (cfg_.alloc_mode == AllocMode::CompactAlloc) {
        SegmentManager::Segment& seg = seg_mgr_.select_segment(hints);
        InmemPortion& portion = seg_mgr_.next_alloc_portion(seg);
        const BlkAllocStatus status = portion.slab_cache_.try_alloc(nblks, hints.is_contiguous, mout);
        if (status == BlkAllocStatus::SUCCESS) {
            alloced_blk_count_.fetch_add(nblks, std::memory_order_relaxed);
        } else {
            COUNTER_INCREMENT(metrics_, num_alloc_failure, 1);
        }
        return status;
    }

    // ExpandedAlloc path
    if (cfg_.use_slab_cache_) {
        // Step 1: try the slab cache of the selected portion.
        SegmentManager::Segment& seg = seg_mgr_.select_segment(hints);
        InmemPortion& portion = seg_mgr_.next_alloc_portion(seg);

        BlkAllocStatus status = portion.slab_cache_.try_alloc(nblks, hints.is_contiguous, mout);

        // Step 2: on cache miss, fill the cache inline and retry once.
        if (status != BlkAllocStatus::SUCCESS) {
            COUNTER_INCREMENT(metrics_, num_retries, 1);
            fill_cache_for_portion(portion);
            status = portion.slab_cache_.try_alloc(nblks, hints.is_contiguous, mout);
        }

        if (status == BlkAllocStatus::SUCCESS) {
            if (portion.slab_cache_.needs_refill()) {
                request_sweep();
            }
            alloced_blk_count_.fetch_add(nblks, std::memory_order_relaxed);
            return BlkAllocStatus::SUCCESS;
        }
    }

    // Direct bitmap scan across all portions via inmem_bm_->alloc().
    COUNTER_INCREMENT(metrics_, num_blks_alloc_direct, 1);
    const BlkAllocStatus status = inmem_bm_->alloc(nblks, hints, out_blkid);

    if (status == BlkAllocStatus::SUCCESS || status == BlkAllocStatus::PARTIAL) {
        blk_count_t got{0};
        auto it = mout.iterate();
        while (auto const b = it.next()) {
            got += b->blk_count();
        }
        alloced_blk_count_.fetch_add(got, std::memory_order_relaxed);
    } else {
        COUNTER_INCREMENT(metrics_, num_alloc_failure, 1);
    }

    return status;
}

// ---- free ----

void SlabBlkAllocator::free(BlkId const& bid) {
    if (cfg_.alloc_mode == AllocMode::CompactAlloc) {
        // CompactAlloc: free directly into slab; update ondisk_bm_ if persistent.
        auto& portion = seg_mgr_.blkid_to_portion(bid.blk_num());
        portion.slab_cache_.free_blk(bid);
    } else {
        // ExpandedAlloc: inmem_bm_->free() resets bits and injects into slab (inject_slab_on_free=true).
        inmem_bm_->free(bid);
    }

    if (ondisk_bm_) {
        ondisk_bm_->free(bid);
    }

    blk_count_t total{0};
    if (bid.is_multi()) {
        auto it = r_cast< MultiBlkId const& >(bid).iterate();
        while (auto const b = it.next()) {
            total += b->blk_count();
        }
    } else {
        total = bid.blk_count();
    }
    alloced_blk_count_.fetch_sub(total, std::memory_order_relaxed);
}

// ---- commit / persist / recovery ----

BlkAllocStatus SlabBlkAllocator::commit(BlkId const& bid) {
    if (inmem_bm_) {
        rcu_read_lock();
        const bool is_recovering = (rcu_dereference(recovering_) != nullptr);
        if (is_recovering) {
            inmem_bm_->commit(bid); // Journal replay during recovery: mark block in both bitmaps.
        } else {
            BLKALLOC_DBG_ASSERT(inmem_bm_->is_blk_alloced(bid, true),
                                "commit() called on bid not already set in inmem_bm");
        }
        rcu_read_unlock();
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
    bool* old = rcu_xchg_pointer(&recovering_, nullptr);
    synchronize_rcu();
    delete old;
}

// ---- query ----

bool SlabBlkAllocator::is_blk_alloced(BlkId const& b, bool use_lock) const {
    if (inmem_bm_) { return inmem_bm_->is_blk_alloced(b, use_lock); }
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
    return fmt::format("SlabBlkAllocator name={} total_blks={} used={} available={}", name_, num_blks_,
                       get_used_blks(), available_blks());
}

nlohmann::json SlabBlkAllocator::get_status(int) const {
    return nlohmann::json{};
}

} // namespace homestore
