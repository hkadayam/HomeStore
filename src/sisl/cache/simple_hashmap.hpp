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

#include <boost/intrusive/slist.hpp>
#include <boost/functional/hash.hpp>
#include <folly/Traits.h>
#include <folly/small_vector.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wattributes"
#endif
#include <folly/SharedMutex.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include <sisl/fds/utils.h>
#include <sisl/fds/enum.h>
#include <sisl/cache/hash_entry_base.hpp>

namespace sisl {

template < typename K, typename V >
class SimpleHashBucket;

ENUM(hash_op_t, uint8_t, CREATE, ACCESS, DELETE, RESIZE)

template < typename K, typename V >
using kv_access_cb_t = std::function< void(const ValueEntryBase&, const K&, const V&, const hash_op_t) >;

template < typename K, typename V >
using key_extractor_cb_t = std::function< K(const V&) >;

static constexpr size_t s_start_seed = 0;

///////////////////////////////////////////// SimpleHashMap Declaration ///////////////////////////////////
template < typename K, typename V >
class SimpleHashMap {
public:
    // BucketCtx bundles the two callbacks that every bucket method needs.
    // It is constructed on the stack from the map's member callbacks and
    // passed by const-ref — zero heap allocation, no thread-local state.
    struct BucketCtx {
        const key_extractor_cb_t< K, V >& extractor;
        const kv_access_cb_t< K, V >&     access_cb; // may be an empty std::function
    };

private:
    uint32_t                     nbuckets_;
    SimpleHashBucket< K, V >*    buckets_;
    key_extractor_cb_t< K, V >   key_extract_cb_;
    kv_access_cb_t< K, V >       kv_access_cb_;

#ifdef GLOBAL_HASHSET_LOCK
    mutable std::mutex m;
#endif

    BucketCtx make_ctx() const { return {key_extract_cb_, kv_access_cb_}; }

public:
    SimpleHashMap(uint32_t nBuckets, const key_extractor_cb_t< K, V >& key_extractor,
                  kv_access_cb_t< K, V > access_cb = nullptr);
    ~SimpleHashMap();

    bool insert(const K& key, const V& value);
    bool upsert(const K& key, const V& value);
    bool get(const K& input_key, V& out_val);
    bool erase(const K& key, V& out_val);
    bool update(const K& key, auto&& update_cb);
    bool upsert_or_delete(const K& key, auto&& update_or_delete_cb);
    K    record_to_key(const ValueEntryBase& record);

    // ── Cache-specific atomic find/insert that bump the refcount ────────────
    // find_and_acquire: finds the entry for `key` and increments its refcount
    // while still holding the bucket read-lock, so the evictor cannot remove
    // the entry between the lookup and the caller's first dereference.
    // Returns {entry*, value*} on hit, {nullptr, nullptr} on miss.
    std::pair< ValueEntryBase*, V* > find_and_acquire(size_t hash, const K& key);

    // insert_and_acquire: inserts `key`→`value` and increments the new entry's
    // refcount before releasing the write-lock.  Returns nullptr on duplicate.
    ValueEntryBase* insert_and_acquire(size_t hash, const K& key, const V& value);

    static size_t compute_hash(const K& key) {
        size_t seed = s_start_seed;
        boost::hash_combine(seed, key);
        return seed;
    }

private:
    SimpleHashBucket< K, V >& get_bucket(const K& key) const;
    SimpleHashBucket< K, V >& get_bucket_from_hash(size_t hash_code) const;
};

///////////////////////////////////////////// SingleEntryHashNode Definitions ///////////////////////////////////
template < typename V >
struct SingleEntryHashNode : public ValueEntryBase, public boost::intrusive::slist_base_hook<> {
    V value_;
    explicit SingleEntryHashNode(const V& value) : value_{value} {}
    V* value_ptr() { return &value_; }
};

///////////////////////////////////////////// SimpleHashBucket Definitions ///////////////////////////////////
template < typename K, typename V >
class SimpleHashBucket {
private:
#ifndef GLOBAL_HASHSET_LOCK
    mutable folly::SharedMutexWritePriority lock_;
#endif
    using hash_node_list_t = boost::intrusive::slist< SingleEntryHashNode< V > >;
    hash_node_list_t list_;

    using BucketCtx = typename SimpleHashMap< K, V >::BucketCtx;

    static void invoke_access_cb(const BucketCtx& ctx, const SingleEntryHashNode< V >& node,
                                  const K& key, const V& value, hash_op_t op) {
        if (ctx.access_cb) ctx.access_cb((const ValueEntryBase&)node, key, value, op);
    }

public:
    SimpleHashBucket() = default;

