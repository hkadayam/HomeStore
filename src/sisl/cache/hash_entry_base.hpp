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
#include <boost/intrusive/list.hpp>

using namespace boost::intrusive;

namespace sisl {

// ValueEntryBase is embedded in every cache entry (SingleEntryHashNode<V>).
//
// ── state_ layout (single std::atomic<uint64_t>, zero padding) ───────────────
//
//  bits  0-26 : size           (27 bits, max 128 MB per entry)
//  bits 27-29 : record_family  (3 bits,  up to 8 families — btree_nodes, blobs, …)
//  bit     30 : invalid        (1 bit)
//  bit     31 : CLOCK_BIT      lock-free; set on hot-queue reads, cleared by evictor
//  bit     32 : IN_HOT_QUEUE   set/cleared under partition lock
//  bit     33 : COLD_ACCESSED  lock-free; set on first cold read, triggers promotion
//  bits 34-63 : refcount       (30 bits, max ~1 billion live handles)
//
//  Everything fits in one 64-bit word — no struct padding, no separate fields.
//  Hot-path operations (set_clock_bit, acquire/release) touch a single cache line.
//
// ── member_hook_ ─────────────────────────────────────────────────────────────
//  Intrusive doubly-linked-list hook shared between the hot and cold eviction
//  queues.  An entry lives in exactly one queue at a time.
//
// ──────────────────────────────────────────────────────────────────────────────
class ValueEntryBase {
    // ── bit positions inside state_ ────────────────────────────────────────
    statuc constexpr uint64_t SIZE_BITS           = 27;
    static constexpr uint64_t SIZE_SHIFT          = 0;
    static constexpr uint64_t SIZE_MASK           = (1ULL << 27) - 1;   // bits 0-26

    static constexpr uint32_t RECORD_FAMILY_BITS  = 3; // up to 8 families
    static constexpr uint64_t FAMILY_MASK         = ((1ULL << RECORD_FAMILY_BITS) - 1) << SIZE_BITS; // bits 27-29

    static constexpr uint64_t INVALID_BIT         = 1ULL << (SIZE_BITS + RECORD_FAMILY_BITS + 0); // bit 30
    static constexpr uint64_t CLOCK_BIT           = 1ULL << (SIZE_BITS + RECORD_FAMILY_BITS + 1); // bit 31
    static constexpr uint64_t IN_HOT_QUEUE        = 1ULL << (SIZE_BITS + RECORD_FAMILY_BITS + 2); // bit 32
    static constexpr uint64_t COLD_ACCESSED       = 1ULL << (SIZE_BITS + RECORD_FAMILY_BITS + 3); // bit 33

    static constexpr uint64_t REFCOUNT_SHIFT      = SIZE_BITS + RECORD_FAMILY_BITS + 4; // bit 34
    static constexpr uint64_t REFCOUNT_ONE        = 1ULL << REFCOUNT_SHIFT;
    static constexpr uint64_t REFCOUNT_MASK       = ~((1ULL << REFCOUNT_SHIFT) - 1); // bits 34-63

public:
    // Intrusive list hook – shared by hot and cold eviction queues.
    mutable list_member_hook< link_mode< auto_unlink > > member_hook_;

    // Single atomic word: size | family | invalid | CLOCK | IN_HOT | COLD_ACC | refcount
    mutable std::atomic< uint64_t > state_{0};

public:
    ValueEntryBase() = default;
    ValueEntryBase(const ValueEntryBase&) = delete;
    ValueEntryBase& operator=(const ValueEntryBase&) = delete;

