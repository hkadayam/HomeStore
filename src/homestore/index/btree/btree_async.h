#pragma once

#ifdef BTREE_ASYNC_MODE

#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

#include <folly/CancellationToken.h>
#include <folly/Executor.h>
#include <folly/ExceptionWrapper.h>
#include <folly/ScopeGuard.h>
#include <folly/Try.h>

#include "common/async.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
//                    BtreeTask<T> + BtreeTaskPromise<T>
// ─────────────────────────────────────────────────────────────────────────────
//
// Custom Task type so we can attach our own promise.  folly::coro::detail::TaskPromise<T> is `final`, so we cannot
// derive from it; instead the promise is written from scratch (it does not inherit from TaskPromiseBase because
// TaskPromiseBase's executor_ field is private and only friend-accessible to Async<T>).  The promise
// keeps result/continuation/executor itself, and routes nested awaits through stack_check_helper above.

template < typename T >
class BtreeTask;

template < typename T >
class BtreeTaskPromise;

namespace detail {

// Common members shared between the void and non-void promise specializations.  Pulled into a CRTP base so we do
// not have to duplicate executor/continuation/cancellation plumbing across the two specializations below.
template < typename Derived, typename T >
class BtreeTaskPromiseBase {
public:
    BtreeTaskPromiseBase() noexcept = default;

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle< Derived > h) noexcept {
            auto cont = h.promise().continuation_;
            return cont ? cont : std::noop_coroutine();
        }
        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept {
        result_.emplaceException(folly::exception_wrapper{std::current_exception()});
    }

    folly::Try< T >& result() noexcept { return result_; }

    // Stack unwinding is handled at the BtreeTask::Awaiter level: every BtreeTask→BtreeTask await routes through
    // executor->add(), which prevents the symmetric-transfer stack growth that breaks folly::coro::Task in
    // debug/ASan builds.  await_transform here just does the standard executor-binding + cancellation wrapping —
    // no extra coroutine frame is allocated per await.
    template < typename Awaitable >
    auto await_transform(Awaitable&& a) {
        return folly::coro::co_viaIfAsync(
            executor_.get_alias(), folly::coro::co_withCancellation(cancel_token_, std::forward< Awaitable >(a)));
    }

    auto await_transform(folly::coro::co_current_executor_t) noexcept {
        return folly::coro::ready_awaitable< folly::Executor* >{executor_.get()};
    }

    auto await_transform(folly::coro::co_current_cancellation_token_t) noexcept {
        return folly::coro::ready_awaitable< const folly::CancellationToken& >{cancel_token_};
    }

    void set_executor(folly::Executor::KeepAlive<> e) noexcept { executor_ = std::move(e); }
    folly::Executor::KeepAlive<> get_executor() const noexcept { return executor_; }
    void set_cancel_token(folly::CancellationToken token) noexcept { cancel_token_ = std::move(token); }
    void set_continuation(std::coroutine_handle<> c) noexcept { continuation_ = c; }

protected:
    folly::Try< T > result_;
    std::coroutine_handle<> continuation_;
    folly::Executor::KeepAlive<> executor_;
    folly::CancellationToken cancel_token_;
};

} // namespace detail

template < typename T >
class BtreeTaskPromise : public detail::BtreeTaskPromiseBase< BtreeTaskPromise< T >, T > {
public:
    using StorageType = T;

    BtreeTask< T > get_return_object() noexcept;

    template < typename U = T >
    void return_value(U&& value) {
        this->result_.emplace(std::forward< U >(value));
    }
};

template <>
class BtreeTaskPromise< void > : public detail::BtreeTaskPromiseBase< BtreeTaskPromise< void >, folly::Unit > {
public:
    using StorageType = void;

    BtreeTask< void > get_return_object() noexcept;

    void return_void() noexcept { result_.emplace(); }
};

template < typename T >
class [[nodiscard]] BtreeTask {
public:
    using promise_type = BtreeTaskPromise< T >;

private:
    using handle_t = std::coroutine_handle< promise_type >;

public:
    BtreeTask(const BtreeTask&) = delete;
    BtreeTask& operator=(const BtreeTask&) = delete;

