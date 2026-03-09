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

#include <array>
#include <memory>
#include <vector>

namespace sisl {

#define VECTOR_POOL_CACHE_COUNT 100

template < typename T, size_t CacheCount = VECTOR_POOL_CACHE_COUNT >
class VectorPoolImpl {
public:
    VectorPoolImpl() {
        for (auto i = 0u; i < CacheCount; ++i) {
            pool_[i] = new std::vector< T >();
        }
        last_ = CacheCount;
    }

    ~VectorPoolImpl() {
        for (auto i = 0u; i < last_; ++i) {
            delete (pool_[i]);
        }
    }

    std::vector< T >* allocate() { return (last_ == 0) ? new std::vector< T >() : pool_[--last_]; }
    void deallocate(std::vector< T >* v) {
        if (last_ == CacheCount) {
            delete (v);
        } else {
            v->clear();
            pool_[last_++] = v;
        }
    }

private:
    std::array< std::vector< T >*, CacheCount > pool_;
    size_t last_{0};
};

template < typename T, size_t CacheCount = VECTOR_POOL_CACHE_COUNT >
class VectorPool {
public:
    static std::vector< T >* alloc() { return impl().allocate(); }
    static void free(std::vector< T >* v, bool no_cache = false) { no_cache ? delete (v) : impl().deallocate(v); }

private:
    static VectorPoolImpl< T, CacheCount >& impl() {
        static thread_local VectorPoolImpl< T, CacheCount > s_impl;
        return s_impl;
    }
};

} // namespace sisl
