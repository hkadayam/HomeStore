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

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
// Use the signal flavor: read side is a compiler barrier (essentially free),
// write side sends SIGUSR1 to force memory barriers on reader threads.
#ifndef RCU_SIGNAL
#define RCU_SIGNAL
#endif
#ifndef URCU_API_MAP
#define URCU_API_MAP
#endif
#include <urcu/urcu.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

namespace sisl {

// ==========================================================================
// Rcu — C++ wrapper around liburcu (membarrier flavor).
//
// Hides all urcu primitives behind a single class. Thread registration is
// automatic via a thread_local whose constructor/destructor calls
// rcu_register_thread / rcu_unregister_thread.
// ==========================================================================
class Rcu {
    struct ThreadRegistry {
        ThreadRegistry() { rcu_register_thread(); }
        ~ThreadRegistry() { rcu_unregister_thread(); }
    };

    static void ensure_registered() {
        static thread_local ThreadRegistry s_reg;
        (void)s_reg;
    }

public:
    // ── Read-side RAII guard ─────────────────────────────────────────────
    class read_guard {
    public:
        read_guard() {
            ensure_registered();
            rcu_read_lock();
        }
        ~read_guard() { rcu_read_unlock(); }
        read_guard(const read_guard&) = delete;
        read_guard& operator=(const read_guard&) = delete;
    };

    // ── Write-side grace period ──────────────────────────────────────────
    static void synchronize() {
        ensure_registered();
        synchronize_rcu();
    }

    // ── Safe pointer operations ──────────────────────────────────────────

    // Read-side dereference (caller must hold read_guard).
    template < typename T >
    static T* dereference(T* p) {
        return static_cast< T* >(rcu_dereference(p));
    }

    // Atomically exchange an RCU-protected pointer (write side).
    template < typename T >
    static T* xchg_pointer(T** ptr, T* new_val) {
        // NOLINTNEXTLINE — rcu_xchg_pointer is a macro that needs a void** cast
        return static_cast< T* >(rcu_xchg_pointer(ptr, new_val));
    }

    // Publish a new pointer value to readers (write side, one-way).
    template < typename T >
    static void assign_pointer(T*& ptr, T* new_val) {
        rcu_assign_pointer(ptr, new_val);
    }

    // ── access_ptr<T> — RAII read-side accessor ─────────────────────────
    // Holds a read_guard for its lifetime, giving safe access to the
    // pointed-to object.
    template < typename T >
    class access_ptr {
    public:
        explicit access_ptr(T* p) : p_{p} {}

        access_ptr(const access_ptr&) = delete;
        access_ptr& operator=(const access_ptr&) = delete;
        access_ptr(access_ptr&& o) noexcept : p_{o.p_}, guard_{std::move(o.guard_)} { o.p_ = nullptr; }
        access_ptr& operator=(access_ptr&& o) noexcept {
            p_ = o.p_;
            guard_ = std::move(o.guard_);
            o.p_ = nullptr;
            return *this;
        }
        ~access_ptr() = default;

        const T* operator->() const { return p_; }
        T* operator->() { return p_; }
        T* get() const { return p_; }

    private:
        T* p_;
        read_guard guard_;
    };

    // ── node<T> — shared_ptr wrapper for RCU-protected data ─────────────
    template < typename T >
    struct node {
        std::shared_ptr< T > val;

        template < typename... Args >
        node(Args&&... args) : val{std::make_shared< T >(std::forward< Args >(args)...)} {}

        std::shared_ptr< T > get() { return val; }
    };

    // ── data<T> — RCU-protected data: lock-free reads, serialised writes ─
    template < typename T >
    class data {
    public:
        template < typename... Args >
        data(Args&&... args) {
            node_.store(new node< T >(std::forward< Args >(args)...), std::memory_order_release);
        }

        data(const data&) = delete;
        data(data&&) = delete;
        data& operator=(const data&) = delete;
        data& operator=(data&&) = delete;

        ~data() {
            delete node_.load(std::memory_order_acquire);
            delete old_node_;
        }

        // Read-side: returns an accessor holding the RCU read-side guard.
        access_ptr< T > get() const {
            T* p = node_.load(std::memory_order_acquire)->val.get();
            return access_ptr< T >(p);
        }

        // Returns the current node (caller must be in a read-side section
        // or otherwise ensure stability).
        node< T >* get_node() const { return node_.load(std::memory_order_acquire); }

        // Atomically replace stored value; returns old shared_ptr after grace period.
        template < typename... Args >
        std::shared_ptr< T > make_and_exchange(Args&&... args) {
            auto* new_nd = new node< T >(std::forward< Args >(args)...);
            auto* old_nd = node_.exchange(new_nd, std::memory_order_acq_rel);
            Rcu::synchronize();
            auto ret = old_nd->get();
            delete old_nd;
            return ret;
        }

        // Two-phase update: make() installs a new node (saving the old),
        // exchange() waits for a grace period and returns the old value.
        template < typename... Args >
        void make(Args&&... args) {
            old_node_ = node_.load(std::memory_order_acquire);
            node_.store(new node< T >(std::forward< Args >(args)...), std::memory_order_release);
        }

        std::shared_ptr< T > exchange() {
            if (old_node_ == nullptr) {
                return nullptr;
            }
            Rcu::synchronize();
            auto ret = old_node_->get();
            delete old_node_;
            old_node_ = nullptr;
            return ret;
        }

    private:
        std::atomic< node< T >* > node_{nullptr};
        node< T >* old_node_{nullptr};
    };

    // ── scoped_ptr<T, Args...> — RCU-protected pointer with CoW update ──
    template < typename T, typename... Args >
    class scoped_ptr {
    public:
        template < class... Args1 >
        scoped_ptr(Args1&&... args) : args_(std::forward< Args1 >(args)...) {
            cur_obj_.store(new T(std::forward< Args1 >(args)...), std::memory_order_release);
        }

        scoped_ptr(scoped_ptr const&) = delete;
        scoped_ptr(scoped_ptr&&) = delete;
        scoped_ptr& operator=(scoped_ptr const&) = delete;
        scoped_ptr& operator=(scoped_ptr&&) = delete;

        ~scoped_ptr() {
            Rcu::synchronize();
            delete cur_obj_.load(std::memory_order_acquire);
        }

        void read(const auto& cb) const {
            read_guard guard;
            cb(static_cast< const T* >(cur_obj_.load(std::memory_order_acquire)));
        }

        access_ptr< T > access() const {
            T* p = cur_obj_.load(std::memory_order_acquire);
            return access_ptr< T >(p);
        }

        void update(const auto& edit_cb) {
            T* old_obj;
            {
                std::scoped_lock l(updater_mutex_);
                T* new_obj;
                {
                    read_guard guard;
                    new_obj = new T(*cur_obj_.load(std::memory_order_acquire));
                }
                edit_cb(new_obj);
                old_obj = cur_obj_.exchange(new_obj, std::memory_order_acq_rel);
            }
            Rcu::synchronize();
            if (old_obj) {
                delete old_obj;
            }
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
            if (sync_rcu_now) {
                Rcu::synchronize();
            }
            return old_obj;
        }

        std::atomic< T* > cur_obj_{nullptr};
        std::mutex updater_mutex_;
        std::tuple< Args... > args_;
    };
};
} // namespace sisl
