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

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "sisl/fds/bitset.h"
#include "sisl/fds/thread_vector.h"
#include "sisl/fds/rcu.h"

#include "homestore/base/homestore_decl.h"
#include "homestore/base/blk.h"
#include "blk_allocator.h"
#include "segment_manager.h"

namespace homestore {
namespace blkalloc {

///
/// BitmapBlkAllocator — Holds a sisl::Bitset over num_blks bits and uses a non-owning SegmentManager& for the
/// portion layout and per-portion mutexes.
///
/// alloc():           direct bitmap scan — finds free (reset) bits, sets them, returns BlkId.
/// free():            resets bits in the bitmap (portion-locked).
/// commit():          sets bits — CP-safe: if acquire_buffer() is active, the bid is pushed to the pending commit
///                    list and applied by release_buffer().
/// scan_free_blks():  acquires the portion lock, scans bm_ for reset bits in the portion's range, and calls
///                    consumer(bid) for each contiguous free range found. Consumer returns
///                    {keep_on_going, num_consumed}. num_consumed > 0 sets those bits in the bitmap;
///                    num_consumed == 0 leaves the bitmap untouched. keep_on_going == false stops the scan.
/// copy_from():       bulk-copy a source bitmap into bm_; used to initialise inmem_bm_ from ondisk_bm_.
///
/// Thread-safety: each bitmap operation acquires the relevant InmemPortion::mtx_ via seg_mgr_.
///
class BitmapBlkAllocator : public BlkAllocator {
public:
    // buf: nullopt allocates a fresh zeroed bitset; a IoBufShared deserializes from persisted bytes.
    BitmapBlkAllocator(BlkAllocConfig const& cfg, SegmentManager& seg_mgr, chunk_num_t id,
                       std::optional< sisl::IoBufShared > buf = std::nullopt);
    BitmapBlkAllocator(BitmapBlkAllocator const&) = delete;
    BitmapBlkAllocator(BitmapBlkAllocator&&) noexcept = delete;
    BitmapBlkAllocator& operator=(BitmapBlkAllocator const&) = delete;
    BitmapBlkAllocator& operator=(BitmapBlkAllocator&&) noexcept = delete;
    ~BitmapBlkAllocator() override = default;

    BlkAllocStatus alloc_contiguous(BlkId& bid) override;
    BlkAllocStatus alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkIds& out_blkids) override;
    void free(BlkId const& bid) override;

    // Sets bits in bm_. CP-safe via commit_list_ when a buffer is acquired.
    BlkAllocStatus commit(BlkId const& bid) override;

    // Serialize bm_; new commits accumulate in commit_list_ until the returned BufferGuard is destroyed.
    BufferGuard acquire_buffer() override;

    // Scan bm_ for free (reset) bits in portion's range. Calls consumer(bid) for each contiguous
    // free range; consumer returns std::pair<bool, blk_count_t>{keep_on_going, num_consumed}.
    //   num_consumed > 0 → those bits are SET in the bitmap (marked as in-cache).
    //   num_consumed == 0 → bitmap is untouched for this range.
    //   keep_on_going == false → scan stops after this iteration.
    // Acquires portion.mtx_ internally.
    template < typename F >
    void scan_free_blks(InmemPortion& portion, F&& consumer);

    // Copy all bits from src.bm_ into bm_ (used for load-time initialisation).
    void copy_from(BitmapBlkAllocator const& src);

    bool is_blk_alloced(BlkId const& b, bool use_lock = false) const override;
    blk_num_t available_blks() const override;
    blk_num_t get_used_blks() const override;
    void recovery_completed() override {}
    void reset() override {}
    std::string to_string() const override;
    nlohmann::json get_status(int) const override { return {}; }

private:
    sisl::ThreadVector< BlkId >* get_commit_list();
    void do_set_bits(BlkId const& b);
    void do_release_buffer();

    unique< sisl::Bitset > bm_;
    SegmentManager& seg_mgr_;
    // Non-null while acquire_buffer() is active; new commits are appended here.
    sisl::ThreadVector< BlkId >* commit_list_{nullptr};
    std::atomic< int64_t > alloced_blk_count_{0};
};

// ---- template implementation ----

template < typename F >
void BitmapBlkAllocator::scan_free_blks(InmemPortion& portion, F&& consumer) {
    auto lock = portion.portion_lock();

    blk_num_t cursor = portion.sweep_cursor_;
    bool wrapped = false;

    // Cap the contiguous range to the largest slab size so try_free can decompose without overflow.
    static constexpr uint32_t max_slab_blks = 1u << (SlabCache::NUM_SLABS - 1);

    while (true) {
        const auto bb = bm_->get_next_contiguous_n_reset_bits(
            cursor, to_u64(portion.end_blk_), 1u, max_slab_blks);
        if (bb.nbits == 0) {
            if (wrapped || cursor == portion.start_blk_) break;
            // wrap around to the start of the portion for a second pass
            cursor = portion.start_blk_;
            wrapped = true;
            continue;
        }

        const blk_num_t start = static_cast< blk_num_t >(bb.start_bit);
        const blk_count_t count = static_cast< blk_count_t >(bb.nbits);

        auto [keep_on_going, num_consumed] = consumer(BlkId{start, count, chunk_id_});
        if (num_consumed > 0) {
            bm_->set_bits(start, num_consumed);
            alloced_blk_count_.fetch_add(num_consumed, std::memory_order_relaxed);
        }
        cursor = start + count;

        if (!keep_on_going || cursor >= portion.end_blk_) break;
    }

    portion.sweep_cursor_ = cursor;
}

} // namespace blkalloc
} // namespace homestore
