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

// Include logging first so that fmt/spdlog headers are parsed before boost::intrusive's 'compare' template is brought
// into scope.
#include <sisl/logging/logging.h>

#include "common/defs.h"
#include <sisl/cache/two_q_evictor.h>

namespace sisl {

// ── construction / destruction ─────────────────────────────────────────────────

TwoQEvictor::TwoQEvictor(Config const& cfg) :
        high_watermark_{to_i64(cfg.max_size * cfg.high_wm_pct)}, low_watermark_{to_i64(cfg.max_size * cfg.low_wm_pct)} {

    const int64_t hot_max = to_i64(cfg.max_size * cfg.hot_pct / cfg.num_partitions);
    partitions_.reserve(cfg.num_partitions);
    for (uint32_t i = 0; i < cfg.num_partitions; ++i) {
        auto p = std::make_unique< Partition >();
        p->hot_max_size = hot_max;
        partitions_.push_back(std::move(p));
    }

    evictor_thread_ = std::thread([this] { evictor_thread_fn(); });
}

TwoQEvictor::~TwoQEvictor() {
    stop();
}

uint32_t TwoQEvictor::register_family(evict_fn_t evict_fn, cold_evict_fn_t cold_evict_fn) {
    std::lock_guard lk{families_mtx_};
    for (uint32_t i = 0; i < families_.size(); ++i) {
        if (!families_[i].registered) {
            families_[i] = Family{std::move(evict_fn), std::move(cold_evict_fn), true};
            return i;
        }
    }
    RELEASE_ASSERT(false, "TwoQEvictor: all {} family slots are in use", families_.size());
    return 0;
}

void TwoQEvictor::unregister_family(uint32_t family_id) {
    std::lock_guard lk{families_mtx_};
    families_[family_id] = Family{};
}

void TwoQEvictor::stop() {
    bool was_running = false;
    {
        std::lock_guard lk(cv_mutex_);
        was_running = running_.exchange(false, std::memory_order_release);
    }
    if (!was_running)
        return; // already stopped — idempotent
    cv_.notify_all();
    if (evictor_thread_.joinable())
        evictor_thread_.join();
}

void TwoQEvictor::drain_all_lists() {
    // Walk every partition and unlink every record from its hot/cold list, without invoking any callbacks.  After
    // this call, no record's intrusive hook is linked, so subsequent destruction of the records (by the hashmap) is
    // safe — auto_unlink finds nothing to splice through.
    for (auto& p_uptr : partitions_) {
        auto& p = *p_uptr;
        std::lock_guard lk(p.lock);
        // Use erase + iterator increment; auto_unlink hooks don't auto-remove on list destruction.
        while (!p.hot_list.empty()) {
            CacheRecord& r = p.hot_list.front();
            p.hot_list.pop_front();
            // After pop_front, the hook is still in 'safe' state but unlinked from this list.  In auto_unlink mode
            // pop_front detaches the hook automatically — nothing more to do.
            (void)r;
        }
        while (!p.cold_list.empty()) {
            p.cold_list.pop_front();
        }
        p.hot_size = 0;
        p.cold_size = 0;
    }
    total_size_.store(0, std::memory_order_relaxed);
}

void TwoQEvictor::drain_family(uint32_t family_id) {
    for (auto& p_uptr : partitions_) {
        auto& p = *p_uptr;
        std::lock_guard lk(p.lock);

        auto unlink_family = [&](EvictList& list, int64_t& list_size) {
            auto it = list.begin();
            while (it != list.end()) {
                if (it->record_family_id() == family_id) {
                    auto const sz = it->size();
                    it = list.erase(it);
                    list_size -= sz;
                    total_size_.fetch_sub(sz, std::memory_order_relaxed);
                } else {
                    ++it;
                }
            }
        };

        unlink_family(p.hot_list, p.hot_size);
        unlink_family(p.cold_list, p.cold_size);
    }
}

// ── public API ─────────────────────────────────────────────────────────────────
// Caller (Cache layer) must call record.set_size(...) before add_to_cold/add_to_hot.  The evictor reads the size from
// the record itself for accounting and partitions records by their address (no hash_code passed).

void TwoQEvictor::add_to_cold(CacheRecord& record) {
    int64_t const sz = record.size();
    record.mark_cold();

    Partition& p = get_partition(record);
    {
        std::lock_guard lk(p.lock);
        p.cold_list.push_front(record); // newest at front, oldest at back
        p.cold_size += sz;
    }

    total_size_.fetch_add(sz, std::memory_order_relaxed);
    if (total_size_.load(std::memory_order_relaxed) >= high_watermark_)
        cv_.notify_one();
}

void TwoQEvictor::add_to_hot(CacheRecord& record) {
    int64_t const sz = record.size();
    record.mark_hot();

    Partition& p = get_partition(record);
    {
        std::lock_guard lk(p.lock);
        // If the clock hand is at end (list was empty), reset after push so it points to the first real entry — the
        // evictor never starts blind.
        bool reset_hand = p.hot_list.empty();
        p.hot_list.push_front(record); // newest at front
        p.hot_size += sz;
        if (reset_hand)
            p.clock_hand = p.hot_list.begin();
    }

    total_size_.fetch_add(sz, std::memory_order_relaxed);
    if (total_size_.load(std::memory_order_relaxed) >= high_watermark_)
        cv_.notify_one();
}

void TwoQEvictor::promote_to_hot(CacheRecord& record) {
    Partition& p = get_partition(record);
    std::lock_guard lk(p.lock);

    // Guard against double-promotion (another thread may have raced us).
    if (record.is_in_hot_queue())
        return;
    // Guard against the entry being evicted while we waited for the lock.
    if (!record.member_hook_.is_linked())
        return;

    const int64_t sz = record.size();

    // Advance clock hand past this node before unlinking it to keep the hand valid.
    if (p.clock_hand != p.cold_list.end() && &*p.clock_hand == &record)
        ++p.clock_hand;

    p.cold_list.erase(p.cold_list.iterator_to(record));
    p.cold_size -= sz;

    record.mark_hot();
    bool reset_hand = p.hot_list.empty();
    p.hot_list.push_front(record);
    p.hot_size += sz;
    if (reset_hand)
        p.clock_hand = p.hot_list.begin();
}

void TwoQEvictor::remove_record(CacheRecord& record) {
    Partition& p = get_partition(record);
    std::lock_guard lk(p.lock);

    if (!record.member_hook_.is_linked())
        return;

    const int64_t sz = record.size();

    if (record.is_in_hot_queue()) {
        // Advance clock hand before unlinking so it remains valid.
        auto it = p.hot_list.iterator_to(record);
        if (p.clock_hand == it)
            ++p.clock_hand;
        p.hot_list.erase(it);
        p.hot_size -= sz;
    } else {
        p.cold_list.erase(p.cold_list.iterator_to(record));
        p.cold_size -= sz;
    }

    total_size_.fetch_sub(sz, std::memory_order_relaxed);
}

// ── background evictor thread ──────────────────────────────────────────────────

void TwoQEvictor::evictor_thread_fn() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lk(cv_mutex_);
            cv_.wait(lk, [this] {
                return !running_.load(std::memory_order_acquire) ||
                    total_size_.load(std::memory_order_relaxed) >= high_watermark_;
            });
        }
        if (!running_.load(std::memory_order_acquire))
            break;
        evict_to_low_watermark();
    }
}