    // destroy() is called explicitly by SimpleHashMap::~SimpleHashMap() before
    // delete[]'ing the bucket array.  It drains the list with DELETE callbacks.
    void destroy(const BucketCtx& ctx) {
        auto it = list_.begin();
        while (it != list_.end()) {
            SingleEntryHashNode< V >* n = &*it;
            const K k = ctx.extractor(n->value_);
            invoke_access_cb(ctx, *n, k, n->value_, hash_op_t::DELETE);
            it = list_.erase(it);
            delete n;
        }
    }

    // Default destructor: by the time it runs, destroy() has already drained
    // the list.  Any remaining entries (shouldn't happen in normal use) are
    // leaked rather than use-after-free'd without the callbacks.
    ~SimpleHashBucket() = default;

    bool insert(const BucketCtx& ctx, const K& input_key, const V& input_value, bool overwrite_ok) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::WriteHolder holder(lock_);
#endif
        SingleEntryHashNode< V >* n = nullptr;
        auto it = list_.begin();
        for (auto itend{list_.end()}; it != itend; ++it) {
            const K k = ctx.extractor(it->value_);
            if (input_key > k) {
                break;
            } else if (input_key == k) {
                n = &*it;
            }
        }

        if (n == nullptr) {
            n = new SingleEntryHashNode< V >(input_value);
            list_.insert(it, *n);
            invoke_access_cb(ctx, *n, input_key, input_value, hash_op_t::CREATE);
            return true;
        } else {
            if (overwrite_ok) {
                n->value_ = input_value;
                invoke_access_cb(ctx, *n, input_key, input_value, hash_op_t::ACCESS);
            }
            return false;
        }
    }

    bool get(const BucketCtx& ctx, const K& input_key, V& out_val) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::ReadHolder holder(lock_);
#endif
        for (const auto& n : list_) {
            const K k = ctx.extractor(n.value_);
            if (input_key > k) { break; }
            if (input_key == k) {
                out_val = n.value_;
                invoke_access_cb(ctx, n, input_key, out_val, hash_op_t::ACCESS);
                return true;
            }
        }
        return false;
    }

    // Finds the entry, calls acquire() atomically under the read-lock, and
    // returns {entry*, value*}.  The refcount is non-zero before the lock is
    // released, so the evictor cannot remove the entry between the lookup and
    // the caller's first dereference.
    std::pair< ValueEntryBase*, V* > find_and_acquire(const BucketCtx& ctx, const K& input_key) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::ReadHolder holder(lock_);
#endif
        for (auto& n : list_) {
            const K k = ctx.extractor(n.value_);
            if (input_key > k) { break; }
            if (input_key == k) {
                n.acquire();
                return {&n, &n.value_};
            }
        }
        return {nullptr, nullptr};
    }

    // Inserts the entry, calls acquire() before releasing the write-lock.
    // Returns nullptr if the key already exists.
    ValueEntryBase* insert_and_acquire(const BucketCtx& ctx, const K& input_key, const V& input_value) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::WriteHolder holder(lock_);
#endif
        auto it = list_.begin();
        for (auto itend{list_.end()}; it != itend; ++it) {
            const K k = ctx.extractor(it->value_);
            if (input_key > k) { break; }
            if (input_key == k) { return nullptr; } // duplicate
        }

        auto* n = new SingleEntryHashNode< V >(input_value);
        list_.insert(it, *n);
        n->acquire();
        invoke_access_cb(ctx, *n, input_key, input_value, hash_op_t::CREATE);
        return n;
    }

    bool erase(const BucketCtx& ctx, const K& input_key, V& out_val) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::WriteHolder holder(lock_);
#endif
        auto it = list_.begin();
        for (auto itend{list_.end()}; it != itend; ++it) {
            const K k = ctx.extractor(it->value_);
            if (input_key > k) { break; }
            if (input_key == k) {
                SingleEntryHashNode< V >* n = &*it;
                invoke_access_cb(ctx, *n, input_key, n->value_, hash_op_t::DELETE);
                out_val = n->value_;
                list_.erase(it);
                delete n;
                return true;
            }
        }
        return false;
    }

    bool upsert_or_delete(const BucketCtx& ctx, const K& input_key, auto&& update_or_delete_cb) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::WriteHolder holder(lock_);
