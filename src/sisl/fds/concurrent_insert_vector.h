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

#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>
#include <vector>

#include <folly/ThreadLocal.h>

namespace sisl {

//
// This data structure provides a vector where concurrent threads can safely emplace or push back the data into.
// However, it does not guarantee any access or iterations happen during the insertion. It is the responsibility of the
// user to synchronize this behavior. This data structure is useful when the user wants to insert data into a vector
// concurrently in a fast manner and then iterate over the data later. If the user wants a vector implementation which
// reads concurrently with writer, they can use sisl::ThreadVector. This data structure is provided as a replacement for
// simplistic cases where insertion and iteration never happen concurrently. As a result it provides better performance
// than even sisl::ThreadVector and better debuggability.
//
// Benchmark shows atleast 10x better performance on more than 4 threads concurrently inserting with mutex.
//
template < typename T >
class ConcurrentInsertVector {
public:
    struct iterator {
        size_t next_thread{0};
        size_t next_id_in_thread{0};
        std::vector< std::vector< T > const* > per_thread_vectors;

        iterator() = default;
        iterator(std::vector< std::vector< T > const* > v) : per_thread_vectors{std::move(v)} {
            if (per_thread_vectors.empty()) { next_thread = std::numeric_limits< size_t >::max(); }
        }

        void operator++() {
            ++next_id_in_thread;
            if (next_id_in_thread >= per_thread_vectors[next_thread]->size()) {
                ++next_thread;
                next_id_in_thread = 0;
            }
            if (next_thread >= per_thread_vectors.size()) { next_thread = std::numeric_limits< size_t >::max(); }
        }

        void operator+=(int64_t count) {
            while ((count > 0) && (next_thread < per_thread_vectors.size())) {
                auto remaining = (int64_t)(per_thread_vectors[next_thread]->size() - next_id_in_thread);
                if (count < remaining) {
                    next_id_in_thread += count;
                    break;
                } else {
                    count -= remaining;
                    ++next_thread;
                    next_id_in_thread = 0;
                }
            }
            if (next_thread >= per_thread_vectors.size()) { next_thread = std::numeric_limits< size_t >::max(); }
        }

        bool operator==(iterator const& other) const {
            return ((next_thread == other.next_thread) && (next_id_in_thread == other.next_id_in_thread));
        }
        bool operator!=(iterator const& other) const { return !(*this == other); }

        T const& operator*() const { return per_thread_vectors[next_thread]->at(next_id_in_thread); }
        T const* operator->() const { return &(per_thread_vectors[next_thread]->at(next_id_in_thread)); }
    };

    ConcurrentInsertVector() = default;
    ConcurrentInsertVector(const ConcurrentInsertVector&) = delete;
    ConcurrentInsertVector(ConcurrentInsertVector&&) noexcept = delete;
    ConcurrentInsertVector& operator=(const ConcurrentInsertVector&) = delete;
    ConcurrentInsertVector& operator=(ConcurrentInsertVector&&) noexcept = delete;

    ~ConcurrentInsertVector() {
        std::unique_lock lg{zombie_mutex_};
        for (auto* v : zombies_) {
            delete v;
        }
    }

    template < typename InputType,
               typename = typename std::enable_if<
                   std::is_convertible< typename std::decay< InputType >::type, T >::value >::type >
    void push_back(InputType&& ele) {
        local_vec().push_back(std::forward< InputType >(ele));
    }

    template < class... Args >
    void emplace_back(Args&&... args) {
        local_vec().emplace_back(std::forward< Args >(args)...);
    }

    iterator begin() {
        std::vector< std::vector< T > const* > ptrs;
        ptrs.reserve(8);
        for (auto& accessor : tl_vec_.accessAllThreads()) {
            auto* v = accessor.get();
            if (v && !v->empty()) { ptrs.push_back(v); }
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto* v : zombies_) {
                if (!v->empty()) { ptrs.push_back(v); }
            }
        }
        return iterator{std::move(ptrs)};
    }

    iterator end() { return iterator{}; }

    void foreach_entry(auto&& cb) {
        for (auto& accessor : tl_vec_.accessAllThreads()) {
            auto* v = accessor.get();
            if (v) {
                for (auto const& e : *v) {
                    cb(e);
                }
            }
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto* v : zombies_) {
                for (auto const& e : *v) {
                    cb(e);
                }
            }
        }
    }

    size_t size() const {
        size_t sz{0};
        for (auto& accessor : const_cast< ConcurrentInsertVector* >(this)->tl_vec_.accessAllThreads()) {
            auto* v = accessor.get();
            if (v) { sz += v->size(); }
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto const* v : zombies_) {
                sz += v->size();
            }
        }
        return sz;
    }

    void clear() {
        for (auto& accessor : tl_vec_.accessAllThreads()) {
            auto* v = accessor.get();
            if (v) { v->clear(); }
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto* v : zombies_) {
                v->clear();
            }
        }
    }

private:
    std::vector< T >& local_vec() {
        auto* v = tl_vec_.get();
        if (!v) {
            auto* owner = this;
            tl_vec_.reset(new std::vector< T >(),
                          [owner](std::vector< T >* vec, folly::TLPDestructionMode) {
                              std::unique_lock lg{owner->zombie_mutex_};
                              owner->zombies_.push_back(vec);
                          });
            v = tl_vec_.get();
        }
        return *v;
    }

    folly::ThreadLocalPtr< std::vector< T > > tl_vec_;
    mutable std::mutex zombie_mutex_;
    std::vector< std::vector< T >* > zombies_;
};

} // namespace sisl
