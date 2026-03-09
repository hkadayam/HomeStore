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
#include <sisl/cache/hash_entry_base.hpp>

namespace sisl {

// ── TwoQEvictor ───────────────────────────────────────────────────────────────
//
// Eviction policy: 2Q with CLOCK approximation on the hot queue.
//
// Three logical structures per partition:
//
//   Cold queue (FIFO)
//     New entries and entries demoted from hot land here.
//     Reads while in cold set the COLD_ACCESSED bit (lock-free atomic).
//     On the SECOND cold read the Cache layer promotes the entry to hot.
//     On eviction from the cold tail the Cache layer saves the key to its
//     ghost list; a ghost hit on the next insert causes direct hot insertion.
//
//   Hot queue (CLOCK approximation)
//     Entries that were accessed at least twice while cold, or inserted
//     directly via CacheHint::READ_WRITE (write path / mutations).
//     Reads set the CLOCK_BIT atomically (no lock, no list movement).
//     The background evictor sweeps the hot queue with a persistent clock hand:
//       – CLOCK_BIT set   → clear bit, give second chance, advance hand
//       – CLOCK_BIT clear, not evictable (refcount > 0) → skip, advance hand
//       – CLOCK_BIT clear, evictable → demote to cold head
//     The clock hand position is preserved across sweeps so every entry gets
//     a fair chance regardless of where it sits in the list.
//
//   Background evictor thread
//     Sleeps until total_size_ crosses high_watermark_.
//     Wakes, sweeps hot → demotes to cold, evicts cold tail, until
//     total_size_ drops to low_watermark_.
//     The cold queue is FIFO so "evict cold tail" is always O(1).
//
// Concurrency model
//   Partition lock protects: hot_list, cold_list, clock_hand, hot_size, cold_size.
//   m_flags (CLOCK_BIT, IN_HOT_QUEUE, COLD_ACCESSED) are atomics → no lock on reads.
//   m_refcount is atomic → no lock on acquire/release.
//   total_size_ is a single atomic across all partitions.
//
// ──────────────────────────────────────────────────────────────────────────────
class TwoQEvictor {
public:
    // Called when the evictor decides to evict an entry (remove from hashmap).
    using evict_fn_t      = std::function< void(CacheRecord&) >;
    // Called specifically when a cold-queue entry is evicted (cache adds key to ghost list).
    using cold_evict_fn_t = std::function< void(CacheRecord&) >;

    struct Config {
        int64_t  max_size;                    // total cache capacity in bytes
        uint32_t num_partitions  = 8;         // sharding factor for concurrency
        float    hot_pct         = 0.80f;     // fraction of max_size for hot queue
        float    high_wm_pct     = 0.90f;     // wake evictor when size > this
        float    low_wm_pct      = 0.75f;     // evictor sleeps when size < this
    };

    TwoQEvictor(const Config& cfg, evict_fn_t evict_fn,
                cold_evict_fn_t cold_evict_fn = nullptr);
    ~TwoQEvictor();

    TwoQEvictor(const TwoQEvictor&)            = delete;
    TwoQEvictor& operator=(const TwoQEvictor&) = delete;

    // ── called by Cache<K,V> ─────────────────────────────────────────────────

    // Register a newly inserted entry in the cold queue.
    void add_to_cold(uint64_t hash_code, CacheRecord& record, uint32_t entry_size);

    // Register a newly inserted entry directly in the hot queue (write path or ghost hit).
    void add_to_hot(uint64_t hash_code, CacheRecord& record, uint32_t entry_size);

    // Promote an existing cold entry to the hot queue (called after COLD_ACCESSED
    // bit indicates second access, or on ghost hit at insert time).
    // Caller must ensure the entry is still in the cold queue (check is_in_hot_queue()).
    void promote_to_hot(uint64_t hash_code, CacheRecord& record);

    // Remove an entry that is being explicitly erased (no eviction callback fired).
    void remove_record(uint64_t hash_code, CacheRecord& record);

    // ── statistics ───────────────────────────────────────────────────────────
    int64_t total_size() const { return total_size_.load(std::memory_order_relaxed); }

private:
    using EvictList = boost::intrusive::list<
        CacheRecord,
        boost::intrusive::member_hook<
            CacheRecord,
            boost::intrusive::list_member_hook<
                boost::intrusive::link_mode< boost::intrusive::auto_unlink > >,
            &CacheRecord::m_member_hook >,
        boost::intrusive::constant_time_size< false > >;

    struct Partition {
        EvictList           hot_list;
        EvictList           cold_list;
        EvictList::iterator clock_hand;  // persistent CLOCK position in hot_list
        int64_t             hot_size{0};
        int64_t             cold_size{0};
        int64_t             hot_max_size{0};
        std::mutex          lock;

        Partition() : clock_hand{hot_list.end()} {}
        // non-copyable/movable because of the mutex and intrusive lists
        Partition(const Partition&) = delete;
        Partition& operator=(const Partition&) = delete;
    };

    // ── background thread ────────────────────────────────────────────────────
    void evictor_thread_fn();
    void evict_to_low_watermark();

    // Per-partition eviction helpers (called with partition lock held).
    // Demotes one hot entry whose CLOCK_BIT is clear to the cold queue.
    // Returns true if a candidate was found and demoted.
    bool clock_sweep_one(Partition& p);

    // Evicts the cold tail if it is evictable. Returns true on success.
    bool evict_cold_tail(Partition& p);

    Partition& get_partition(uint64_t hash_code) {
        return *partitions_[hash_code % partitions_.size()];
    }

    // Use unique_ptr so Partition objects are stable in memory (no moves after construction).
    std::vector< std::unique_ptr< Partition > > partitions_;

    std::atomic< int64_t >  total_size_{0};
    int64_t                 high_watermark_;
    int64_t                 low_watermark_;

    evict_fn_t              evict_fn_;
    cold_evict_fn_t         cold_evict_fn_;

    std::thread             evictor_thread_;
    std::mutex              cv_mutex_;
    std::condition_variable cv_;
    std::atomic< bool >     running_{true};
};

} // namespace sisl