#endif
        SingleEntryHashNode< V >* n = nullptr;
        auto it = list_.begin();
        for (auto itend{list_.end()}; it != itend; ++it) {
            const K k = ctx.extractor(it->value_);
            if (input_key > k) { break; }
            if (input_key == k) { n = &*it; break; }
        }

        bool found = (n != nullptr);
        if (!found) {
            n = new SingleEntryHashNode< V >(V{});
            list_.insert(it, *n);
        }

        if (update_or_delete_cb(n->value_, found)) {
            if (found) { invoke_access_cb(ctx, *n, input_key, n->value_, hash_op_t::DELETE); }
            list_.erase(it);
            delete n;
        } else {
            invoke_access_cb(ctx, *n, input_key, n->value_,
                             (found ? hash_op_t::ACCESS : hash_op_t::CREATE));
        }
        return !found;
    }

    bool update(const BucketCtx& ctx, const K& input_key, auto&& update_cb) {
#ifndef GLOBAL_HASHSET_LOCK
        folly::SharedMutexWritePriority::WriteHolder holder(lock_);
#endif
        for (auto& n : list_) {
            const K k = ctx.extractor(n.value_);
            if (input_key > k) { break; }
            if (input_key == k) {
                invoke_access_cb(ctx, n, input_key, n.value_, hash_op_t::ACCESS);
                update_cb(n.value_);
                return true;
            }
        }
        return false;
    }
};

///////////////////////////////////////////// SimpleHashMap Definitions ///////////////////////////////////
template < typename K, typename V >
SimpleHashMap< K, V >::SimpleHashMap(uint32_t nBuckets, const key_extractor_cb_t< K, V >& extract_cb,
                                     kv_access_cb_t< K, V > access_cb)
    : nbuckets_{nBuckets}, key_extract_cb_{extract_cb}, kv_access_cb_{std::move(access_cb)} {
    buckets_ = new SimpleHashBucket< K, V >[nBuckets];
}

template < typename K, typename V >
SimpleHashMap< K, V >::~SimpleHashMap() {
    const BucketCtx ctx = make_ctx();
    for (uint32_t i = 0; i < nbuckets_; ++i) {
        buckets_[i].destroy(ctx);
    }
    delete[] buckets_;
}

template < typename K, typename V >
bool SimpleHashMap< K, V >::insert(const K& key, const V& value) {
#ifdef GLOBAL_HASHSET_LOCK
    std::lock_guard< std::mutex > lk(m);
#endif
    return get_bucket(key).insert(make_ctx(), key, value, false /* overwrite_ok */);
}

template < typename K, typename V >
bool SimpleHashMap< K, V >::upsert(const K& key, const V& value) {
#ifdef GLOBAL_HASHSET_LOCK
    std::lock_guard< std::mutex > lk(m);
#endif
    return get_bucket(key).insert(make_ctx(), key, value, true /* overwrite_ok */);
}

template < typename K, typename V >
bool SimpleHashMap< K, V >::get(const K& key, V& out_val) {
#ifdef GLOBAL_HASHSET_LOCK
    std::lock_guard< std::mutex > lk(m);
#endif
    return get_bucket(key).get(make_ctx(), key, out_val);
}

template < typename K, typename V >
bool SimpleHashMap< K, V >::erase(const K& key, V& out_val) {
#ifdef GLOBAL_HASHSET_LOCK
    std::lock_guard< std::mutex > lk(m);
#endif
    return get_bucket(key).erase(make_ctx(), key, out_val);
}

template < typename K, typename V >
std::pair< ValueEntryBase*, V* > SimpleHashMap< K, V >::find_and_acquire(size_t hash, const K& key) {
    return get_bucket_from_hash(hash).find_and_acquire(make_ctx(), key);
}

template < typename K, typename V >
ValueEntryBase* SimpleHashMap< K, V >::insert_and_acquire(size_t hash, const K& key, const V& value) {
    return get_bucket_from_hash(hash).insert_and_acquire(make_ctx(), key, value);
}

template < typename K, typename V >
K SimpleHashMap< K, V >::record_to_key(const ValueEntryBase& record) {
    return key_extract_cb_(r_cast< SingleEntryHashNode< V > const& >(record).value_);
}

template < typename K, typename V >
bool SimpleHashMap< K, V >::upsert_or_delete(const K& key, auto&& update_or_delete_cb) {
#ifdef GLOBAL_HASHSET_LOCK
    std::lock_guard< std::mutex > lk(m);
#endif
    return get_bucket(key).upsert_or_delete(make_ctx(), key, std::move(update_or_delete_cb));
}

template < typename K, typename V >
bool SimpleHashMap< K, V >::update(const K& key, auto&& update_cb) {
#ifdef GLOBAL_HASHSET_LOCK
    std::lock_guard< std::mutex > lk(m);
#endif
    return get_bucket(key).update(make_ctx(), key, std::move(update_cb));
}

template < typename K, typename V >
SimpleHashBucket< K, V >& SimpleHashMap< K, V >::get_bucket(const K& key) const {
    return buckets_[compute_hash(key) % nbuckets_];
}

template < typename K, typename V >
SimpleHashBucket< K, V >& SimpleHashMap< K, V >::get_bucket_from_hash(size_t hash_code) const {
    return buckets_[hash_code % nbuckets_];
}

} // namespace sisl
