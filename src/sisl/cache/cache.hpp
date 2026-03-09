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

#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <sisl/cache/simple_hashmap.hpp>
#include <sisl/cache/two_q_evictor.hpp>

namespace sisl {

// ── CacheHint ─────────────────────────────────────────────────────────────────
// Controls which queue a newly inserted entry enters.
enum class CacheHint : uint8_t {
    // Pure read (scans, lookups): entry goes into the cold FIFO queue.
    // Scan-resistant: one-hit entries age out without polluting the hot queue.
    READ       = 0,
    // Write / mutation path: entry goes directly into the hot queue.
    // Mutations are pinned (handle held) until checkpoint flush; keeping them
    // in hot prevents them from blocking cold-queue eviction while pinned.
    READ_WRITE = 1,
};

// ── CacheHandle<V> ─────────────────────────────────────────────────────────────
// RAII handle returned by Cache::find() / Cache::insert().
//
// While a CacheHandle is alive:
//   • The entry's refcount is > 0 → the background evictor will not remove it.
//   • Dereferencing gives direct access to V inside the hashmap node (no copy).
//
// CacheHandle is move-only (not copyable) — matches the BtreeNode RAII model.
template < typename V >
class CacheHandle {
    template < typename K, typename VV >
    friend class Cache;

    ValueEntryBase* entry_{nullptr};
    V*              value_{nullptr};

    CacheHandle(ValueEntryBase* e, V* v) : entry_{e}, value_{v} {}

public:
    CacheHandle() = default;

    ~CacheHandle() {
        if (entry_) entry_->release();
    }

    CacheHandle(CacheHandle&& o) noexcept : entry_{o.entry_}, value_{o.value_} {
        o.entry_ = nullptr;
        o.value_ = nullptr;
    }
    CacheHandle& operator=(CacheHandle&& o) noexcept {
        if (entry_) entry_->release();
        entry_ = o.entry_;
        value_ = o.value_;
        o.entry_ = nullptr;
        o.value_ = nullptr;
        return *this;
    }

    CacheHandle(const CacheHandle&)            = delete;
    CacheHandle& operator=(const CacheHandle&) = delete;

    explicit operator bool() const { return entry_ != nullptr; }
    V&       operator*()           { return *value_; }
    V*       operator->()          { return value_; }
    const V& operator*()  const    { return *value_; }
    const V* operator->() const    { return value_; }
};

// ── Cache<K, V> ────────────────────────────────────────────────────────────────
//
// Unified cache with 2Q + CLOCK eviction and handle-based eviction veto.
//
// Key design points
// ─────────────────
//  • find() returns CacheHandle<V>.  The handle holds the refcount; the evictor
//    cannot remove the entry while the handle is alive.
//  • No thread-local state: no set_current_instance(), no accumulated TLS.
//    SimpleHashMap methods receive a BucketCtx on the stack for every call.
//  • Ghost list per evictor partition: tracks keys of recently cold-evicted
//    entries.  On insert, if the key is in the ghost list the entry bypasses
//    the cold queue and goes directly to hot.
//  • CacheHint::READ_WRITE bypasses the cold queue regardless of ghost status.
//
// Thread safety
// ─────────────
//  • Hashmap bucket lock (SharedMutex, per bucket): protects V storage.
//  • Evictor partition lock (mutex, per partition): protects queue membership.
//  • Ghost list lock (mutex, per evictor partition): protects the ghost map/list.
//  • ValueEntryBase::state_ is a single atomic<uint64_t>; all flag/refcount
//    operations are lock-free.
//
// Preconditions
// ─────────────
//  • remove() must only be called when no CacheHandles are alive for that key.
//    Calling remove() while a handle exists is undefined behaviour (the handle
//    holds a pointer into the deleted node).
//
template < typename K, typename V >
class Cache {
public:
    using evict_cb_t = std::function< void(const K&, V) >;

    struct Config {
        TwoQEvictor::Config evictor;
        uint32_t            num_buckets    = 64 * 1024; // hashmap buckets
        uint32_t            ghost_capacity = 4096;      // ghost entries per partition
        evict_cb_t          evict_cb;                   // optional — called on eviction
    };

    Cache(Config cfg, key_extractor_cb_t< K, V > key_extractor)
        : cfg_{std::move(cfg)},
          key_extract_{std::move(key_extractor)},
          map_{cfg_.num_buckets, key_extract_,
               nullptr /* access_cb not used; Cache drives everything */},
          evictor_{cfg_.evictor,
                   [this](CacheRecord& r) { on_evict(r); },
                   [this](CacheRecord& r) { on_cold_evict(r); }},
          ghost_lists_(cfg_.evictor.num_partitions,
                       GhostList{cfg_.ghost_capacity}) {}

    // ── find ────────────────────────────────────────────────────────────────
    // Returns a valid CacheHandle on hit, invalid handle on miss.
    // The entry's refcount is incremented atomically before the bucket lock is
    // released — no eviction window between lookup and first dereference.
    CacheHandle< V > find(const K& key, CacheHint hint = CacheHint::READ) {
        const size_t hash = SimpleHashMap< K, V >::compute_hash(key);
        auto [entry, val] = map_.find_and_acquire(hash, key);

        if (!entry) return {}; // miss

        if (entry->is_in_hot_queue()) {
            entry->set_clock_bit(); // lock-free, no list movement
        } else {
            if (entry->test_and_set_cold_accessed()) {
                // COLD_ACCESSED was already set → second access → promote
                evictor_.promote_to_hot(hash, *entry);
            }
            // first access: bit just set, entry stays in cold
        }

        return CacheHandle< V >{entry, val};
    }

