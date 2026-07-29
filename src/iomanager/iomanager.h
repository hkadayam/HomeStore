#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <folly/Try.h>
#include <folly/synchronization/Baton.h>
#include <folly/executors/IOThreadPoolExecutor.h>

#include "sisl/logging/logging.h"
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include "common/async.h"

#include <sisl/logging/logging.h>

// Forward-declare the global iomgr() accessor (fully declared lower in this header) so template helpers defined
// inside namespace iomanager — e.g. blocking_wait — can name it, since a non-dependent name must be declared
// before use in a template definition.
namespace iomanager {
class IOManager;
}
iomanager::IOManager& iomgr();

namespace iomanager {

// Extracts T from Async<T>
template < typename Task >
struct task_value_type_impl;
template < typename T >
struct task_value_type_impl< Async< T > > {
    using type = T;
};
template < typename Fn, typename... Args >
using task_value_t = typename task_value_type_impl< std::invoke_result_t< Fn, Args... > >::type;

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTarget
// ─────────────────────────────────────────────────────────────────────────────

struct ReactorTarget {
    enum class Tag { Current, Reactor, Any, All };

    Tag tag;
    size_t reactor_id{0}; // valid only when tag == Reactor

    static ReactorTarget current() { return {Tag::Current, 0}; }
    static ReactorTarget any() { return {Tag::Any, 0}; }
    static ReactorTarget all() { return {Tag::All, 0}; }
    static ReactorTarget reactor(size_t id) { return {Tag::Reactor, id}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// IOManager
//
// Owns a folly::IOThreadPoolExecutor of N threads, each with its own
// folly::EventBase (reactor loop). At startup each thread registers its own
// EventBase via a barrier so that reactor_for(id) is a stable O(1) array read
// — no lock, no map lookup on the hot path.
// ─────────────────────────────────────────────────────────────────────────────

class IOManager {
public:
    IOManager() = default;
    ~IOManager();

    IOManager(const IOManager&) = delete;
    IOManager& operator=(const IOManager&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Start N reactor threads with a deterministic EventBase assignment.
    void start(size_t num_reactors);

    // Drain all EventBases and join all reactor threads.
    void stop();

    // ── Accessors ─────────────────────────────────────────────────────────────

    size_t num_reactors() const { return num_reactors_; }

    // O(1) array access — no lock, no map.
    folly::EventBase* reactor_for(size_t reactor_id) const { return shard_ebs_[reactor_id]; }

    // Round-robin reactor selection.
    size_t next_reactor() { return spawn_rr_.fetch_add(1, std::memory_order_relaxed) % num_reactors_; }

    // Returns reactor_id of the calling thread, or num_reactors() if not on a reactor.
    size_t current_reactor_id() const;

    // ── Dispatch ──────────────────────────────────────────────────────────────

    // Fire-and-forget. Accepts a zero-arg factory that returns Task<T>.
    // The factory is invoked on the reactor thread, so reference captures
    // in the lambda body remain valid (CP.51: do not pass already-created Tasks).
    template < typename F >
    void spawn_detached(ReactorTarget target, F factory);

    // Dispatch task to the target reactor and co_await the result.
    // Call from a coroutine context.
    template < typename T >
    Async< T > spawn_waitable(ReactorTarget target, Async< T > task);

    // Dispatch task to the target reactor and BLOCK the current (non-reactor) thread.
    template < typename T >
    T spawn_and_block(ReactorTarget target, Async< T > task);

    // Run fn(reactor_id) on every reactor sequentially.
    // fn must return Async<R>. Returns Task<vector<R>> for non-void R,
    // or Task<void> for void R.
    template < typename Fn >
    auto spawn_waitable_all(Fn&& fn) -> Async< std::conditional_t< std::is_void_v< task_value_t< Fn, size_t > >, void,
                                                                   std::vector< task_value_t< Fn, size_t > > > >;

    // Yield to the current EventBase loop.
    Async< void > yield_now();

    // Async sleep using the current EventBase timer.
    Async< void > sleep(std::chrono::milliseconds dur);

    // Resolves a ReactorTarget to the concrete EventBase* for dispatch.
    folly::EventBase* resolve_target(ReactorTarget target) const;

private:
    size_t num_reactors_{0};
    std::unique_ptr< folly::EventBaseManager > ebm_; // owns the per-thread EventBases
    std::shared_ptr< folly::IOThreadPoolExecutor > pool_;
    std::vector< folly::EventBase* > shard_ebs_; // indexed by reactor_id, fixed after start()
    std::atomic< uint64_t > spawn_rr_{0};

    // Reactor id of the current thread; SIZE_MAX means "not a reactor thread".
    static thread_local size_t t_reactor_id_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Template implementations (must be in the header)
// ─────────────────────────────────────────────────────────────────────────────

template < typename F >
void IOManager::spawn_detached(ReactorTarget target, F factory) {
    auto* eb = resolve_target(target);
    // co_invoke moves factory into a heap-allocated coroutine frame so the
    // lambda closure outlives all suspension points (CP.51 fix).
    auto task = folly::coro::co_invoke(std::move(factory));
    // A detached task is fire-and-forget: nobody awaits its result, so the completion handler is the terminal owner
    // of any exception.  Rethrowing here would escape into the EventBase loop with no handler (std::terminate), so
    // we must consume it — but never SILENTLY.  Log it on the iomgr module; a task that needs failure to be fatal
    // must handle it itself before returning.
    eb->runInEventBaseThread([eb, task = std::move(task)]() mutable {
        folly::coro::co_withExecutor(eb, std::move(task)).startInlineUnsafe([](auto&& t) {
            if (t.hasException()) {
                LOGERRORMOD(iomgr, "spawn_detached task terminated with exception: {}",
                            t.exception().what().toStdString());
            }
        });
    });
}

template < typename T >
Async< T > IOManager::spawn_waitable(ReactorTarget target, Async< T > task) {
    auto* eb = resolve_target(target);
    co_return co_await folly::coro::co_withExecutor(eb, std::move(task));
}

template < typename T >
T IOManager::spawn_and_block(ReactorTarget target, Async< T > task) {
    auto* eb = resolve_target(target);
    // Use TaskWithExecutor::start(tryCallback) to run the task on the reactor
    // and signal completion via a Baton.  This avoids blockingWait, which can
    // leave a stale FunctionLoopCallback on the EventBase after it returns.
    folly::Baton<> baton;
    folly::Try< T > result;

    eb->runInEventBaseThread([eb, task = std::move(task), &baton, &result]() mutable {
        folly::coro::co_withExecutor(eb, std::move(task)).startInlineUnsafe([&baton, &result](folly::Try< T > t) {
            result = std::move(t);
            baton.post();
        });
    });

    baton.wait();
    return std::move(result).value();
}

template < typename Fn >
auto IOManager::spawn_waitable_all(Fn&& fn)
    -> Async< std::conditional_t< std::is_void_v< task_value_t< Fn, size_t > >, void,
                                  std::vector< task_value_t< Fn, size_t > > > > {
    using R = task_value_t< Fn, size_t >;
    if constexpr (std::is_void_v< R >) {
        for (size_t i = 0; i < num_reactors_; ++i) {
            co_await folly::coro::co_withExecutor(shard_ebs_[i], fn(i));
        }
    } else {
        std::vector< R > results;
        results.reserve(num_reactors_);
        for (size_t i = 0; i < num_reactors_; ++i) {
            results.push_back(co_await folly::coro::co_withExecutor(shard_ebs_[i], fn(i)));
        }
        co_return std::move(results);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void init_iomgr(size_t num_reactors);
void stop_iomgr();

// ─────────────────────────────────────────────────────────────────────────────
// Convenience free functions
// ─────────────────────────────────────────────────────────────────────────────

template < typename F >
void spawn_detached(ReactorTarget target, F factory);

template < typename T >
Async< T > spawn_waitable(ReactorTarget target, Async< T > task);

template < typename T >
T spawn_and_block(ReactorTarget target, Async< T > task);

template < typename Fn >
auto spawn_waitable_all(Fn&& fn) -> Async< std::conditional_t< std::is_void_v< task_value_t< Fn, size_t > >, void,
                                                               std::vector< task_value_t< Fn, size_t > > > >;

/// Synchronously drive `task` to completion on the calling thread.  Debug-asserts the caller is NOT on an iomgr
/// reactor — blocking a reactor deadlocks any work whose continuation routes back to that same reactor.  Use
/// this everywhere instead of folly::coro::blockingWait so reactor-context misuse is caught at debug time.
template < typename T >
T blocking_wait(Async< T >&& task) {
    DEBUG_ASSERT(iomgr().current_reactor_id() >= iomgr().num_reactors(),
                 "iomanager::blocking_wait called from reactor — would deadlock");
    return folly::coro::blockingWait(std::move(task));
}

} // namespace iomanager

// ─────────────────────────────────────────────────────────────────────────────
// Singleton accessor — declared at global scope so callers can write `iomgr()`
// without any qualification. Defined in iomanager.cpp; delegates to the
// internal pointer owned inside namespace iomanager.
// ─────────────────────────────────────────────────────────────────────────────

iomanager::IOManager& iomgr();

namespace iomanager {

template < typename F >
void spawn_detached(ReactorTarget target, F factory) {
    ::iomgr().spawn_detached(target, std::move(factory));
}

template < typename T >
Async< T > spawn_waitable(ReactorTarget target, Async< T > task) {
    return ::iomgr().spawn_waitable(target, std::move(task));
}

template < typename T >
T spawn_and_block(ReactorTarget target, Async< T > task) {
    return ::iomgr().spawn_and_block(target, std::move(task));
}

template < typename Fn >
auto spawn_waitable_all(Fn&& fn) -> Async< std::conditional_t< std::is_void_v< task_value_t< Fn, size_t > >, void,
                                                               std::vector< task_value_t< Fn, size_t > > > > {
    return ::iomgr().spawn_waitable_all(std::forward< Fn >(fn));
}

} // namespace iomanager
