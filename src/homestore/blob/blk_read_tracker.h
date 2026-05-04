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

#include <cstdint>
#include <memory>
#include <vector>

#include <folly/Unit.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/small_vector.h>
#include <sisl/cache/simple_hashmap.h>
#include <sisl/fds/utils.h>

#include <homestore/base/blk.h> // BlkId, blk_num_t, blk_count_t, chunk_num_t

namespace homestore {

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// BlkReadTracker
//
// Tracks concurrent reads on block ranges so that invalidate (free) can safely wait until all in-flight reads on the
// target blocks have completed.  Coroutine-friendly: wait_on() returns a SemiFuture<Unit> that can be co_awaited.
//
// Usage:
//   tracker.insert(bid);                           // before issuing a read
//   auto [ec, buf] = co_await vdev.read(buf, bid);
//   tracker.remove(bid);                           // after read completes
//
//   co_await tracker.wait_on(bid);                 // blocks until ref_cnt drops to zero for all aligned ranges
//   vdev.free_blk(bid);                            // now safe to free
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

// Waiter wraps a folly::Promise and fulfils it in its destructor.  Because multiple BlkTrackRecords may hold a
// shared_ptr<BlkTrackWaiter> (one per aligned range), the promise is fulfilled only when the last range releases its
// copy.
struct BlkTrackWaiter {
    folly::Promise< folly::Unit > promise;
    explicit BlkTrackWaiter(folly::Promise< folly::Unit > p) : promise{std::move(p)} {}
    ~BlkTrackWaiter() { promise.setValue(folly::unit); }
    BlkTrackWaiter(const BlkTrackWaiter&) = delete;
    BlkTrackWaiter& operator=(const BlkTrackWaiter&) = delete;
};

//
// clang-format off
//
//  A read can never overlap a unfinished free-blk id;
//  A read can overlap a pending read;
//
//  Say alignment is 16;
//  1. read-1: {17, 32, 0}
//  2. free blk: {8, 32, 0}
//  3. read-2: {0, 4, 0}  // <<< this read will also create track record on {0, 16, 0}, it is fine though as we don't
//  create any record under {0, 16, 0} on free;
//

//
// When ref_cnt drops to zero (read completes), remove every waiter from the vector in this Record;
// same waiter can be attached to multple BlkTrackRecord's vector and whom ever is the last to dereference the
// shared_ptr of a waiter, triggers waiter's destrctor which sends the callback;
//
//
//  Chunk_id: 0 (alignment: 16)
//   ---------------------------------------------------------------
//  | 1, 2, ... 15, 16 | 17, 18, ..., 31, 32 | 33, 34, ..., 47, 48 |  Blk Number (unique within same chunk)
//   ---------------------------------------------------------------
//     BlkTrackRecord-1    BlkTrackRecord-2             Record-1 and Record-2 could belongs to two different read (or one read);
//                                                      Record-1/2 could be referenced by multiple reads fall through on same base ids;
//      [ ],  [ ],  [ ]     [ ], [ ], [ ]               m_waiters: vector of shared_ptr
//       |     |      \     /     |    |
//       |     |       \   /      |    |
//       |     |         |        |    |
//      ( )   ( )       ( )      ( )  ( )                waiter's instance
//
//  clang-format on
//
struct BlkTrackRecord {
    BlkId key; // aligned key
    int64_t ref_cnt{0};
    folly::small_vector< shared< BlkTrackWaiter >, 8 > waiters; // multiple waiters can wait on same record
};

class BlkReadTracker {
    static constexpr uint32_t kExpectedNumRecords = 1000;
    static constexpr uint16_t kDefaultEntriesPerRecord = 8;

public:
    BlkReadTracker();
    ~BlkReadTracker() = default;

    BlkReadTracker(const BlkReadTracker&) = delete;
    BlkReadTracker& operator=(const BlkReadTracker&) = delete;
    BlkReadTracker(BlkReadTracker&&) = delete;
    BlkReadTracker& operator=(BlkReadTracker&&) = delete;

    uint16_t entries_per_record() const { return entries_per_record_; }
    void set_entries_per_record(uint16_t n) { entries_per_record_ = n; }

    /// Mark a block range as being read (increment ref count for every aligned range it touches).
    void insert(const BlkId& bid) { merge(bid, 1, nullptr); }

    /// Mark a read as complete (decrement ref count).  When a range's ref count drops to zero any waiters are
    /// notified and the record is removed.
    void remove(const BlkId& bid) { merge(bid, -1, nullptr); }

    /// Returns a SemiFuture that resolves when no in-flight reads overlap the given block range.  If there are no
    /// pending reads the future is already fulfilled (fast path — no heap allocation).  For blocks spanning multiple
    /// aligned ranges the future resolves only when ALL ranges are free.
    ///
    /// The Arc pattern: a single Promise is wrapped in shared_ptr<BlkTrackWaiter>.  Each aligned range's record
    /// holds one copy.  ~BlkTrackWaiter (which calls promise.setValue) fires only when the LAST shared_ptr is
    /// destroyed — i.e. when every range has released its ref.
    folly::SemiFuture< folly::Unit > wait_on(const BlkId& bid);

private:
    void merge(const BlkId& bid, int64_t ref_delta, const shared< BlkTrackWaiter >& waiter);

    sisl::SimpleHashMap< BlkId, BlkTrackRecord > pending_reads_map_;
    uint16_t entries_per_record_{kDefaultEntriesPerRecord};
};

} // namespace homestore
