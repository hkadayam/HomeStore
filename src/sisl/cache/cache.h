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
#include <utility>
#include <vector>

#include "common/defs.h"
#include "sisl/cache/cache_node.h"
#include "sisl/cache/simple_hashmap.h"
#include "sisl/cache/two_q_evictor.h"

namespace sisl {

// ─────────────────────────────────────────────────── CacheHint ───────────────────────────────────────────────────────
// Controls which queue a newly inserted entry enters.
enum class CacheHint : uint8_t {
    // Read-miss / scan / lookup: entry goes into the cold FIFO queue (scan-resistant via 2Q).
    COLD = 0,
    // Write / mutation: entry goes directly into the hot queue (caller knows it'll be touched again).
    HOT = 1,
};

// ───────────────────────────────────────────────── CacheHandle<V> ────────────────────────────────────────────────────
// User-facing RAII handle.  Internally wraps a Handle<CacheNode<V>> and exposes V* / V& directly so callers never see
// the CacheNode wrapper.
template < typename V >
class CacheHandle {
    Handle< CacheNode< V > > inner_;

public:
    CacheHandle() = default;
    explicit CacheHandle(Handle< CacheNode< V > >&& h) : inner_{std::move(h)} {}

    CacheHandle(CacheHandle&&) noexcept = default;
    CacheHandle& operator=(CacheHandle&&) noexcept = default;
    CacheHandle(CacheHandle const&) = delete;
    CacheHandle& operator=(CacheHandle const&) = delete;

    explicit operator bool() const { return bool(inner_); }

    // Primary accessor — returns a reference to the stored user value V.
    V& value() { return inner_->value_; }
    V const& value() const { return inner_->value_; }

    // Pointer-style sugar.
    V* operator->() { return &inner_->value_; }
    V const* operator->() const { return &inner_->value_; }
};

// ────────────────────────────────────────────────── Cache<K, V> ──────────────────────────────────────────────────────
// Unified cache with 2Q + CLOCK eviction and handle-based eviction veto.  Stores CacheNode<V> in a SimpleHashMap; the
// hashmap drives refcount via HashmapTraits<CacheNode<V>>.  TwoQEvictor sees only CacheRecord.
//
// V is moved/copied into the cache slot at insert time.  For non-movable values (e.g. types containing std::mutex),
// or for types whose actual subclass varies in size, use Cache<K, unique<V>> or Cache<K, shared<V>> and pass a smart
// pointer to a heap-allocated V.  CacheTraits<V> can be specialised to bill the actual on-heap footprint.
template < typename K, typename V >
class Cache {
public:
    using key_extractor_t = std::function< K(V const&) >;

    struct Config {
        uint32_t num_buckets = 64 * 1024; // hashmap buckets
        uint32_t ghost_capacity = 4096;   // ghost entries per partition
    };

    Cache(Config cfg, shared< TwoQEvictor > evictor, key_extractor_t key_extractor) :
            cfg_{std::move(cfg)},
            key_extract_{std::move(key_extractor)},
            evictor_{std::move(evictor)},
            map_{cfg_.num_buckets,
                 [extract = key_extract_](CacheNode< V > const& cn) { return extract(cn.value_); }} {
        family_id_ = evictor_->register_family(
            [this](CacheRecord& record) { on_evict(record); },
            [this](CacheRecord& record) { on_cold_evict(record); });

        ghost_lists_.reserve(evictor_->num_partitions());
        for (uint32_t i = 0; i < evictor_->num_partitions(); ++i) {
            ghost_lists_.push_back(std::make_unique< GhostList >(cfg_.ghost_capacity));
        }
    }

    // Returns a valid CacheHandle on hit, invalid on miss.
    CacheHandle< V > find(K const& key) {
        auto h = map_.find(key);
        if (!h)
            return CacheHandle< V >{};

        CacheRecord& record = h.value();
        if (record.is_in_hot_queue()) {
            record.set_clock_bit();
        } else {
            if (record.test_and_set_cold_accessed()) {
                // Second cold access → promote to hot.
                evictor_->promote_to_hot(record);
            }
        }
        return CacheHandle< V >{std::move(h)};
    }