    // ── insert ──────────────────────────────────────────────────────────────
    // Returns a valid CacheHandle on success, invalid handle if key exists.
    // The hint (READ / READ_WRITE) determines the initial queue.
    // Ghost-list hit overrides READ hint → direct hot insertion.
    CacheHandle< V > insert(const K& key, V value, CacheHint hint = CacheHint::READ) {
        const size_t hash = SimpleHashMap< K, V >::compute_hash(key);
        ValueEntryBase* entry = map_.insert_and_acquire(hash, key, value);

        if (!entry) return {}; // duplicate

        // Ghost hit or write path → hot queue; otherwise cold.
        const bool to_hot = (hint == CacheHint::READ_WRITE)
                         || ghost_lists_[hash % ghost_lists_.size()].check_and_remove(key);

        V* val = static_cast< SingleEntryHashNode< V >* >(entry)->value_ptr();
        if (to_hot) {
            evictor_.add_to_hot(hash, *entry, static_cast< uint32_t >(sizeof(V)));
        } else {
            evictor_.add_to_cold(hash, *entry, static_cast< uint32_t >(sizeof(V)));
        }

        return CacheHandle< V >{entry, val};
    }

    // ── remove ──────────────────────────────────────────────────────────────
    // Explicit removal.  evict_cb is NOT called.
    //
    // Precondition: no CacheHandle for this key must be alive when remove() is
    // called.  The caller must release all handles first.
    //
    // Implementation:
    //   1. find_and_acquire — bumps refcount so the background evictor cannot
    //      race with us and delete the node while we work.
    //   2. evictor_.remove_record — unlinks from hot/cold queue and decrements
    //      total_size_ while the node is still alive.
    //   3. entry->release() — drops our temporary hold (refcount back to 0;
    //      safe because the entry is no longer in any evictor queue).
    //   4. map_.erase — deletes the node; auto_unlink is a no-op (already
    //      unlinked by remove_record).
    bool remove(const K& key) {
        const size_t hash = SimpleHashMap< K, V >::compute_hash(key);

        auto [entry, val] = map_.find_and_acquire(hash, key);
        if (!entry) return false; // key not in cache

        // Unlink from evictor and fix total_size_ before the node is deleted.
        evictor_.remove_record(hash, *entry);

        // Drop the temporary refcount acquired above.  The entry is no longer
        // reachable by the evictor, so refcount==0 is safe here.
        entry->release();

        // Delete the node from the hashmap.  auto_unlink fires but the hook is
        // already detached, so it is a no-op.
        V out{};
        map_.erase(key, out);
        return true;
    }

    int64_t size() const { return evictor_.total_size(); }

private:
    // ── eviction callbacks (called by TwoQEvictor background thread) ─────────

    // on_evict: copy key/value out of the node, erase from hashmap (which
    // deletes the node), then notify the caller's evict_cb.
    void on_evict(CacheRecord& record) {
        auto& node  = static_cast< SingleEntryHashNode< V >& >(record);
        K     key   = key_extract_(node.value_);
        V     value = node.value_;

        V dummy{};
        map_.erase(key, dummy); // auto_unlink hook fires, node is deleted

        if (cfg_.evict_cb) cfg_.evict_cb(key, std::move(value));
    }

    // on_cold_evict: called before on_evict for cold-queue entries so the
    // ghost list can record the key for future insert promotion.
    void on_cold_evict(CacheRecord& record) {
        auto& node = static_cast< SingleEntryHashNode< V >& >(record);
        const K      key  = key_extract_(node.value_);
        const size_t hash = SimpleHashMap< K, V >::compute_hash(key);
        ghost_lists_[hash % ghost_lists_.size()].add(key);
    }

    // ── ghost list ────────────────────────────────────────────────────────────
    // Per evictor-partition ghost list: tracks recently cold-evicted keys.
    // O(1) lookup (unordered_map), O(1) add, bounded capacity (oldest dropped).
    struct GhostList {
        std::unordered_map< K, typename std::list< K >::iterator > map;
        std::list< K >  order; // front = oldest
        size_t          capacity;
        std::mutex      lock;

        explicit GhostList(size_t cap) : capacity{cap} {}
        GhostList(const GhostList&)            = delete;
        GhostList& operator=(const GhostList&) = delete;

        // Returns true and removes the key if present.
        bool check_and_remove(const K& key) {
            std::lock_guard lk(lock);
            auto it = map.find(key);
            if (it == map.end()) return false;
            order.erase(it->second);
            map.erase(it);
            return true;
        }

        // Adds key to ghost list, evicting the oldest entry if at capacity.
        void add(const K& key) {
            std::lock_guard lk(lock);
            if (map.size() >= capacity) {
                map.erase(order.front());
                order.pop_front();
            }
            order.push_back(key);
            map[key] = std::prev(order.end());
        }
    };

    Config                          cfg_;
    key_extractor_cb_t< K, V >      key_extract_;
    SimpleHashMap< K, V >           map_;
    TwoQEvictor                     evictor_;
    std::vector< GhostList >        ghost_lists_;
};

} // namespace sisl