    BtreeTask(BtreeTask&& o) noexcept : coro_{std::exchange(o.coro_, {})} {}
    BtreeTask& operator=(BtreeTask&& o) noexcept {
        if (coro_) {
            coro_.destroy();
        }
        coro_ = std::exchange(o.coro_, {});
        return *this;
    }

    ~BtreeTask() {
        if (coro_) {
            coro_.destroy();
        }
    }

    class Awaiter {
    public:
        explicit Awaiter(handle_t c) noexcept : coro_{c} {}
        Awaiter(Awaiter&& o) noexcept : coro_{std::exchange(o.coro_, {})} {}
        ~Awaiter() {
            if (coro_) {
                coro_.destroy();
            }
        }

        bool await_ready() noexcept { return false; }

        template < typename Promise >
        void await_suspend(std::coroutine_handle< Promise > caller) {
            auto& p = coro_.promise();
            p.set_continuation(caller);
            // Resume on the executor so the C++ stack unwinds before the body runs.
            p.get_executor()->add([h = coro_]() mutable { h.resume(); });
        }

        T await_resume() {
            SCOPE_EXIT {
                std::exchange(coro_, {}).destroy();
            };
            if constexpr (std::is_void_v< T >) {
                std::move(coro_.promise().result()).value();
            } else {
                return std::move(coro_.promise().result()).value();
            }
        }

    private:
        handle_t coro_;
    };

    // co_viaIfAsync is what folly's TaskPromiseBase::await_transform calls to bind a child awaitable to the
    // parent's executor.  Defining this overload makes BtreeTask awaitable from any folly::coro::Task body — the
    // parent's executor lands on our promise, then Awaiter schedules the body to run on it.
    friend Awaiter co_viaIfAsync(folly::Executor::KeepAlive<> executor, BtreeTask&& t) noexcept {
        t.coro_.promise().set_executor(std::move(executor));
        return Awaiter{std::exchange(t.coro_, {})};
    }

    friend BtreeTask co_withCancellation(folly::CancellationToken token, BtreeTask&& t) noexcept {
        t.coro_.promise().set_cancel_token(std::move(token));
        return std::move(t);
    }

private:
    friend class BtreeTaskPromise< T >;
    explicit BtreeTask(handle_t c) noexcept : coro_{c} {}
    handle_t coro_;
};

template < typename T >
BtreeTask< T > BtreeTaskPromise< T >::get_return_object() noexcept {
    return BtreeTask< T >{std::coroutine_handle< BtreeTaskPromise< T > >::from_promise(*this)};
}

inline BtreeTask< void > BtreeTaskPromise< void >::get_return_object() noexcept {
    return BtreeTask< void >{std::coroutine_handle< BtreeTaskPromise< void > >::from_promise(*this)};
}

#define CO_AWAIT co_await
#define CO_RETURN co_return
using BtreeMutex = folly::coro::Mutex;
using BtreeSharedMutex = folly::coro::SharedMutex;

// Wrappers used by btree code that must compile in either sync or async mode.  In async mode they return awaitables
// (co_await locks); in sync mode they acquire immediately and return void.  Call sites uniformly do
// `CO_AWAIT lock_shared_async(m)` — expanding to `co_await m.co_lock_shared()` in async mode, and to a plain
// `m.lock_shared()` in sync mode.
inline auto lock_async(BtreeSharedMutex& m) {
    return m.co_lock();
}
inline auto lock_shared_async(BtreeSharedMutex& m) {
    return m.co_lock_shared();
}
inline auto lock_async(BtreeMutex& m) {
    return m.co_lock();
}
} // namespace homestore

#else

#include <mutex>
#include <shared_mutex>

namespace homestore {
template < typename T >
using BtreeTask = T;
#define CO_AWAIT
#define CO_RETURN return
using BtreeMutex = std::mutex;
using BtreeSharedMutex = folly::SharedMutex;

// Sync-mode equivalents of the async wrappers.  `CO_AWAIT` is empty in sync mode, so `CO_AWAIT lock_shared_async(m)`
// collapses to just `lock_shared_async(m);` — which acquires the lock synchronously.
inline void lock_async(BtreeSharedMutex& m) {
    m.lock();
}
inline void lock_shared_async(BtreeSharedMutex& m) {
    m.lock_shared();
}
inline void lock_async(BtreeMutex& m) {
    m.lock();
}
} // namespace homestore

#endif
