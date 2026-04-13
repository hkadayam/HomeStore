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

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include <boost/intrusive/slist.hpp>
#include <boost/functional/hash.hpp>

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wattributes"
#endif
#include <folly/SharedMutex.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include "common/defs.h"
#include <sisl/cache/hashmap_traits.h>

namespace sisl {

// ───────────────────────────────────────────────── HashNode<V> ───────────────────────────────────────────────────────
// One entry in a hash bucket's intrusive slist.  Holds the value V by value.
//
// For non-refcounted V (e.g. BlkReadTracker): overhead = 8 bytes (slist hook).  For refcounted V = CacheNode<Foo>:
// see cache_node.h for the full layout.
template < typename V >
struct HashNode : public boost::intrusive::slist_base_hook<> {
    V value_;

    explicit HashNode(V const& v) : value_(v) {}
    explicit HashNode(V&& v) : value_(std::move(v)) {}

    HashNode(HashNode const&) = delete;
    HashNode& operator=(HashNode const&) = delete;
};

// ────────────────────────────────────────────────── Handle<V> ────────────────────────────────────────────────────────
// RAII handle returned by SimpleHashMap::find() / insert() and friends.  Move-only.
//
// While a Handle is alive:
//   • For refcounted V (per HashmapTraits): the trait's acquire() bumped the refcount before the handle was returned;
//     ~Handle calls release().
//   • For non-refcounted V: the trait acquire/release are no-ops, and the handle is essentially a tagged pointer.
template < typename V >
class Handle {
    HashNode< V >* node_{nullptr};

public:
    Handle() = default;
    explicit Handle(HashNode< V >* n) : node_{n} {}

    ~Handle() {
        if (node_)
            HashmapTraits< V >::release(node_->value_);
    }

    Handle(Handle&& o) noexcept : node_{o.node_} { o.node_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            if (node_)
                HashmapTraits< V >::release(node_->value_);
            node_ = o.node_;
            o.node_ = nullptr;
        }
        return *this;
    }

    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;

    explicit operator bool() const { return node_ != nullptr; }

    V& value() { return node_->value_; }
    V const& value() const { return node_->value_; }

    V* operator->() { return &node_->value_; }
    V const* operator->() const { return &node_->value_; }
};

template < typename K, typename V >
using key_extractor_t = std::function< K(V const&) >;

// ─────────────────────────────────────────────── SimpleHashBucket ────────────────────────────────────────────────────
// One bucket of the hashmap.  Holds an intrusive slist of HashNode<V> sorted by key.  Refcount semantics for V are
// driven entirely by HashmapTraits<V>.
template < typename K, typename V >
class SimpleHashBucket {
private:
#ifndef GLOBAL_HASHSET_LOCK
    mutable folly::SharedMutexWritePriority lock_;
#endif
    using HashNodeList = boost::intrusive::slist< HashNode< V > >;
    HashNodeList list_;

public:
    SimpleHashBucket() = default;
    ~SimpleHashBucket() = default;

    // Drain the bucket on hashmap destruction.
    void destroy() {
        auto it = list_.begin();
        while (it != list_.end()) {
            HashNode< V >* n = &*it;
            it = list_.erase(it);
            delete n;
        }
    }

    // Inserts (key, value).  If the key already exists: returns an empty handle (insert failed) — does NOT overwrite.
    // On success the returned Handle holds an acquired ref via HashmapTraits.
    template < typename Vin >
    Handle< V > insert(key_extractor_t< K, V > const& extract, K const& input_key, Vin&& value) {
#ifndef GLOBAL_HASHSET_LOCK
        std::unique_lock holder(lock_);
#endif
        auto it = list_.begin();
        for (auto end = list_.end(); it != end; ++it) {
            K const k = extract(it->value_);
            if (input_key > k)
                break;
            if (input_key == k)
                return Handle< V >{}; // duplicate
        }

        auto* n = new HashNode< V >(std::forward< Vin >(value));
        list_.insert(it, *n);

        // Refcount counts only outstanding handles.  Map membership itself is NOT a reference.  Bump once for the
        // handle we are about to return.
        HashmapTraits< V >::acquire(n->value_);
        return Handle< V >{n};
    }

