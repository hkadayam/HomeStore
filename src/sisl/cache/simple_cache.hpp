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

#include <set>
#include <sisl/fds/utils.h>
#include <sisl/cache/evictor.hpp>
#include <sisl/cache/simple_hashmap.hpp>

using namespace std::placeholders;

namespace sisl {

ENUM(SimpleCacheStatus, uint8_t, success, not_found, duplicate, cant_evict);

template < typename K, typename V >
class SimpleCache {
public:
    using can_evict_cb_t = std::function< bool(const CacheRecord&) >;
    using size_extractor = std::function< uint32_t(const V&) >;

    enum class Status : uint8_t { success, not_found, duplicate, cant_evict };

private:
    std::shared_ptr< Evictor > m_evictor;
    key_extractor_cb_t< K, V > m_key_extract_cb;
    can_evict_cb_t m_can_evict_cb;
    size_extractor m_size_extract_cb;
    SimpleHashMap< K, V > m_map;
    uint32_t m_record_family_id;

    static thread_local std::vector< K > t_failed_keys;
    static thread_local std::vector< K > t_to_evict_keys;
    static thread_local std::vector< K > t_invalidated_keys;

public:
    SimpleCache(const std::shared_ptr< Evictor >& evictor, uint32_t num_buckets,
                key_extractor_cb_t< K, V >&& extract_cb, size_extractor&& size_cb, can_evict_cb_t evict_cb = nullptr) :
            m_evictor{evictor},
            m_key_extract_cb{std::move(extract_cb)},
            m_can_evict_cb{std::move(evict_cb)},
            m_size_extract_cb{std::move(size_cb)},
            m_map{num_buckets, m_key_extract_cb,
                  std::bind(&SimpleCache< K, V >::on_hash_operation, this, _1, _2, _3, _4)} {
        m_record_family_id = m_evictor->register_record_family(
            Evictor::RecordFamily{.do_evict_cb = std::bind(&SimpleCache< K, V >::do_evict, this, _1)});
    }

    ~SimpleCache() { m_evictor->unregister_record_family(m_record_family_id); }

    SimpleCacheStatus insert(const V& value) {
        K k = m_key_extract_cb(value);
        bool const s = m_map.insert(k, value);
        if (!s) { return SimpleCacheStatus::duplicate; }

        // We were able to insert into map, but check if evictor had reported any errors
        return handle_evictor_response();
    }

    SimpleCacheStatus update(const V& value) {
        K k = m_key_extract_cb(value);
        bool success = m_map.update(k, [&value](V& v) { v = value; });

        // We were able to insert into map, but check if evictor had reported any errors
        return success ? handle_evictor_response() : SimpleCacheStatus::not_found;
    }

#if 0
    std::pair< SimpleCacheStatus, bool > upsert(const V& value) {
        K k = m_key_extract_cb(value);
        bool found = !m_map.upsert(k, value);

        // We were able to insert into map, but check if evictor had reported any errors
        auto const status = handle_evictor_response();
        return std::make_pair(status, found);
    }
#endif

    SimpleCacheStatus remove(const K& key, V& out_val) {
        bool success = m_map.erase(key, out_val);
        return success ? handle_evictor_response() : SimpleCacheStatus::not_found;
    }

    SimpleCacheStatus get(const K& key, V& out_val) {
        bool success = m_map.get(key, out_val);
        return success ? handle_evictor_response() : SimpleCacheStatus::not_found;
    }

private:
    void on_hash_operation(const CacheRecord& r, const K& key, const V& value, const hash_op_t op) {
        CacheRecord& record = const_cast< CacheRecord& >(r);
        const auto hash_code = SimpleHashMap< K, V >::compute_hash(key);

        switch (op) {
        case hash_op_t::CREATE:
            record.set_record_family(m_record_family_id);
            record.set_size(m_size_extract_cb(value));
            if (!m_evictor->add_record(hash_code, record)) {
                // We were not able to evict any, so mark this record and we will erase them upon all callbacks are done
                t_failed_keys.push_back(key);
            }
            break;

        case hash_op_t::DELETE:
            m_evictor->remove_record(hash_code, record);
            break;

        case hash_op_t::ACCESS:
            // If evictor says that this record is already soft deleted, then add to thread variable, which will be
            // cleaned up by the top level SimpleCache API.
            if (!m_evictor->record_accessed(hash_code, record)) { t_invalidated_keys.push_back(key); }
            break;

        case hash_op_t::RESIZE: {
            DEBUG_ASSERT(false, "Don't expect RESIZE operation for simple cache entries");
            break;
        }
        default:
            DEBUG_ASSERT(false, "Invalid hash_op");
            break;
        }
    }

    bool do_evict(const CacheRecord& record) {
        if ((m_can_evict_cb == nullptr) || m_can_evict_cb(record)) {
            t_to_evict_keys.push_back(m_map.record_to_key(record));
            return true;
        } else {
            return false;
        }
    }

    SimpleCacheStatus handle_evictor_response() {
        SimpleCacheStatus status{SimpleCacheStatus::success};
        if (!t_to_evict_keys.empty()) {
            V dummy_val;
            // Some keys had to be evicted, so remove them from the map
            for (const auto& key : t_to_evict_keys) {
                m_map.erase(key, dummy_val);
            }
            t_to_evict_keys.clear();
        }

        if (!t_failed_keys.empty()) {
            V dummy_val;
            for (const auto& key : t_failed_keys) {
                m_map.erase(key, dummy_val);
            }
            t_failed_keys.clear();
            status = SimpleCacheStatus::cant_evict;
        }

        if (!t_invalidated_keys.empty()) {
            // We support as of now only 1 key get per operation. When we support bulk operation, we can relax this
            // assert
            DEBUG_ASSERT_EQ(t_invalidated_keys.size(), 1,
                            "We should have encountered only 1 invalidated key during update/remove/get");
            t_invalidated_keys.clear();
            status = SimpleCacheStatus::not_found;
        }
        return status;
    }
};

template < typename K, typename V >
thread_local std::vector< K > SimpleCache< K, V >::t_failed_keys;

template < typename K, typename V >
thread_local std::vector< K > SimpleCache< K, V >::t_to_evict_keys;

template < typename K, typename V >
thread_local std::vector< K > SimpleCache< K, V >::t_invalidated_keys;
} // namespace sisl