    ValueEntryBase(ValueEntryBase&& o) noexcept {
        state_.store(o.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        member_hook_.swap_nodes(o.member_hook_);
    }
    ValueEntryBase& operator=(ValueEntryBase&& o) noexcept {
        state_.store(o.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        member_hook_.swap_nodes(o.member_hook_);
        return *this;
    }

    // ── size / family / validity ────────────────────────────────────────────
    // These are written under the bucket write-lock (insert path) or partition
    // lock (evictor), so relaxed atomics are safe — the lock provides ordering.
    void set_size(uint32_t sz) {
        uint64_t s = state_.load(std::memory_order_relaxed);
        s = (s & ~SIZE_MASK) | (static_cast< uint64_t >(sz) & SIZE_MASK);
        state_.store(s, std::memory_order_relaxed);
    }
    uint32_t size() const {
        return static_cast< uint32_t >(state_.load(std::memory_order_relaxed) & SIZE_MASK);
    }

    void set_record_family(uint32_t fid) {
        uint64_t s = state_.load(std::memory_order_relaxed);
        s = (s & ~FAMILY_MASK) | ((static_cast< uint64_t >(fid) << FAMILY_SHIFT) & FAMILY_MASK);
        state_.store(s, std::memory_order_relaxed);
    }
    uint32_t record_family_id() const {
        return static_cast< uint32_t >((state_.load(std::memory_order_relaxed) & FAMILY_MASK)
                                       >> FAMILY_SHIFT);
    }

    void invalidate() {
        state_.fetch_or(INVALID_BIT, std::memory_order_relaxed);
    }
    bool is_invalidated() const {
        return (state_.load(std::memory_order_relaxed) & INVALID_BIT) != 0;
    }

    static constexpr size_t max_record_families() { return (1u << RECORD_FAMILY_BITS); }

    // ── handle-based eviction veto ──────────────────────────────────────────
    // acquire() is called while the bucket read-lock is held so the refcount
    // is non-zero before the lock is released, guaranteeing the evictor cannot
    // remove the entry between the lookup and the caller's first dereference.
    void acquire() { state_.fetch_add(REFCOUNT_ONE, std::memory_order_relaxed); }
    // release() is called from ~CacheHandle() with no lock held.
    void release() { state_.fetch_sub(REFCOUNT_ONE, std::memory_order_release); }
    // is_evictable() is checked by the background evictor under partition lock.
    bool is_evictable() const {
        return (state_.load(std::memory_order_acquire) & REFCOUNT_MASK) == 0;
    }

    // ── queue membership (set under partition lock) ─────────────────────────
    bool is_in_hot_queue() const {
        return (state_.load(std::memory_order_relaxed) & IN_HOT_QUEUE) != 0;
    }
    void mark_hot() {
        // Set IN_HOT_QUEUE, clear COLD_ACCESSED (entry is now hot — reset for future demotions).
        state_.fetch_or(IN_HOT_QUEUE, std::memory_order_relaxed);
        state_.fetch_and(~COLD_ACCESSED, std::memory_order_relaxed);
    }
    void mark_cold() {
        // Clear IN_HOT_QUEUE, COLD_ACCESSED, and CLOCK_BIT on cold entry/demotion.
        state_.fetch_and(~(IN_HOT_QUEUE | COLD_ACCESSED | CLOCK_BIT),
                          std::memory_order_relaxed);
    }

    // ── CLOCK bit (hot queue) — lock-free set, evictor-controlled clear ─────
    // Conditional write: skip the store when the bit is already set to avoid
    // cache-line invalidation on frequently-read (hot) entries.
    void set_clock_bit() {
        if (!(state_.load(std::memory_order_relaxed) & CLOCK_BIT))
            state_.fetch_or(CLOCK_BIT, std::memory_order_relaxed);
    }
    // Called by the background evictor under the partition lock.
    // Returns true → bit was set → entry gets a second chance; evictor clears
    // the bit and skips this entry for now.
    bool test_and_clear_clock_bit() {
        return (state_.fetch_and(~CLOCK_BIT, std::memory_order_acq_rel) & CLOCK_BIT) != 0;
    }

    // ── cold-accessed bit (lock-free) ───────────────────────────────────────
    // Called on every cold-queue read hit (no lock needed).
    // Returns true if the bit was ALREADY set, meaning this is the entry's
    // SECOND access while in cold → caller should promote it to the hot queue.
    bool test_and_set_cold_accessed() {
        return (state_.fetch_or(COLD_ACCESSED, std::memory_order_acq_rel) & COLD_ACCESSED) != 0;
    }
};

using CacheRecord = ValueEntryBase;

} // namespace sisl
