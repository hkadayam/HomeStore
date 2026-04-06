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
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <folly/ThreadLocal.h>

namespace sisl {

//
// Same per-thread insertion pattern as ConcurrentInsertVector, but backed by std::unordered_set per thread. Duplicates
// within the same thread are eliminated at insert time (O(1) amortized). Cross-thread duplicates are resolved at gather
// time — gather() returns a single deduped unordered_set merging all per-thread sets and zombie sets.
//
// Use this when the hot path inserts the same key repeatedly (e.g. marking a chunk dirty on every write) and you want
// O(1) storage per unique key per thread rather than O(N) storage per insert.
//
template < typename T, typename Hash = std::hash< T >, typename KeyEqual = std::equal_to< T > >
class ConcurrentInsertSet {
public:
    ConcurrentInsertSet() = default;
    ConcurrentInsertSet(const ConcurrentInsertSet&) = delete;
    ConcurrentInsertSet(ConcurrentInsertSet&&) noexcept = delete;
    ConcurrentInsertSet& operator=(const ConcurrentInsertSet&) = delete;
    ConcurrentInsertSet& operator=(ConcurrentInsertSet&&) noexcept = delete;

    ~ConcurrentInsertSet() {
        std::unique_lock lg{zombie_mutex_};
        for (auto* s : zombies_) {
            delete s;
        }
    }

    template < typename InputType,
               typename = typename std::enable_if<
                   std::is_convertible< typename std::decay< InputType >::type, T >::value >::type >
    void insert(InputType&& ele) {
        local_set().insert(std::forward< InputType >(ele));
    }

    template < class... Args >
    void emplace(Args&&... args) {
        local_set().emplace(std::forward< Args >(args)...);
    }

    // Returns a single deduped set merging all per-thread sets and zombie sets. If clear_on_gather is true, every
    // per-thread set and zombie set is cleared after its entries are merged — useful for "drain and reset" patterns
    // like CP flush where the caller wants to atomically snapshot and reset dirty state.
    std::unordered_set< T, Hash, KeyEqual > gather(bool clear_on_gather = false) {
        std::unordered_set< T, Hash, KeyEqual > result;
        for (auto& s : tl_set_.accessAllThreads()) {
            result.insert(s.begin(), s.end());
            if (clear_on_gather) { s.clear(); }
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto* s : zombies_) {
                result.insert(s->begin(), s->end());
                if (clear_on_gather) { delete s; }
            }
            if (clear_on_gather) { zombies_.clear(); }
        }
        return result;
    }

    size_t size() const {
        size_t sz{0};
        for (auto& s : const_cast< ConcurrentInsertSet* >(this)->tl_set_.accessAllThreads()) {
            sz += s.size();
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto const* s : zombies_) {
                sz += s->size();
            }
        }
        return sz;
    }

    bool empty() const { return size() == 0; }

    void clear() {
        for (auto& s : tl_set_.accessAllThreads()) {
            s.clear();
        }
        {
            std::unique_lock lg{zombie_mutex_};
            for (auto* s : zombies_) {
                s->clear();
            }
        }
    }

private:
    using SetType = std::unordered_set< T, Hash, KeyEqual >;

    SetType& local_set() {
        auto* s = tl_set_.get();
        if (!s) {
            auto* owner = this;
            tl_set_.reset(new SetType(),
                          [owner](SetType* set, folly::TLPDestructionMode) {
                              std::unique_lock lg{owner->zombie_mutex_};
                              owner->zombies_.push_back(set);
                          });
            s = tl_set_.get();
        }
        return *s;
    }

    // Tag with our own class so accessAllThreads() is enabled.
    struct Tag {};
    folly::ThreadLocalPtr< SetType, Tag > tl_set_;
    mutable std::mutex zombie_mutex_;
    std::vector< SetType* > zombies_;
};

} // namespace sisl
