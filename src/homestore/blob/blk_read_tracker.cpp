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
#include "blob/blk_read_tracker.h"
#include "common/homestore_assert.hpp"

namespace homestore {
static BlkId extract_key(const BlkTrackRecord& rec) {
    return rec.key;
}

BlkReadTracker::BlkReadTracker() : pending_reads_map_(kExpectedNumRecords, extract_key, nullptr /* access_cb */) {
}

void BlkReadTracker::merge(const BlkId& blkid, int64_t new_ref_count, const shared< BlkTrackWaiter >& waiter) {
    HS_DBG_ASSERT(new_ref_count ? waiter == nullptr : waiter != nullptr, "Invalid waiter");

    auto cur_base_blk_num = s_cast< blk_num_t >(sisl::round_down(blkid.blk_num(), entries_per_record()));
    auto last_base_blk_num =
        s_cast< blk_num_t >(sisl::round_down(blkid.blk_num() + blkid.blk_count() - 1, entries_per_record()));

    [[maybe_unused]] bool waiter_rescheduled{false};
    // everything is aligned after this point, so we don't need to handle sub_range in a base blkid;
    while (cur_base_blk_num <= last_base_blk_num) {
        BlkId base_blkid{cur_base_blk_num, entries_per_record(), blkid.chunk_num()};

        if (new_ref_count > 0) {
            // This is an insert operation
            pending_reads_map_.upsert_or_delete(base_blkid,
                                                [&base_blkid, new_ref_count](BlkTrackRecord& rec, bool existing) {
                                                    if (!existing) {
                                                        rec.key = base_blkid;
                                                    }
                                                    rec.ref_cnt += new_ref_count;
                                                    return false;
                                                });
        } else if (new_ref_count < 0) {
            // This is a remove operation
            pending_reads_map_.upsert_or_delete(
                base_blkid, [new_ref_count, &base_blkid](BlkTrackRecord& rec, bool existing) {
                    HS_DBG_ASSERT_EQ(existing, true, "Decrement a ref count (blk: {}) which does not exist in map",
                                     base_blkid.to_string());
                    rec.ref_cnt += new_ref_count;
                    return (rec.ref_cnt == 0);
                });
        } else {
            // this is wait_on operation
            pending_reads_map_.update(base_blkid, [&waiter_rescheduled, &waiter](BlkTrackRecord& rec) {
                rec.waiters.push_back(waiter);
                waiter_rescheduled = true;
            });
        }

        cur_base_blk_num += entries_per_record();
    }

    // if no record is found for a wait-on operation, it means no one is holding reference for this waiter and the
    // promise will be fulfilled automatically when this function exits (waiter's destructor will be called);
}

folly::SemiFuture< folly::Unit > BlkReadTracker::wait_on(const BlkId& bid) {
    auto [promise, future] = folly::makePromiseContract< folly::Unit >();
    auto waiter = std::make_shared< BlkTrackWaiter >(std::move(promise));
    merge(bid, 0, waiter);
    return std::move(future);
}

} // namespace homestore
