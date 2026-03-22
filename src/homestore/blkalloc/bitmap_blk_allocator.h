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

#include <sisl/fds/bitset.h>
#include <sisl/fds/thread_vector.h>
#include <urcu.h>

#include <homestore/homestore_decl.hpp>
#include <homestore/blk.h>
#include "blk_allocator.h"
#include "segment_manager.h"

namespace homestore {

///
/// BitmapBlkAllocator — Holds a sisl::Bitset over num_blks bits and uses a non-owning SegmentManager& for the
/// portion layout and per-portion mutexes.
///
/// alloc():           direct bitmap scan — finds free (reset) bits, sets them, returns BlkId.
/// free():            resets bits; if inject_slab_on_free_ is true also injects the freed range
///                    into the owning InmemPortion::slab_cache_.
/// commit():          sets bits — CP-safe: if acquire_buffer() is active, the bid is pushed to the
///                    pending commit list and applied by release_buffer().
/// scan_free_blks():  acquires the portion lock, scans bm_ for reset bits in the portion's range,
///                    sets them (marking as "in-cache"), and calls producer(bid) for each range found.
///                    Used by SlabBlkAllocator::fill_cache_for_portion().
/// copy_from():       bulk-copy a source bitmap into bm_; used to initialise inmem_bm_ from ondisk_bm_.
///
/// Thread-safety: each bitmap operation acquires the relevant InmemPortion::mtx_ via seg_mgr_.
///
class BitmapBlkAllocator : public BlkAllocator {
public:
    // buf: nullopt allocates a fresh zeroed bitset; a ByteArray deserializes from persisted bytes.
    BitmapBlkAllocator(BlkAllocConfig const& cfg, SegmentManager& seg_mgr, bool inject_slab_on_free,
                       chunk_num_t id, std::optional< sisl::ByteArray > buf = std::nullopt);
    BitmapBlkAllocator(BitmapBlkAllocator const&) = delete;
    BitmapBlkAllocator(BitmapBlkAllocator&&) noexcept = delete;
    BitmapBlkAllocator& operator=(BitmapBlkAllocator const&) = delete;
    BitmapBlkAllocator& operator=(BitmapBlkAllocator&&) noexcept = delete;
    ~BitmapBlkAllocator() override = default;

    BlkAllocStatus alloc_contiguous(BlkId& bid) override;
    BlkAllocStatus alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkId& out_blkid) override;
    void free(BlkId const& bid) override;

    // Sets bits in bm_. CP-safe via commit_list_ when a buffer is acquired.
    BlkAllocStatus commit(BlkId const& bid) override;

    // Serialize bm_; new commits accumulate in commit_list_ until the returned BufferGuard is destroyed.
    BufferGuard acquire_buffer() override;

    // Scan bm_ for free (reset) bits in portion's range, set them, call producer(bid) per range.
    // Acquires portion.mtx_ internally. Producer returns false to stop early.
    template < typename F >
    void scan_free_blks(InmemPortion& portion, F&& producer);

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
    sisl::ThreadVector< MultiBlkId >* get_commit_list();
    void do_set_bits(BlkId const& b);
    void do_release_buffer();

    unique< sisl::Bitset > bm_;
    SegmentManager& seg_mgr_;
    bool inject_slab_on_free_;
    // Non-null while acquire_buffer() is active; new commits are appended here.
    sisl::ThreadVector< MultiBlkId >* commit_list_{nullptr};
    std::atomic< int64_t > alloced_blk_count_{0};
};

// ---- template implementation ----

template < typename F >
void BitmapBlkAllocator::scan_free_blks(InmemPortion& portion, F&& producer) {
    auto lock = portion.portion_lock();

    blk_num_t cursor = portion.sweep_cursor_;
    bool wrapped = false;

    while (true) {
        const auto bb = bm_->get_next_contiguous_n_reset_bits(cursor, static_cast< uint64_t >(portion.end_blk_), 1u,
                                                              static_cast< uint32_t >(portion.end_blk_ - cursor));
        if (bb.nbits == 0) {
            if (wrapped || cursor == portion.start_blk_) break;
            // wrap around to the start of the portion for a second pass
            cursor = portion.start_blk_;
            wrapped = true;
            continue;
        }

        const blk_num_t start = static_cast< blk_num_t >(bb.start_bit);
        const blk_count_t count = static_cast< blk_count_t >(bb.nbits);

        bm_->set_bits(start, count);
        alloced_blk_count_.fetch_add(count, std::memory_order_relaxed);

        const bool keep_going = producer(BlkId{start, count, chunk_id_});
        cursor = start + count;

        if (!keep_going || cursor >= portion.end_blk_) break;
    }

    portion.sweep_cursor_ = cursor;
}

} // namespace homestore