    // upsert: like insert but overwrites the existing value if present.  Returns a Handle to the (new or existing)
    // entry.
    template < typename Vin >
    Handle< V > upsert(key_extractor_t< K, V > const& extract, K const& input_key, Vin&& value) {
#ifndef GLOBAL_HASHSET_LOCK
        std::unique_lock holder(lock_);
#endif
        auto it = list_.begin();
        for (auto end = list_.end(); it != end; ++it) {
            K const k = extract(it->value_);
            if (input_key > k)
                break;
            if (input_key == k) {
                it->value_ = std::forward< Vin >(value);
                HashmapTraits< V >::acquire(it->value_);
                return Handle< V >{&*it};
            }
        }

        auto* n = new HashNode< V >(std::forward< Vin >(value));
        list_.insert(it, *n);
        HashmapTraits< V >::acquire(n->value_);
        return Handle< V >{n};
    }

    // Returns a Handle to the matching entry; empty handle on miss.  Acquire happens under the bucket read-lock so
    // concurrent eviction cannot race.
    Handle< V > find(key_extractor_t< K, V > const& extract, K const& input_key) {
#ifndef GLOBAL_HASHSET_LOCK
        std::shared_lock holder(lock_);
#endif
        for (auto& n : list_) {
            K const k = extract(n.value_);
            if (input_key > k)
                break;
            if (input_key == k) {
                HashmapTraits< V >::acquire(n.value_);
                return Handle< V >{&n};
            }
        }
        return Handle< V >{};
    }

    // Unconditional erase (no refcount check).  Returns true if the entry existed.  Caller is responsible for ensuring
    // no Handle is alive.
    bool erase(key_extractor_t< K, V > const& extract, K const& input_key) {
#ifndef GLOBAL_HASHSET_LOCK
        std::unique_lock holder(lock_);
#endif
        auto it = list_.begin();
        for (auto end = list_.end(); it != end; ++it) {
            K const k = extract(it->value_);
            if (input_key > k)
                break;
            if (input_key == k) {
                HashNode< V >* n = &*it;
                list_.erase(it);
                delete n;
                return true;
            }
        }
        return false;
    }

    // Atomically erase the entry only if no Handle is currently referencing it (refcount == 0).  Returns:
    //   true  — entry existed, was unreferenced, was erased
    //   false — entry not found OR was still referenced
    //
    // Used by the evictor to claim eviction candidates safely.  The bucket write-lock + atomic refcount check is the
    // synchronisation point that prevents a racing find() from acquiring after the evictor commits.
    bool erase_if_no_reference(key_extractor_t< K, V > const& extract, K const& input_key) {
#ifndef GLOBAL_HASHSET_LOCK
        std::unique_lock holder(lock_);
#endif
        auto it = list_.begin();
        for (auto end = list_.end(); it != end; ++it) {
            K const k = extract(it->value_);
            if (input_key > k)
                break;
            if (input_key == k) {
                if (!HashmapTraits< V >::is_unreferenced(it->value_))
                    return false;
                HashNode< V >* n = &*it;
                list_.erase(it);
                delete n;
                return true;
            }
        }
        return false;
    }

    // Callback-driven update.  Callback signature: UpdateAction fn(V&, bool found).  Returns enum:
    //   UpdateAction::Keep  — entry stays (after possible mutation by cb)
    //   UpdateAction::Erase — entry is erased
    //
    // If the key was not found, the callback is called with a default-constructed V and `found=false`.  If the
    // callback returns Keep, the entry is inserted with that value.
    //
    // Returns true if a new entry was created, false if updated/erased.
    enum class UpdateAction { Keep, Erase };

