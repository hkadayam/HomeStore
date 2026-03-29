/*********************************************************************************
 *
 * Author/Developer(s): Harihara Kadayam, Aditya Marella
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

#include <atomic>
#include <memory>
#include <mutex>
#include <tuple>

#include <folly/synchronization/Rcu.h>

namespace sisl {

// ---------------------------------------------------------------------------
// _urcu_access_ptr<T>
// Holds a folly::rcu_reader guard for the duration of its lifetime, providing
// safe read-side access to an RCU-protected pointer.
// ---------------------------------------------------------------------------
template < typename T >
class _urcu_access_ptr {
public:
    _urcu_access_ptr(T* p, std::unique_ptr< std::unique_lock< folly::rcu_domain > > guard) :
            p_{p}, guard_{std::move(guard)} {}

    _urcu_access_ptr(const _urcu_access_ptr&) = delete;
    _urcu_access_ptr& operator=(const _urcu_access_ptr&) = delete;
    _urcu_access_ptr(_urcu_access_ptr&&) noexcept = default;
    _urcu_access_ptr& operator=(_urcu_access_ptr&&) noexcept = default;
    ~_urcu_access_ptr() = default;

    const T* operator->() const { return p_; }
    T* operator->() { return p_; }
    T* get() const { return p_; }

private:
    T* p_;
    std::unique_ptr< std::unique_lock< folly::rcu_domain > > guard_;
};

// ---------------------------------------------------------------------------
// urcu_node<T>
// Thin wrapper holding a shared_ptr<T>; exists to keep the same interface as
// the liburcu version so callers using get_node()->get() don't change.
// ---------------------------------------------------------------------------
template < typename T >
struct urcu_node {
    std::shared_ptr< T > val;

    template < typename... Args >
    urcu_node(Args&&... args) : val{std::make_shared< T >(std::forward< Args >(args)...)} {}

    std::shared_ptr< T > get() { return val; }
};

// ---------------------------------------------------------------------------
// urcu_data<T>
// RCU-protected data: concurrent lock-free reads, serialised writes.
// ---------------------------------------------------------------------------
template < typename T >
class urcu_data {
public:
    template < typename... Args >
    urcu_data(Args&&... args) {
        node_.store(new urcu_node< T >(std::forward< Args >(args)...), std::memory_order_release);
    }

    urcu_data(const urcu_data&) = delete;
    urcu_data(urcu_data&&) = delete;
    urcu_data& operator=(const urcu_data&) = delete;
    urcu_data& operator=(urcu_data&&) = delete;

    ~urcu_data() {
        delete node_.load(std::memory_order_acquire);
        delete old_node_;
    }

    // Read-side: returns an accessor that holds the RCU read-side guard.
    _urcu_access_ptr< T > get() const {
        auto guard = std::make_unique< std::unique_lock< folly::rcu_domain > >(folly::rcu_default_domain());
        T* p = node_.load(std::memory_order_acquire)->val.get();
        return _urcu_access_ptr< T >(p, std::move(guard));
    }

    // Returns the current node pointer (caller must be in an RCU read-side
    // critical section or hold another reference ensuring stability).
    urcu_node< T >* get_node() const { return node_.load(std::memory_order_acquire); }

    // Atomically replace stored value; returns old shared_ptr after grace period.
    template < typename... Args >
    std::shared_ptr< T > make_and_exchange(Args&&... args) {
        auto* new_node = new urcu_node< T >(std::forward< Args >(args)...);
        auto* old_node = node_.exchange(new_node, std::memory_order_acq_rel);
        folly::rcu_synchronize();
        auto ret = old_node->get();
        delete old_node;
        return ret;
    }

    // Two-phase update: make() installs a new node (saving the old),
    // exchange() waits for a grace period and returns the old value.
    template < typename... Args >
    void make(Args&&... args) {
        old_node_ = node_.load(std::memory_order_acquire);
        node_.store(new urcu_node< T >(std::forward< Args >(args)...), std::memory_order_release);
    }

    std::shared_ptr< T > exchange() {
        if (old_node_ == nullptr) { return nullptr; }
        folly::rcu_synchronize();
        auto ret = old_node_->get();
        delete old_node_;
        old_node_ = nullptr;
        return ret;
    }

private:
    std::atomic< urcu_node< T >* > node_{nullptr};
    urcu_node< T >* old_node_{nullptr};
};

// ---------------------------------------------------------------------------
// urcu_scoped_ptr<T, Args...>
// RCU-protected single pointer with copy-on-write update semantics.
// ---------------------------------------------------------------------------
template < typename T, typename... Args >
class urcu_scoped_ptr {
public:
    template < class... Args1 >
    urcu_scoped_ptr(Args1&&... args) : args_(std::forward< Args1 >(args)...) {
        cur_obj_.store(new T(std::forward< Args1 >(args)...), std::memory_order_release);
    }

    urcu_scoped_ptr(urcu_scoped_ptr const&) = delete;
    urcu_scoped_ptr(urcu_scoped_ptr&&) = delete;
    urcu_scoped_ptr& operator=(urcu_scoped_ptr const&) = delete;
    urcu_scoped_ptr& operator=(urcu_scoped_ptr&&) = delete;

    ~urcu_scoped_ptr() {
        folly::rcu_synchronize();
        delete cur_obj_.load(std::memory_order_acquire);
    }

    void read(const auto& cb) const {
        auto guard = std::make_unique< std::unique_lock< folly::rcu_domain > >(folly::rcu_default_domain());
        cb(static_cast< const T* >(cur_obj_.load(std::memory_order_acquire)));
    }

    _urcu_access_ptr< T > access() const {
        auto guard = std::make_unique< std::unique_lock< folly::rcu_domain > >(folly::rcu_default_domain());
        T* p = cur_obj_.load(std::memory_order_acquire);
        return _urcu_access_ptr< T >(p, std::move(guard));
    }

    void update(const auto& edit_cb) {
        T* old_obj;
        {
            std::scoped_lock l(updater_mutex_);
            T* new_obj;
            {
                std::unique_lock< folly::rcu_domain > guard{folly::rcu_default_domain()};
                new_obj = new T(*cur_obj_.load(std::memory_order_acquire));
            }
            edit_cb(new_obj);
            old_obj = cur_obj_.exchange(new_obj, std::memory_order_acq_rel);
        }
        folly::rcu_synchronize();
        if (old_obj) { delete old_obj; }
    }

    T* make_and_exchange(const bool sync_rcu_now = true) {
        return _make_and_exchange(sync_rcu_now, args_, std::index_sequence_for< Args... >());
    }

private:
    template < std::size_t... Is >
    T* _make_and_exchange(const bool sync_rcu_now, const std::tuple< Args... >& tuple,
                          std::index_sequence< Is... >) {
        auto* new_obj = new T(std::get< Is >(tuple)...);
        auto* old_obj = cur_obj_.exchange(new_obj, std::memory_order_acq_rel);
        if (sync_rcu_now) { folly::rcu_synchronize(); }
        return old_obj;
    }

    std::atomic< T* > cur_obj_{nullptr};
    std::mutex updater_mutex_;
    std::tuple< Args... > args_;
};

// ---------------------------------------------------------------------------
// urcu_ctl — convenience wrappers; thread register/unregister are no-ops
// with folly::rcu (it manages thread registration automatically).
// ---------------------------------------------------------------------------
class urcu_ctl {
public:
    static void register_rcu() {}   // no-op: folly manages thread registration
    static void unregister_rcu() {} // no-op
    static void sync_rcu() { folly::rcu_synchronize(); }
};

} // namespace sisl
