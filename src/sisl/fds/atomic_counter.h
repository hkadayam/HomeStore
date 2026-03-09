/*********************************************************************************
 *
 * Author/Developer(s): Harihara Kadayam, Bryan Zimmerman
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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace sisl {

template < typename T >
class AtomicCounter {
    typedef std::decay_t< T > value_type;
    static_assert(std::is_integral< value_type >::value, "AtomicCounter needs integer");

public:
    AtomicCounter() : count_{value_type{}} {}
    AtomicCounter(const value_type count) : count_{count} {}
    AtomicCounter(const AtomicCounter& other) : count_{other.count_.load(std::memory_order_acquire)} {}
    AtomicCounter(AtomicCounter&& other) noexcept : count_{std::move(other.count_)} {}
    AtomicCounter& operator=(const AtomicCounter& rhs) {
        if (this != &rhs) { count_.store(rhs.count_.load(std::memory_order_acquire), std::memory_order_release); }
        return *this;
    }
    AtomicCounter& operator=(AtomicCounter&& rhs) noexcept {
        if (this != &rhs) { count_ = std::move(rhs.count_); }
        return *this;
    }

    value_type increment(const value_type n = 1) {
        const value_type count{count_.fetch_add(n, std::memory_order_relaxed)};
        assert((n >= 0) ? count <= std::numeric_limits< value_type >::max() - n
                        : count >= std::numeric_limits< value_type >::min() - n);
        return count + n;
    }

    bool increment_test_eq(const value_type check, const value_type n = 1) {
        const value_type count{count_.fetch_add(n, std::memory_order_release)};
        assert((n >= 0) ? count <= std::numeric_limits< value_type >::max() - n
                        : count >= std::numeric_limits< value_type >::min() - n);
        if (count == (check - n)) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    bool increment_test_ge(const value_type check, const value_type n = 1) {
        const value_type count{count_.fetch_add(n, std::memory_order_release)};
        assert((n >= 0) ? count <= std::numeric_limits< value_type >::max() - n
                        : count >= std::numeric_limits< value_type >::min() - n);
        if (count >= (check - n)) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    std::pair< bool, value_type > increment_test_ge_with_count(const value_type check, const value_type n = 1) {
        const value_type count{count_.fetch_add(n, std::memory_order_release)};
        assert((n >= 0) ? count <= std::numeric_limits< value_type >::max() - n
                        : count >= std::numeric_limits< value_type >::min() - n);
        if (count >= (check - n)) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return {true, count + n};
        }
        return {false, count + n};
    }

    value_type decrement(const value_type n = 1) {
        const value_type count{count_.fetch_sub(n, std::memory_order_release)};
        assert((n >= 0) ? count >= std::numeric_limits< value_type >::min() + n
                        : count <= std::numeric_limits< value_type >::max() + n);
        return count - n;
    }

    bool decrement_testz(const value_type n = 1) {
        const value_type count{count_.fetch_sub(n, std::memory_order_release)};
        assert((n >= 0) ? count >= std::numeric_limits< value_type >::min() + n
                        : count <= std::numeric_limits< value_type >::max() + n);
        if (count == n) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    bool decrement_test_eq(const value_type check, const value_type n = 1) {
        const value_type count{count_.fetch_sub(n, std::memory_order_release)};
        assert((n >= 0) ? count >= std::numeric_limits< value_type >::min() + n
                        : count <= std::numeric_limits< value_type >::max() + n);
        if (count == (check + n)) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    bool decrement_test_le(const value_type check, const value_type n = 1) {
        const value_type count{count_.fetch_sub(n, std::memory_order_release)};
        assert((n >= 0) ? count >= std::numeric_limits< value_type >::min() + n
                        : count <= std::numeric_limits< value_type >::max() + n);
        if (count <= (check + n)) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    std::pair< bool, value_type > decrement_test_le_with_count(const value_type check, const value_type n = 1) {
        const value_type count{count_.fetch_sub(n, std::memory_order_release)};
        assert((n >= 0) ? count >= std::numeric_limits< value_type >::min() + n
                        : count <= std::numeric_limits< value_type >::max() + n);
        if (count <= (check + n)) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return {true, count - n};
        }
        return {false, count - n};
    }

    bool test_eq(const value_type check) const {
        if (get() != check) { return false; }
        std::atomic_thread_fence(std::memory_order_acquire);
        return true;
    }

    bool test_le(const value_type check) const {
        if (get() > check) { return false; }
        std::atomic_thread_fence(std::memory_order_acquire);
        return true;
    }

    std::pair< bool, value_type > test_le_with_count(const value_type check) const {
        const value_type count{get()};
        if (count > check) { return {false, count}; }
        std::atomic_thread_fence(std::memory_order_acquire);
        return {true, count};
    }

    bool test_ge(const value_type check) const {
        if (get() < check) { return false; }
        std::atomic_thread_fence(std::memory_order_acquire);
        return true;
    }

    std::pair< bool, value_type > test_ge_with_count(const value_type check) const {
        const value_type count{get()};
        if (count < check) { return {false, count}; }
        std::atomic_thread_fence(std::memory_order_acquire);
        return {true, count};
    }

    bool testz() const {
        if (get() == 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    value_type get() const { return count_.load(std::memory_order_relaxed); }
    void set(const value_type n) { count_.store(n, std::memory_order_release); }

private:
    std::atomic< value_type > count_;
};

} // namespace sisl
