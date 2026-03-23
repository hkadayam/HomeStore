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
#include <optional>

#include "bitmap_blk_allocator.h"
#include "common/homestore_assert.hpp"

namespace homestore {

BitmapBlkAllocator::BitmapBlkAllocator(BlkAllocConfig const& cfg, SegmentManager& seg_mgr, bool inject_slab_on_free,
                                       chunk_num_t id, std::optional< sisl::ByteArray > buf) :
        BlkAllocator{cfg, id}, seg_mgr_{seg_mgr}, inject_slab_on_free_{inject_slab_on_free} {
    if (buf.has_value()) {
        bm_ = std::make_unique< sisl::Bitset >(std::move(*buf));
        alloced_blk_count_.store(to_i64(bm_->get_set_count()), std::memory_order_relaxed);
    } else {
        bm_ = std::make_unique< sisl::Bitset >(cfg.capacity_, id, cfg.align_size_);
    }
}

void BitmapBlkAllocator::copy_from(BitmapBlkAllocator const& src) {
    bm_->copy(*(src.bm_));
    alloced_blk_count_.store(static_cast< int64_t >(bm_->get_set_count()), std::memory_order_relaxed);
}

// ---- alloc ----

BlkAllocStatus BitmapBlkAllocator::alloc_contiguous(BlkId& bid) {
    blk_alloc_hints hints;
    hints.is_contiguous = true;

    BlkIds out_blkids;
    auto const status = alloc(1, hints, out_blkids);
    if (status == BlkAllocStatus::SUCCESS) {
        bid = out_blkids.front();
    }
    return status;
}

BlkAllocStatus BitmapBlkAllocator::alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkIds& out_blkids) {
    blk_count_t remain = nblks;

    for (auto& seg : seg_mgr_.segments()) {
        for (auto& portion_ptr : seg.portions_) {
            if (remain == 0)
                goto done;
            InmemPortion& portion = *portion_ptr;
            auto lock = portion.portion_lock();

            blk_num_t cursor = portion.start_blk_;
            while (cursor < portion.end_blk_ && remain > 0) {
                const blk_count_t want = hints.is_contiguous
                    ? nblks
                    : static_cast< blk_count_t >(std::min< uint32_t >(remain, hints.max_blks_per_piece));

                const blk_count_t min_needed =
                    hints.is_contiguous ? nblks : static_cast< blk_count_t >(hints.min_blks_per_piece);

                const auto bb =
                    bm_->get_next_contiguous_n_reset_bits(cursor, to_u64(portion.end_blk_), min_needed, want);
                if (bb.nbits == 0)
                    break;

                const blk_num_t start = static_cast< blk_num_t >(bb.start_bit);
                const blk_count_t got = static_cast< blk_count_t >(bb.nbits);
                bm_->set_bits(start, got);
                alloced_blk_count_.fetch_add(got, std::memory_order_relaxed);
                out_blkids.push_back(BlkId{start, got, chunk_id_});
                remain -= got;

                if (hints.is_contiguous)
                    goto done;
                cursor = start + got;
            }
        }
    }

done:
    if (remain == nblks)
        return BlkAllocStatus::SPACE_FULL;
    if (remain > 0) {
        if (!hints.partial_alloc_ok) {
            for (auto const& bid : out_blkids) {
                free(bid);
            }
            out_blkids.clear();
            return BlkAllocStatus::SPACE_FULL;
        }
        return BlkAllocStatus::PARTIAL;
    }
    return BlkAllocStatus::SUCCESS;
}

// ---- free ----

void BitmapBlkAllocator::free(BlkId const& bid) {
    InmemPortion& portion = seg_mgr_.blkid_to_portion(bid.blk_num());

    auto lock = portion.portion_lock();
    bm_->reset_bits(bid.blk_num(), bid.blk_count());
    alloced_blk_count_.fetch_sub(bid.blk_count(), std::memory_order_relaxed);
    if (inject_slab_on_free_) {
        portion.slab_cache_.free_blk(bid);
    }
}

// ---- commit ----

void BitmapBlkAllocator::do_set_bits(BlkId const& b) {
    InmemPortion& portion = seg_mgr_.blkid_to_portion(b.blk_num());
    auto lock = portion.portion_lock();
    bm_->set_bits(b.blk_num(), b.blk_count());
    alloced_blk_count_.fetch_add(b.blk_count(), std::memory_order_relaxed);
}

sisl::ThreadVector< BlkId >* BitmapBlkAllocator::get_commit_list() {
    return rcu_dereference(commit_list_);
}

BlkAllocStatus BitmapBlkAllocator::commit(BlkId const& bid) {
    rcu_read_lock();
    auto* list = get_commit_list();
    if (list) {
        // Buffer is currently acquired — defer the commit; release_buffer() will apply it.
        list->push_back(bid);
        rcu_read_unlock();
        return BlkAllocStatus::SUCCESS;
    }
    rcu_read_unlock();

    // No buffer held — set bits directly.
    do_set_bits(bid);
    return BlkAllocStatus::SUCCESS;
}

// ---- acquire / release buffer ----

BlkAllocator::BufferGuard BitmapBlkAllocator::acquire_buffer() {
    auto* new_list = new sisl::ThreadVector< BlkId >();
    auto* old_list = rcu_xchg_pointer(&commit_list_, new_list);
    synchronize_rcu();
    HS_REL_ASSERT_EQ(old_list, nullptr, "acquire_buffer called while buffer already acquired");
    return make_buffer_guard(bm_->serialize(align_size_), [this]() { do_release_buffer(); });
}

void BitmapBlkAllocator::do_release_buffer() {
    auto* old_list = rcu_xchg_pointer(&commit_list_, nullptr);
    synchronize_rcu();

    auto it = old_list->begin(true /* latest */);
    const BlkId* bid{nullptr};
    while ((bid = old_list->next(it)) != nullptr) {
        do_set_bits(*bid);
    }
    old_list->clear();
    delete old_list;
}

// ---- query ----

bool BitmapBlkAllocator::is_blk_alloced(BlkId const& b, bool use_lock) const {
    if (use_lock) {
        InmemPortion const& portion = seg_mgr_.blkid_to_portion(b.blk_num());
        auto lock = portion.portion_lock();
        return bm_->is_bits_set(b.blk_num(), b.blk_count());
    }
    return bm_->is_bits_set(b.blk_num(), b.blk_count());
}

blk_num_t BitmapBlkAllocator::available_blks() const {
    const auto used = alloced_blk_count_.load(std::memory_order_acquire);
    return (used >= 0 && static_cast< blk_num_t >(used) <= num_blks_) ? (num_blks_ - static_cast< blk_num_t >(used))
                                                                      : 0;
}

blk_num_t BitmapBlkAllocator::get_used_blks() const {
    const auto used = alloced_blk_count_.load(std::memory_order_acquire);
    return (used >= 0) ? static_cast< blk_num_t >(used) : 0;
}

std::string BitmapBlkAllocator::to_string() const {
    return fmt::format("BitmapBlkAllocator name={} num_blks={} used={} available={}", name_, num_blks_, get_used_blks(),
                       available_blks());
}

} // namespace homestore