    // Insert a value into the cache.  V is moved into the cache slot.  Returns a valid CacheHandle on success;
    // invalid handle if the key already exists.
    template < typename Vin >
    CacheHandle< V > insert(K const& key, Vin&& value, CacheHint hint = CacheHint::COLD) {
        auto h = map_.insert(key, CacheNode< V >{std::forward< Vin >(value)});
        if (!h)
            return CacheHandle< V >{};

        CacheRecord& record = h.value();

        // Bill the cache for the value's actual footprint via CacheTraits<V>::size_of.  Defaults to sizeof(V); when V
        // is unique<T> / shared<T>, specialise CacheTraits<V> to return T's actual on-heap size.
        record.set_size(CacheTraits< V >::size_of(h.value().value_));
        record.set_record_family(family_id_);

        // Ghost hit or write path → hot queue; otherwise cold.
        size_t const ghost_idx = ghost_index_for(key);
        bool const to_hot = (hint == CacheHint::HOT) || ghost_lists_[ghost_idx]->check_and_remove(key);

        if (to_hot) {
            evictor_->add_to_hot(record);
        } else {
            evictor_->add_to_cold(record);
        }
        return CacheHandle< V >{std::move(h)};
    }

    // Explicit removal.  Caller must ensure no live CacheHandle for this key.
    bool remove(K const& key) {
        auto h = map_.find(key);
        if (!h)
            return false;

        CacheRecord& record = h.value();
        evictor_->remove_record(record);

        h = {}; // releases the handle's ref → refcount back to 0
        return map_.erase_if_no_reference(key);
    }

    int64_t size() const { return evictor_->total_size(); }

    // Explicit destruction order: stop the evictor thread (so it can't call back into us mid-shutdown), then walk the
    // hashmap and explicitly unlink each entry from the evictor lists before any auto_unlink hooks fire during the
    // hashmap's own destructor (which would try to splice through the already-destroyed evictor list heads).
    ~Cache() {
        // Unregister our family so the evictor won't call back into us after we're destroyed.  The evictor is shared
        // and may outlive this Cache instance.
        evictor_->unregister_family(family_id_);

        // Drain this cache's entries from the evictor queues.  We can't drain ALL lists (other caches may be using
        // them), so we drain our own family's entries only.
        evictor_->drain_family(family_id_);
    }

private:
    // ────────────────────────────── eviction callbacks (TwoQEvictor background thread) ───────────────────────────────

    void on_evict(CacheRecord& record) {
        auto& cnode = s_cast< CacheNode< V >& >(record);
        K key = key_extract_(cnode.value_);
        // erase_if_no_reference is the synchronisation point: a concurrent find() may have just bumped the refcount,
        // and the bucket lock + atomic refcount check resolves that race.
        map_.erase_if_no_reference(key);
    }

    void on_cold_evict(CacheRecord& record) {
        auto& cnode = s_cast< CacheNode< V >& >(record);
        K key = key_extract_(cnode.value_);
        ghost_lists_[ghost_index_for(key)]->add(key);
    }

    // Pick the ghost-list shard for a key.  Independent from the evictor's address-based partitioning.
    size_t ghost_index_for(K const& key) const {
        return SimpleHashMap< K, CacheNode< V > >::compute_hash(key) % ghost_lists_.size();
    }

    // ─────────────────────────────────────────────── GhostList ─────────────────────────────────────────────────────
    //
    // Ghost list = bounded FIFO of keys that were RECENTLY EVICTED from the cold queue.  Its purpose is to absorb the
    // common scan-then-rescan access pattern: an entry that was scanned once, evicted (because it lived briefly in
    // the cold queue and never got promoted), and is now being inserted again — that re-insertion almost certainly
    // comes from a real working-set hit, not a one-shot scan.  By remembering the key for a while after eviction, the
    // cache can detect this pattern: if an insert sees the key in the ghost list, the entry skips the cold queue
    // entirely and goes straight to the hot queue.  This is the "ghost cache" trick from the 2Q / ARC paper family.
    //
    // The ghost list does NOT store values, only keys.  Memory cost is bounded by `capacity` per shard.  Sharded by
    // the key's hash so concurrent inserts on different keys don't contend on the same mutex.
    struct GhostList {
        std::unordered_map< K, typename std::list< K >::iterator > map;
        std::list< K > order;
        size_t capacity;
        std::mutex lock;

        explicit GhostList(size_t cap) : capacity{cap} {}
        GhostList(GhostList const&) = delete;
        GhostList& operator=(GhostList const&) = delete;

        bool check_and_remove(K const& key) {
            std::lock_guard lk(lock);
            auto it = map.find(key);
            if (it == map.end())
                return false;
            order.erase(it->second);
            map.erase(it);
            return true;
        }

        void add(K const& key) {
            std::lock_guard lk(lock);
            if (map.size() >= capacity) {
                map.erase(order.front());
                order.pop_front();
            }
            order.push_back(key);
            map[key] = std::prev(order.end());
        }
    };

    Config cfg_;
    key_extractor_t key_extract_;
    shared< TwoQEvictor > evictor_;
    uint32_t family_id_{0};
    SimpleHashMap< K, CacheNode< V > > map_;
    std::vector< unique< GhostList > > ghost_lists_;
};

} // namespace sisl
