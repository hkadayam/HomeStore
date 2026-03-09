#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <folly/synchronization/Baton.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/coro/Task.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>

namespace homestore {

// Extracts T from folly::coro::Task<T>
template <typename Task> struct task_value_type_impl;
template <typename T>    struct task_value_type_impl<folly::coro::Task<T>> { using type = T; };
template <typename Fn, typename... Args>
using task_value_t = typename task_value_type_impl<std::invoke_result_t<Fn, Args...>>::type;

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTarget
// ─────────────────────────────────────────────────────────────────────────────

struct ReactorTarget {
    enum class Tag { Current, Reactor, Any, All };

    Tag    tag;
    size_t reactor_id{0};  // valid only when tag == Reactor

    static ReactorTarget current()           { return {Tag::Current, 0}; }
    static ReactorTarget any()               { return {Tag::Any,     0}; }
    static ReactorTarget all()               { return {Tag::All,     0}; }
    static ReactorTarget reactor(size_t id)  { return {Tag::Reactor, id}; }
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

    IOManager(const IOManager&)            = delete;
    IOManager& operator=(const IOManager&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Start N reactor threads with a deterministic EventBase assignment.
    void start(size_t num_reactors);

    // Drain all EventBases and join all reactor threads.
    void stop();

    // ── Accessors ─────────────────────────────────────────────────────────────

    size_t num_reactors() const { return num_reactors_; }

    // O(1) array access — no lock, no map.
    folly::EventBase* reactor_for(size_t reactor_id) const {
        return shard_ebs_[reactor_id];
    }

    // Round-robin reactor selection.
    size_t next_reactor() {
        return spawn_rr_.fetch_add(1, std::memory_order_relaxed) % num_reactors_;
    }

    // Returns reactor_id of the calling thread, or num_reactors() if not on a reactor.
    size_t current_reactor_id() const;

    // ── Dispatch ──────────────────────────────────────────────────────────────

    // Fire-and-forget. Schedules task on the target reactor, does not wait.
    template <typename T>
    void spawn_detached(ReactorTarget target, folly::coro::Task<T> task);

    // Dispatch task to the target reactor and co_await the result.
    // Call from a coroutine context.
    template <typename T>
    folly::coro::Task<T> spawn_waitable(ReactorTarget target, folly::coro::Task<T> task);

    // Dispatch task to the target reactor and BLOCK the current (non-reactor) thread.
    template <typename T>
    T spawn_and_block(ReactorTarget target, folly::coro::Task<T> task);

    // Run fn(reactor_id) on every reactor sequentially.
    // fn must return folly::coro::Task<R>. Returns Task<vector<R>> for non-void R,
    // or Task<void> for void R.
    template <typename Fn>
    auto spawn_waitable_all(Fn&& fn)
        -> folly::coro::Task<std::conditional_t<
               std::is_void_v<task_value_t<Fn, size_t>>,
               void,
               std::vector<task_value_t<Fn, size_t>>>>;

    // Yield to the current EventBase loop.
    folly::coro::Task<void> yield_now();

    // Async sleep using the current EventBase timer.
    folly::coro::Task<void> sleep(std::chrono::milliseconds dur);

private:
    // Resolves a ReactorTarget to the concrete EventBase* for dispatch.
    folly::EventBase* resolve_target(ReactorTarget target) const;

    size_t num_reactors_{0};
    std::unique_ptr<folly::EventBaseManager>     ebm_;   // owns the per-thread EventBases
    std::shared_ptr<folly::IOThreadPoolExecutor> pool_;
    std::vector<folly::EventBase*> shard_ebs_;  // indexed by reactor_id, fixed after start()
    std::atomic<uint64_t>          spawn_rr_{0};

    // Reactor id of the current thread; SIZE_MAX means "not a reactor thread".
    static thread_local size_t t_reactor_id_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Template implementations (must be in the header)
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
void IOManager::spawn_detached(ReactorTarget target, folly::coro::Task<T> task) {
    auto* eb = resolve_target(target);
    // Hop to the reactor thread first, then start the coroutine there.
    // This avoids a race where the SemiFuture (from .start()) is destroyed
    // on the calling thread while the reactor thread is simultaneously
    // beginning execution — both the start and the SemiFuture discard now
    // happen on the same reactor thread.
    eb->runInEventBaseThread([eb, task = std::move(task)]() mutable {
        (void)std::move(task).scheduleOn(eb).start();
    });
}

template <typename T>
folly::coro::Task<T> IOManager::spawn_waitable(ReactorTarget target,
                                                folly::coro::Task<T> task) {
    auto* eb = resolve_target(target);
    co_return co_await std::move(task).scheduleOn(eb);
}

template <typename T>
T IOManager::spawn_and_block(ReactorTarget target, folly::coro::Task<T> task) {
    auto* eb = resolve_target(target);
    return folly::coro::blockingWait(std::move(task).scheduleOn(eb));
}

template <typename Fn>
auto IOManager::spawn_waitable_all(Fn&& fn)
    -> folly::coro::Task<std::conditional_t<
           std::is_void_v<task_value_t<Fn, size_t>>,
           void,
           std::vector<task_value_t<Fn, size_t>>>> {
    using R = task_value_t<Fn, size_t>;
    if constexpr (std::is_void_v<R>) {
        for (size_t i = 0; i < num_reactors_; ++i) {
            co_await fn(i).scheduleOn(shard_ebs_[i]);
        }
    } else {
        std::vector<R> results;
        results.reserve(num_reactors_);
        for (size_t i = 0; i < num_reactors_; ++i) {
            results.push_back(co_await fn(i).scheduleOn(shard_ebs_[i]));
        }
        co_return std::move(results);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Global singleton
// ─────────────────────────────────────────────────────────────────────────────

void       init_iomgr(size_t num_reactors);
void       stop_iomgr();
IOManager& iomgr();

// ─────────────────────────────────────────────────────────────────────────────
// Convenience free functions (mirror Rust's free functions)
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
void spawn_detached(ReactorTarget target, folly::coro::Task<T> task) {
    iomgr().spawn_detached(target, std::move(task));
}

template <typename T>
folly::coro::Task<T> spawn_waitable(ReactorTarget target, folly::coro::Task<T> task) {
    return iomgr().spawn_waitable(target, std::move(task));
}

template <typename T>
T spawn_and_block(ReactorTarget target, folly::coro::Task<T> task) {
    return iomgr().spawn_and_block(target, std::move(task));
}

template <typename Fn>
auto spawn_waitable_all(Fn&& fn)
    -> folly::coro::Task<std::conditional_t<
           std::is_void_v<task_value_t<Fn, size_t>>,
           void,
           std::vector<task_value_t<Fn, size_t>>>> {
    return iomgr().spawn_waitable_all(std::forward<Fn>(fn));
}

} // namespace homestore