void TwoQEvictor::evict_to_low_watermark() {
    // Round-robin across partitions so one hot partition can't starve others.
    const size_t n = partitions_.size();
    size_t attempts = 0;
    const size_t max_attempts = n * 4; // avoid spinning forever if all pinned

    while (total_size_.load(std::memory_order_relaxed) > low_watermark_ && attempts < max_attempts) {
        bool progress = false;
        for (size_t i = 0; i < n; ++i) {
            if (total_size_.load(std::memory_order_relaxed) <= low_watermark_)
                break;
            Partition& p = *partitions_[i];
            std::lock_guard lk(p.lock);

            // Step 1: try to evict directly from the cold tail (O(1)).
            if (evict_cold_tail(p)) {
                progress = true;
                continue;
            }

            // Step 2: cold is empty or its tail is pinned — sweep hot via CLOCK.
            if (clock_sweep_one(p)) {
                progress = true;
            }
        }
        if (!progress)
            ++attempts;
        else
            attempts = 0;
    }

    if (attempts >= max_attempts) {
        LOGWARNMOD(cache,
                   "TwoQEvictor: could not reach low watermark — "
                   "all candidates are pinned (size={})",
                   total_size());
    }
}

// ── per-partition helpers (called with partition lock held) ────────────────────

bool TwoQEvictor::evict_cold_tail(Partition& p) {
    if (p.cold_list.empty())
        return false;

    CacheRecord& victim = p.cold_list.back(); // oldest cold entry
    if (!victim.is_unreferenced())
        return false; // handle held — skip

    const int64_t sz = victim.size();

    // Notify Cache layer: cold-evict callback first (adds key to ghost list),
    // then generic evict callback (erases from hashmap).
    auto const& fam = families_[victim.record_family_id()];
    if (fam.cold_evict_fn)
        fam.cold_evict_fn(victim);
    if (fam.evict_fn)
        fam.evict_fn(victim);

    // auto_unlink removes from cold_list when the node is deleted in evict_fn_,
    // but adjust accounting here while we hold the lock.
    p.cold_size -= sz;
    total_size_.fetch_sub(sz, std::memory_order_relaxed);
    return true;
}

bool TwoQEvictor::clock_sweep_one(Partition& p) {
    if (p.hot_list.empty())
        return false;

    // Wrap-around guard: at most one full revolution before giving up.
    const size_t limit = p.hot_list.size() + 1;
    for (size_t scanned = 0; scanned < limit; ++scanned) {
        if (p.clock_hand == p.hot_list.end())
            p.clock_hand = p.hot_list.begin();
        if (p.clock_hand == p.hot_list.end())
            break; // list is empty

        CacheRecord& candidate = *p.clock_hand;
        ++p.clock_hand; // advance BEFORE any possible removal

        if (candidate.test_and_clear_clock_bit()) {
            // Bit was set → second chance; clear and skip.
            continue;
        }
        if (!candidate.is_unreferenced()) {
            // Handle held → skip (it will be re-visited next sweep).
            continue;
        }

        // Demote to cold head rather than evict directly — gives one more
        // cold-FIFO lifetime before eviction.
        const int64_t sz = candidate.size();
        p.hot_list.erase(p.hot_list.iterator_to(candidate));
        p.hot_size -= sz;

        candidate.mark_cold();
        p.cold_list.push_front(candidate);
        p.cold_size += sz;
        return true;
    }
    return false; // nothing to demote this sweep
}

} // namespace sisl
