/*********************************************************************************
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
#include <cstdint>

#include <boost/intrusive/list.hpp>

#include "common/defs.h"
#include <sisl/cache/hashmap_traits.h>

namespace sisl {

// ──────────────────────────────────────────────── CacheRecord ──────────────────────────────────────────────────────
// Cache entry header.  Lives at the front of every Cache<K,V> entry's allocation; the user's V (or V-derived class)
// follows immediately after, in the same allocation, sized exactly to that V's actual sizeof.  TwoQEvictor operates
// on CacheRecord* so it doesn't need to know V.
//
// Layout (sizeof = 24, alignof = 8, no trailing padding):
//   offset  0-15  : member_hook_  (list_member_hook, 2 ptrs, 16B)
//   offset 16-19  : state_        (uint32_t, plain) — size + family, written once at insert
//   offset 20-23  : flags_        (atomic<uint32_t>) — refcount + queue/access bits
//
// state_ packs (32 bits exact, plain memory — no atomic needed because written only at insert):
//   bits  0-29 : size           (30 bits, max 1 GB per entry)
//   bits 30-31 : record_family  (2 bits, up to 4 caches sharing one evictor)
//
// flags_ packs (32 bits exact, atomic):
//   bits  0-27 : refcount       (28 bits, max ~256 M live handles)
//   bit     28 : HOT_BIT        (set iff entry is in the hot queue)
//   bit     29 : COLD_ACCESSED  (set on first cold-queue access; second access promotes to hot)
//   bit     30 : INVALID        (set on explicit invalidate)
//   bit     31 : CLOCK_BIT      (set on hot-queue access; cleared by evictor sweep)
//
// Refcount lives in the LOW 28 bits so acquire / release are simply fetch_add(1) / fetch_sub(1).  Flag bits live in
// the HIGH 4 bits and use fetch_or / fetch_and with single-bit constants — no shift, no compute at runtime.
//
// All RMWs (clock, cold-acc, hot, refcount) hit the same atomic word, so they serialise on the same cache line and
// never cross cache-line boundaries.
class CacheRecord {
    // state_ (plain, written once at insert)
    static constexpr uint32_t SIZE_BITS = 30;
    static constexpr uint32_t SIZE_MASK = (1u << SIZE_BITS) - 1; // bits 0-29
    static constexpr uint32_t FAMILY_SHIFT = SIZE_BITS;
    static constexpr uint32_t FAMILY_BITS = 2;
    static constexpr uint32_t FAMILY_MASK = ((1u << FAMILY_BITS) - 1) << FAMILY_SHIFT; // bits 30-31

    // flags_ (atomic)
    static constexpr uint32_t REFCOUNT_BITS = 28;
    static constexpr uint32_t REFCOUNT_MASK = (1u << REFCOUNT_BITS) - 1; // bits 0-27
    static constexpr uint32_t HOT_BIT = 1u << 28;                        // bit 28
    static constexpr uint32_t COLD_ACC_BIT = 1u << 29;                   // bit 29
    static constexpr uint32_t INVALID_BIT = 1u << 30;                    // bit 30
    static constexpr uint32_t CLOCK_BIT = 1u << 31;                      // bit 31

    // Pre-combined mask (compile-time) so mark_cold doesn't recompute on every call.
    static constexpr uint32_t MARK_COLD_CLEAR_MASK = ~(HOT_BIT | COLD_ACC_BIT | CLOCK_BIT);

public:
    // Intrusive list hook — shared by hot and cold eviction queues.
    mutable boost::intrusive::list_member_hook< boost::intrusive::link_mode< boost::intrusive::auto_unlink > >
        member_hook_;

    // size + family.  Set once at insert before the entry becomes visible to other threads → no atomic needed.
    uint32_t state_{0};

    // refcount + HOT + COLD_ACC + INVALID + CLOCK.  All RMWs hit this single word.
    mutable std::atomic< uint32_t > flags_{0};

    static constexpr size_t max_record_families() { return (1u << FAMILY_BITS); }

    CacheRecord() = default;
    CacheRecord(CacheRecord const&) = delete;
    CacheRecord& operator=(CacheRecord const&) = delete;

    // Movable: needed because Cache::insert constructs a temporary CacheNode<V> (which derives from CacheRecord) and
    // moves it into the hashmap slot.  The intrusive list hook supports swap-on-move; the atomics are loaded plainly
    // since there are no concurrent observers of the source object during the move.
    CacheRecord(CacheRecord&& o) noexcept : state_{o.state_} {
        flags_.store(o.flags_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        member_hook_.swap_nodes(o.member_hook_);
    }
    CacheRecord& operator=(CacheRecord&& o) noexcept {
        if (this != &o) {
            state_ = o.state_;
            flags_.store(o.flags_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            member_hook_.swap_nodes(o.member_hook_);
        }
        return *this;
    }

    // Refcount counts ONLY outstanding handles.  Map membership is NOT a reference.  is_unreferenced() returns true
    // iff no live handles exist (refcount field == 0).  Refcount lives in the low 28 bits so we fetch_add(1) directly.
    void acquire() { flags_.fetch_add(1, std::memory_order_relaxed); }
    void release() { flags_.fetch_sub(1, std::memory_order_release); }
    bool is_unreferenced() const { return (flags_.load(std::memory_order_acquire) & REFCOUNT_MASK) == 0; }

    void set_size(uint32_t sz) { state_ = (state_ & ~SIZE_MASK) | (sz & SIZE_MASK); }
    uint32_t size() const { return state_ & SIZE_MASK; }

    void set_record_family(uint32_t fid) { state_ = (state_ & ~FAMILY_MASK) | ((fid << FAMILY_SHIFT) & FAMILY_MASK); }
    uint32_t record_family_id() const { return (state_ & FAMILY_MASK) >> FAMILY_SHIFT; }

    void invalidate() { flags_.fetch_or(INVALID_BIT, std::memory_order_relaxed); }
    bool is_invalidated() const { return (flags_.load(std::memory_order_relaxed) & INVALID_BIT) != 0; }

    // ───────────────────────────────────────────── queue membership ──────────────────────────────────────────────────
    // mark_hot / mark_cold are called under the partition lock.  Two RMWs on the same atomic word are essentially as
    // cheap as one — they serialise on the same cache line and need no extra coherency traffic vs a single instruction.
    bool is_in_hot_queue() const { return (flags_.load(std::memory_order_relaxed) & HOT_BIT) != 0; }
    void mark_hot() {
        flags_.fetch_or(HOT_BIT, std::memory_order_relaxed);
        flags_.fetch_and(~COLD_ACC_BIT, std::memory_order_relaxed);
    }
    void mark_cold() { flags_.fetch_and(MARK_COLD_CLEAR_MASK, std::memory_order_relaxed); }

    // ─────────────────────────────────────── lock-free hot/cold access bits ──────────────────────────────────────────
    // set_clock_bit's relaxed pre-load skips the expensive RMW once the bit is already set — common case on hot
    // entries since CLOCK_BIT stays set until the next evictor sweep.
    void set_clock_bit() {
        if (!(flags_.load(std::memory_order_relaxed) & CLOCK_BIT)) {
            flags_.fetch_or(CLOCK_BIT, std::memory_order_relaxed);
        }
    }
    bool test_and_clear_clock_bit() {
        return (flags_.fetch_and(~CLOCK_BIT, std::memory_order_acq_rel) & CLOCK_BIT) != 0;
    }
    bool test_and_set_cold_accessed() {
        return (flags_.fetch_or(COLD_ACC_BIT, std::memory_order_acq_rel) & COLD_ACC_BIT) != 0;
    }
};

static_assert(sizeof(CacheRecord) == 24, "CacheRecord must be 24 bytes — layout assumption");

// ────────────────────────────────────────────────── CacheNode<V> ─────────────────────────────────────────────────────
// Stores the user value V.  Inherits CacheRecord which contributes 24 bytes at the front (member_hook + state +
// flags, no trailing padding).
//
// Layout for HashNode<CacheNode<V>>:
//   offset  0-7   : slist hook (HashNode)            8B
//   offset  8-23  : member_hook_                    16B (CacheRecord)
//   offset 24-27  : state_                           4B (CacheRecord)
//   offset 28-31  : flags_                           4B (CacheRecord)
//   offset 32+    : value_                           sizeof(V)
//
// Total non-V overhead = 32 bytes.
template < typename V >
class CacheNode : public CacheRecord {
public:
    V value_;

    CacheNode() = default;
    explicit CacheNode(V const& v) : value_(v) {}
    explicit CacheNode(V&& v) : value_(std::move(v)) {}

    // Non-copyable; movable via CacheRecord's move (swap-moves the intrusive hook) and V's move.
    CacheNode(CacheNode const&) = delete;
    CacheNode& operator=(CacheNode const&) = delete;
    CacheNode(CacheNode&&) noexcept = default;
    CacheNode& operator=(CacheNode&&) noexcept = default;
};

// HashmapTraits<CacheNode<V>> — tells SimpleHashMap that CacheNode<V> is refcounted.  Called by the hashmap on
// find/insert/Handle destruction and from erase_if_no_reference.
template < typename V >
struct HashmapTraits< CacheNode< V > > {
    static constexpr bool refcounted = true;
    static void acquire(CacheNode< V >& n) { n.acquire(); }
    static void release(CacheNode< V >& n) { n.release(); }
    static bool is_unreferenced(CacheNode< V > const& n) { return n.is_unreferenced(); }
};

// ────────────────────────────────────────────────── CacheTraits<V> ───────────────────────────────────────────────────
// Customisation point for Cache<K, V> users.  Distinct from HashmapTraits because size accounting is a Cache-only
// concern: a plain SimpleHashMap user (e.g. BlkReadTracker) has no capacity to bill against and shouldn't have to
// think about size.
//
// Default specialisation returns sizeof(V).  Specialise CacheTraits<V> when V owns variable-size buffers and the
// cache's capacity accounting must reflect the actual on-heap footprint instead of the wrapper struct size.
//
// Example: a btree node holding a 4-64 KB page should report n.node_size(), not sizeof(BtreeNode):
//
//     template <> struct sisl::CacheTraits< BtreeNode > {
//         static uint32_t size_of(BtreeNode const& n) { return n.node_size(); }
//     };
template < typename V >
struct CacheTraits {
    static uint32_t size_of(V const&) { return to_u32(sizeof(V)); }
};

} // namespace sisl
