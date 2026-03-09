#include "iomanager.h"
#include "drive_interface.hpp"

#include <cassert>
#include <climits>
#include <stdexcept>

#include <folly/coro/Sleep.h>
#include <folly/io/async/EventBaseManager.h>
#include <sisl/logging/logging.h>

#ifdef __linux__
#  include <folly/io/async/IoUringBackend.h>
#endif

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// Thread-local reactor id — SIZE_MAX means "not a reactor thread"
// ─────────────────────────────────────────────────────────────────────────────

thread_local size_t IOManager::t_reactor_id_ = std::numeric_limits<size_t>::max();

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void IOManager::start(size_t num_reactors) {
    assert(num_reactors > 0);
    num_reactors_ = num_reactors;
    shard_ebs_.resize(num_reactors, nullptr);

    // Build an EventBaseManager whose per-thread EventBases use IoUringBackend
    // (Linux) or the default epoll/kqueue backend (non-Linux).
#ifdef __linux__
    folly::IoUringBackend::Options uring_opts;
    uring_opts.setFlags(folly::IoUringOptions::POLL_SQ)  // kernel SQ poll: no submit syscall
              .setCapacity(512);

    ebm_ = std::make_unique<folly::EventBaseManager>(
        folly::EventBase::Options().setBackendFactory(
            [uring_opts]() -> std::unique_ptr<folly::EventBaseBackendBase> {
                return std::make_unique<folly::IoUringBackend>(uring_opts);
            }));
#else
    ebm_ = std::make_unique<folly::EventBaseManager>();
#endif

    pool_ = std::make_shared<folly::IOThreadPoolExecutor>(
        num_reactors,
        std::make_shared<folly::NamedThreadFactory>("HSReactor"),
        ebm_.get());

    // Startup barrier: each reactor thread registers its EventBase at a
    // deterministic index and sets up the per-reactor drive state.
    std::atomic<size_t> assigned{0};
    folly::Baton<>      barrier;
    std::atomic<size_t> done{0};

    for (size_t i = 0; i < num_reactors; ++i) {
        pool_->add([&, num_reactors]() {
            size_t my_id  = assigned.fetch_add(1, std::memory_order_relaxed);
            t_reactor_id_ = my_id;

            // Use ebm_, not the global EventBaseManager: IOThreadPoolExecutor
            // runs each reactor thread's loop on the EventBase obtained via ebm_,
            // so we must store that same instance.
            auto* eb = ebm_->getEventBase();
            shard_ebs_[my_id] = eb;

            // Register the loopPoll runBeforeLoop callback and store the
            // IoUringBackend* in thread-local storage for IO submission.
            drive_interface_init_reactor(eb);

            LOGDEBUGMOD(iomgr, "Reactor {} started on thread {:x}", my_id,
                        std::hash<std::thread::id>{}(std::this_thread::get_id()));

            if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == num_reactors) {
                barrier.post();
            }
        });
    }

    barrier.wait();
    LOGINFO("IOManager started with {} reactors", num_reactors_);
}

void IOManager::stop() {
    if (!pool_) return;
    pool_->join();
    pool_.reset();
    shard_ebs_.clear();
    num_reactors_ = 0;
    LOGINFO("IOManager stopped");
}

IOManager::~IOManager() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

size_t IOManager::current_reactor_id() const { return t_reactor_id_; }

// ─────────────────────────────────────────────────────────────────────────────
// Target resolution
// ─────────────────────────────────────────────────────────────────────────────

folly::EventBase* IOManager::resolve_target(ReactorTarget target) const {
    switch (target.tag) {
    case ReactorTarget::Tag::Reactor: {
        assert(target.reactor_id < num_reactors_);
        return shard_ebs_[target.reactor_id];
    }
    case ReactorTarget::Tag::Current: {
        size_t rid = current_reactor_id();
        assert(rid < num_reactors_ && "ReactorTarget::Current used from non-reactor thread");
        return shard_ebs_[rid];
    }
    case ReactorTarget::Tag::Any:
        return shard_ebs_[const_cast<IOManager*>(this)->next_reactor()];
    case ReactorTarget::Tag::All:
        throw std::logic_error("resolve_target called with ReactorTarget::All — use spawn_waitable_all");
    }
    __builtin_unreachable();
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-template dispatch methods
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task<void> IOManager::yield_now() {
    co_await folly::coro::co_reschedule_on_current_executor;
}

folly::coro::Task<void> IOManager::sleep(std::chrono::milliseconds dur) {
    co_await folly::coro::sleep(dur);
}

// ─────────────────────────────────────────────────────────────────────────────
// Global singleton
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Production: raw pointer set once at init — zero-overhead dereference.
// Tests can use a different pattern if needed (e.g. reset between tests).
IOManager* g_iomgr{nullptr};
}

void init_iomgr(size_t num_reactors) {
    assert(g_iomgr == nullptr && "init_iomgr called twice");
    g_iomgr = new IOManager();
    g_iomgr->start(num_reactors);
}

void stop_iomgr() {
    assert(g_iomgr != nullptr && "stop_iomgr called without init_iomgr");
    g_iomgr->stop();
    delete g_iomgr;
    g_iomgr = nullptr;
}

IOManager& iomgr() {
    assert(g_iomgr != nullptr && "IOManager not initialized — call init_iomgr() first");
    return *g_iomgr;
}

} // namespace homestore