    template < typename Fn >
    bool update_or_erase(key_extractor_t< K, V > const& extract, K const& input_key, Fn&& fn) {
#ifndef GLOBAL_HASHSET_LOCK
        std::unique_lock holder(lock_);
#endif
        HashNode< V >* found_node = nullptr;
        auto it = list_.begin();
        for (auto end = list_.end(); it != end; ++it) {
            K const k = extract(it->value_);
            if (input_key > k)
                break;
            if (input_key == k) {
                found_node = &*it;
                break;
            }
        }

        bool const found = (found_node != nullptr);
        if (!found) {
            found_node = new HashNode< V >(V{});
            // Don't insert yet — wait until we know whether the cb wants Keep.
        }

        UpdateAction const action = fn(found_node->value_, found);
        if (action == UpdateAction::Erase) {
            if (found) {
                list_.erase(it);
            }
            delete found_node;
            return false;
        } else if (action == UpdateAction::Keep) {
            // Keep: insert if new (refcount stays at 0 — no handle is returned).
            if (!found) {
                list_.insert(it, *found_node);
            }
        }
        return !found;
    }
};

// ──────────────────────────────────────────────── SimpleHashMap<K, V> ────────────────────────────────────────────────
template < typename K, typename V >
class SimpleHashMap {
private:
    uint32_t nbuckets_;
    SimpleHashBucket< K, V >* buckets_;
    key_extractor_t< K, V > key_extract_;

#ifdef GLOBAL_HASHSET_LOCK
    mutable std::mutex m_;
#endif

public:
    using UpdateAction = typename SimpleHashBucket< K, V >::UpdateAction;

    SimpleHashMap(uint32_t num_buckets, key_extractor_t< K, V > extract) :
            nbuckets_{num_buckets}, key_extract_{std::move(extract)} {
        buckets_ = new SimpleHashBucket< K, V >[num_buckets];
    }

    ~SimpleHashMap() {
        for (uint32_t i = 0; i < nbuckets_; ++i) {
            buckets_[i].destroy();
        }
        delete[] buckets_;
    }

    SimpleHashMap(SimpleHashMap const&) = delete;
    SimpleHashMap& operator=(SimpleHashMap const&) = delete;

    static size_t compute_hash(K const& key) {
        size_t seed = 0;
        boost::hash_combine(seed, key);
        return seed;
    }

    template < typename Vin >
    Handle< V > insert(K const& key, Vin&& value) {
#ifdef GLOBAL_HASHSET_LOCK
        std::lock_guard< std::mutex > lk(m_);
#endif
        return get_bucket(key).insert(key_extract_, key, std::forward< Vin >(value));
    }

    template < typename Vin >
    Handle< V > upsert(K const& key, Vin&& value) {
#ifdef GLOBAL_HASHSET_LOCK
        std::lock_guard< std::mutex > lk(m_);
#endif
        return get_bucket(key).upsert(key_extract_, key, std::forward< Vin >(value));
    }

    Handle< V > find(K const& key) {
#ifdef GLOBAL_HASHSET_LOCK
        std::lock_guard< std::mutex > lk(m_);
#endif
        return get_bucket(key).find(key_extract_, key);
    }

    bool erase(K const& key) {
#ifdef GLOBAL_HASHSET_LOCK
        std::lock_guard< std::mutex > lk(m_);
#endif
        return get_bucket(key).erase(key_extract_, key);
    }

    bool erase_if_no_reference(K const& key) {
#ifdef GLOBAL_HASHSET_LOCK
        std::lock_guard< std::mutex > lk(m_);
#endif
        return get_bucket(key).erase_if_no_reference(key_extract_, key);
    }

    template < typename Fn >
    bool update_or_erase(K const& key, Fn&& fn) {
#ifdef GLOBAL_HASHSET_LOCK
        std::lock_guard< std::mutex > lk(m_);
#endif
        return get_bucket(key).update_or_erase(key_extract_, key, std::forward< Fn >(fn));
    }

private:
    SimpleHashBucket< K, V >& get_bucket(K const& key) const {
        return buckets_[compute_hash(key) % nbuckets_];
    }
};

} // namespace sisl
