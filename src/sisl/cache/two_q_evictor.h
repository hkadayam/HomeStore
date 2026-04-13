/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Author/Developer(s): Harihara Kadayam
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
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/intrusive/list.hpp>
#include <sisl/cache/cache_node.h>

namespace sisl {

// ────────────────────────────────────────────────── TwoQEvictor ──────────────────────────────────────────────────────
//
// Eviction policy: 2Q with CLOCK approximation on the hot queue.
//
// Three logical structures per partition:
//
//   Cold queue (FIFO)
//     - New entries and entries demoted from hot land here.
//     - Reads while in cold set, the COLD_ACCESSED bit (lock-free atomic).
//     - On the SECOND cold read, the Cache layer promotes the entry to hot.
//     - On eviction from the cold, tail the Cache layer saves the key to its ghost list;
//       a ghost hit on the next insert causes direct hot insertion.
//
//   Hot queue (CLOCK approximation)
//     - Entries that were accessed at least twice while cold, or inserted directly via CacheHint::HOT (writepath
//       / mutations).
//     - Reads while in hot set the CLOCK_BIT (lock-free atomic).
//     - Reads set the CLOCK_BIT atomically (no lock, no list movement).
//     - The background evictor sweeps the hot queue with a persistent clock hand:
//       – CLOCK_BIT set   → clear bit, give second chance, advance hand
//       – CLOCK_BIT clear, not evictable (refcount > 0) → skip, advance hand
//       – CLOCK_BIT clear, evictable → demote to cold head
//     - The clock hand position is preserved across sweeps so every entry gets fair chance regardless of where it sits
//       in the list.
//
//   Background evictor thread
//     - Sleeps until total_size_ crosses high_watermark_.
//     - Wakes, sweeps hot → demotes to cold, evicts cold tail, until
//       total_size_ drops to low_watermark_.
//     - The cold queue is FIFO so "evict cold tail" is always O(1).
//
// Concurrency model
//   - Partition lock protects: hot_list, cold_list, clock_hand, hot_size, cold_size. flags_ (CLOCK_BIT, IN_HOT_QUEUE,
//     COLD_ACCESSED) are atomics → no lock on reads.
//   - refcount_ is atomic → no lock on acquire/release. total_size_ is a single atomic across all partitions.
//
class TwoQEvictor {
public:
    // Called when the evictor decides to evict an entry.
    using evict_fn_t = std::function< void(CacheRecord&) >;

    // Called specifically when a cold-queue entry is evicted (cache adds key to ghost list).
    using cold_evict_fn_t = std::function< void(CacheRecord&) >;

    struct Config {
        int64_t max_size;            // total cache capacity in bytes
        uint32_t num_partitions = 8; // sharding factor for concurrency
        float hot_pct = 0.80f;       // fraction of max_size for hot queue
        float high_wm_pct = 0.90f;   // wake evictor when size > this
        float low_wm_pct = 0.75f;    // evictor sleeps when size < this
    };

    explicit TwoQEvictor(Config const& cfg);
    ~TwoQEvictor();

    TwoQEvictor(const TwoQEvictor&) = delete;
    TwoQEvictor& operator=(const TwoQEvictor&) = delete;

    // Register/unregister a cache family.  Each Cache instance registers as a separate family so the evictor can
    // dispatch eviction callbacks to the right Cache based on CacheRecord::record_family_id().
    uint32_t register_family(evict_fn_t evict_fn, cold_evict_fn_t cold_evict_fn = nullptr);
    void unregister_family(uint32_t family_id);

    // ─────────────────────────────────────────── called by Cache<K,V> ────────────────────────────────────────────────
    //
    // The evictor partitions records purely for concurrency sharding; the partition is derived from the record's
    // address inside the evictor.  The cache layer must call record.set_size(...) before add_to_hot/add_to_cold so the
    // evictor can read the size from the record itself — no entry_size parameter is passed.

    // Register a newly inserted entry in the cold queue.  Caller must have set record.set_size(...) first.
    void add_to_cold(CacheRecord& record);

    // Register a newly inserted entry directly in the hot queue (write path or ghost hit).
    void add_to_hot(CacheRecord& record);

    // Promote an existing cold entry to the hot queue (called after COLD_ACCESSED bit indicates second access, or on
    // ghost hit at insert time).  Caller must ensure the entry is still in the cold queue (check is_in_hot_queue()).
    void promote_to_hot(CacheRecord& record);

    // Remove an entry that is being explicitly erased (no eviction callback fired).
    void remove_record(CacheRecord& record);

    // Stop the background eviction thread.  Idempotent.  Called by Cache::~Cache before tearing down the hashmap so
    // no callbacks fire mid-shutdown.  After stop() returns the evictor will not call evict_fn_ / cold_evict_fn_.
    void stop();

    // Drain all hot/cold lists, unlinking every record without calling any callbacks.
    void drain_all_lists();

    // Drain only records belonging to a specific family from all hot/cold lists.  Used by Cache::~Cache when the
    // evictor is shared — other families' records stay linked.
    void drain_family(uint32_t family_id);

    int64_t total_size() const { return total_size_.load(std::memory_order_relaxed); }
    uint32_t num_partitions() const { return to_u32(partitions_.size()); }

private:
    using EvictList = boost::intrusive::list<
        CacheRecord,
        boost::intrusive::member_hook<
            CacheRecord,
            boost::intrusive::list_member_hook< boost::intrusive::link_mode< boost::intrusive::auto_unlink > >,
            &CacheRecord::member_hook_ >,
        boost::intrusive::constant_time_size< false > >;

    struct Partition {
        EvictList hot_list;
        EvictList cold_list;
        EvictList::iterator clock_hand; // persistent CLOCK position in hot_list
        int64_t hot_size{0};
        int64_t cold_size{0};
        int64_t hot_max_size{0};
        std::mutex lock;

        Partition() : clock_hand{hot_list.end()} {}
        // non-copyable/movable because of the mutex and intrusive lists
        Partition(const Partition&) = delete;
        Partition& operator=(const Partition&) = delete;
    };

    // ────────────────────────────────────────────── background thread ────────────────────────────────────────────────
    void evictor_thread_fn();
    void evict_to_low_watermark();

    // Per-partition eviction helpers (called with partition lock held).
    // Demotes one hot entry whose CLOCK_BIT is clear to the cold queue.
    // Returns true if a candidate was found and demoted.
    bool clock_sweep_one(Partition& p);

    // Evicts the cold tail if it is evictable. Returns true on success.
    bool evict_cold_tail(Partition& p);

    // Partition selector — derives a stable partition from the record's address (allocator-managed addresses are well
    // distributed in the relevant bits, and the record's address is stable for its lifetime in the evictor).
    Partition& get_partition(CacheRecord const& record) {
        auto const bits = r_cast< std::uintptr_t >(&record) >> 6; // skip cache-line alignment bits
        return *partitions_[bits % partitions_.size()];
    }

    // Use unique_ptr so Partition objects are stable in memory (no moves after construction).
    std::vector< std::unique_ptr< Partition > > partitions_;

    std::atomic< int64_t > total_size_{0};
    int64_t high_watermark_;
    int64_t low_watermark_;

    struct Family {
        evict_fn_t evict_fn;
        cold_evict_fn_t cold_evict_fn;
        bool registered{false};
    };
    std::array< Family, CacheRecord::max_record_families() > families_;
    std::mutex families_mtx_;

    std::thread evictor_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::atomic< bool > running_{true};
};

} // namespace sisl
