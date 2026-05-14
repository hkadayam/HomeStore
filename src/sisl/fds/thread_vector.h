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
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <vector>

#include <folly/SharedMutex.h>
#include <folly/ThreadLocal.h>

namespace sisl {

/*
 * This data structure inserts elements into per-thread buffers and provides APIs to access the elements.
 *
 * Concurrent push_back from any number of threads is safe. begin(true) drains all current per-thread
 * data into a snapshot buffer — it serializes with in-flight push_backs via an exclusive lock.
 * next() and clear() operate on the drained snapshot and are not thread-safe with push_back.
 */
template < typename T >
class ThreadVector {
public:
    ThreadVector() = default;
    ThreadVector(const ThreadVector&) = delete;
    ThreadVector(ThreadVector&&) noexcept = delete;
    ThreadVector& operator=(const ThreadVector&) = delete;
    ThreadVector& operator=(ThreadVector&&) noexcept = delete;

    ~ThreadVector() {
        // Destroy tl_vec_ first so its per-thread-slot deleter pushes into zombies_ while zombies_ is still
        // alive.  Without this, members destruct in reverse declaration order (zombies_ then tl_vec_) and the
        // deleter writes into freed memory — push_back resurrects the vector with a fresh allocation that
        // nobody ever frees (ASan reports the resurrected storage as a leak).  Re-init tl_vec_ as an empty TLP
        // so the natural member destruction is a no-op.
        tl_vec_.~ThreadLocalPtr();
        new (&tl_vec_) folly::ThreadLocalPtr< std::vector< T >, ThreadVectorTag >{};

        // Now drain zombies_ that were collected here (both pre-existing and the freshly pushed).
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto* v : zombies_) {
                delete v;
            }
        }
        clear_snapshot();
    }

    struct Iterator {
        size_t next_thread{0};
        size_t next_idx_in_thread{0};
    };

    template < typename InputType,
               typename = typename std::enable_if<
                   std::is_convertible< typename std::decay< InputType >::type, T >::value >::type >
    void push_back(InputType&& ele) {
        // Shared lock: many threads may push concurrently; drain takes exclusive lock.
        std::shared_lock< folly::SharedMutex > guard{drain_mutex_};
        local_vec().push_back(std::forward< InputType >(ele));
    }

    // If latest=true, atomically drains all current per-thread vectors into the snapshot.
    // Returns an iterator positioned at the start of the snapshot.
    Iterator begin(bool latest) {
        if (latest) {
            // Exclusive lock: no push_backs can be in-flight.
            std::unique_lock< folly::SharedMutex > guard{drain_mutex_};

            for (auto& vec : tl_vec_.accessAllThreads()) {
                if (!vec.empty()) {
                    auto* v = new std::vector< T >(std::move(vec));
                    snapshot_.push_back(v);
                    vec.clear();
                }
            }

            {
                std::unique_lock lg{zombie_mutex_};
                for (auto* v : zombies_) {
                    if (!v->empty()) {
                        snapshot_.push_back(v);
                    } else {
                        delete v;
                    }
                }
                zombies_.clear();
            }
        }
        return Iterator{};
    }

    T* next(Iterator& it) {
        while (it.next_thread < snapshot_.size()) {
            auto* tvec = snapshot_[it.next_thread];
            if (it.next_idx_in_thread < tvec->size()) {
                return &tvec->at(it.next_idx_in_thread++);
            } else {
                ++it.next_thread;
                it.next_idx_in_thread = 0;
            }
        }
        return nullptr;
    }

    void clear() {
        // Clear current per-thread vectors
        {
            std::unique_lock< folly::SharedMutex > guard{drain_mutex_};
            for (auto& vec : tl_vec_.accessAllThreads()) {
                vec.clear();
            }
            {
                std::unique_lock lg{zombie_mutex_};
                for (auto* v : zombies_) {
                    delete v;
                }
                zombies_.clear();
            }
        }
        clear_snapshot();
    }

    // Release the drained snapshot_ without touching TLS.  Callers that drain via begin(true) and process via next()
    // should use this instead of clear() to avoid losing pushes that arrived after the drain completed.
    void clear_snapshot() {
        for (auto* tvec : snapshot_) {
            delete tvec;
        }
        snapshot_.clear();
    }

    size_t size() {
        size_t sz{0};
        for (auto const* tvec : snapshot_) {
            sz += tvec->size();
        }
        {
            std::shared_lock< folly::SharedMutex > guard{drain_mutex_};
            for (auto& accessor : tl_vec_.accessAllThreads()) {
                auto* v = accessor.get();
                if (v) {
                    sz += v->size();
                }
            }
            {
                std::unique_lock lg{zombie_mutex_};
                for (auto const* v : zombies_) {
                    sz += v->size();
                }
            }
        }
        return sz;
    }

private:
    std::vector< T >& local_vec() {
        auto* v = tl_vec_.get();
        if (!v) {
            auto* owner = this;
            tl_vec_.reset(new std::vector< T >(), [owner](std::vector< T >* vec, folly::TLPDestructionMode) {
                std::unique_lock lg{owner->zombie_mutex_};
                owner->zombies_.push_back(vec);
            });
            v = tl_vec_.get();
        }
        return *v;
    }

    struct ThreadVectorTag {};
    folly::ThreadLocalPtr< std::vector< T >, ThreadVectorTag > tl_vec_;
    folly::SharedMutex drain_mutex_;
    mutable std::mutex zombie_mutex_;
    std::vector< std::vector< T >* > zombies_;
    std::vector< std::vector< T >* > snapshot_; // drained per-thread vectors
};

} // namespace sisl
