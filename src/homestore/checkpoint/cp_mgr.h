/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
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
#include <stack>
#include <unordered_map>

#include <sisl/metrics/metrics.h>
#include <sisl/fds/enum.h>
#include <sisl/fds/utils.h>
#include <folly/SharedMutex.h>
#include <folly/coro/Task.h>
#include <folly/futures/Future.h>
#include <folly/futures/SharedPromise.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/Request.h>

#include "iomanager/coro_timer.h"

#include <homestore/meta/module_meta_blk.h>
#include <homestore/checkpoint/cp.h>

namespace homestore {
class CPMgrMetrics : public sisl::MetricsGroup {
public:
    explicit CPMgrMetrics() : sisl::MetricsGroup("CPMgr") {
        REGISTER_COUNTER(back_to_back_cps, "back to back cp");
        REGISTER_COUNTER(cp_cnt, "cp cnt");
        REGISTER_COUNTER(cp_by_timer, "Cp taken because of timer");
        REGISTER_COUNTER(cp_by_index_full, "Cp taken because of index dirty buffer/free blks fulls");
        REGISTER_HISTOGRAM(cp_latency, "cp latency (in us)");
        register_me_to_farm();
    }

    CPMgrMetrics(const CPMgrMetrics&) = delete;
    CPMgrMetrics(CPMgrMetrics&&) noexcept = delete;
    CPMgrMetrics& operator=(const CPMgrMetrics&) = delete;
    CPMgrMetrics& operator=(const CPMgrMetrics&&) noexcept = delete;
    ~CPMgrMetrics() { deregister_me_from_farm(); }
};

class CPCallbacks {
public:
    virtual ~CPCallbacks() = default;

    /// @brief CPManager calls this method when a new CP is triggered. Consumers must switch their dirty buffer
    /// collection to new_cp and prepare old cur_cp for flushing. Consumers own their per-CP state internally.
    /// @param cur_cp Pointer to the CP about to be flushed (null on first registration)
    /// @param new_cp Pointer to the new CP session to accumulate into
    virtual void on_switchover_cp(CP* cur_cp, CP* new_cp) = 0;

    /// @brief CPManager calls this once per CP flush, one consumer at a time (sequential). Consumers flush all dirty
    /// data accumulated since the previous CP. Returns true on success.
    /// @param cp CP pointer to flush
    virtual folly::coro::Task< bool > cp_flush(CP* cp) = 0;

    /// @brief After all consumers flushed the CP, CPManager calls this method to clean up any CP related structures.
    virtual void cp_cleanup(CP* cp) = 0;

    /// @brief While CP is progressing, CPManager calls this method frequently to check its flush progress.
    /// @return Returns the progress percentage of flush.
    virtual int cp_progress_percent() = 0;

    /// @brief In case CP is not progressing at all, CPManager calls this method to attempt the consumer to push harder
    /// to flush. Consumers are expected to increase any flow control to ensure flush goes faster.
    virtual void repair_slow_cp() {}
};

class CPWatchdog : private folly::AsyncTimeout {
public:
    explicit CPWatchdog(CPManager* cp_mgr);
    void set_cp(CP* cp);
    void reset_cp();
    void watch_cp();

    /// Request the watchdog to stop and return a future that completes when the timer loop exits. The returned future
    /// must be co_awaited (not .get()) since the timer loop runs on a reactor.
    folly::SemiFuture< bool > stop();

private:
    void timeoutExpired() noexcept override;

    CP* cp_{nullptr};
    CPManager* cp_mgr_;
    std::shared_mutex cp_mtx_;
    Clock::time_point last_state_ch_time_;
    uint64_t timer_sec_{0};
    uint32_t progress_pct_{0};
    std::atomic< bool > stopped_{false};
    folly::EventBase* wd_eb_{nullptr};
    folly::SharedPromise< bool > done_promise_;
};

static constexpr uint64_t cp_sb_magic{0xc0c0c01a};
static constexpr uint32_t cp_sb_version{0x1};

#pragma pack(1)
struct CPManagerSuperBlock {
    uint64_t magic{cp_sb_magic};
    uint32_t version{cp_sb_version};
    cp_id_t m_last_flushed_cp{-1};
};
#pragma pack()

class CPManager;
class CPGuard {
private:
    CP* cp_{nullptr};
    bool pushed_{false};

    // The per-thread stack lives on CPManager (see CPManager::thread_stack()).  Tying it to the manager's
    // lifetime — instead of to a thread_local or a long-lived RequestContext — guarantees that destroying the
    // manager (e.g. between gtest runs) drops every per-thread stack with it, so the next manager never sees
    // dangling CP* entries from the previous one.
    //
    // The stack (not just a single CP pointer) is necessary so that nested CPGuards within a synchronous call
    // chain all reuse the outermost CP, even if a CP switch happened after the outermost guard was taken.
    //
    // CPGuard must NOT be held across a co_await: doing so keeps enter_cnt_ non-zero across the suspension,
    // which stalls any pending CP flush until the coroutine resumes and releases the guard.

public:
    CPGuard(CPManager* mgr);
    ~CPGuard();

    CPGuard(const CPGuard& other);
    CPGuard operator=(const CPGuard& other);

    CP* operator->();
    CP* get();
};

VENUM(CPTriggerReason, uint8_t,
      Unknown = 0,               // Caller has not given a reason for it
      Timer = 1,                 // Time was up
      IndexBufferFull = 2,       // Index Dirty buffer was full
      IndexFreeBlksExceeded = 3, // Index blocks freed has hit a limit
      LogStoreFull = 4,          // Log store has gotten really full
      DataFreeBlksExceeded = 5,  // Number of free blks in data service exceeded
      UserDriven = 6,            // User explicitly requested for
);

/* It is responsible to trigger the checkpoints when all concurrent IOs are completed.
 * @ cp_type :- It is a consumer checkpoint with a base class of cp
 */
class CPManager {
    friend class CPGuard;
    friend class CPWatchdog;

public:
    static constexpr size_t max_concurent_cps{2};

private:
    CP* cur_cp_{nullptr};
    std::unique_ptr< CPMgrMetrics > metrics_;
    std::mutex trigger_cp_mtx_;
    std::unique_ptr< CPWatchdog > wd_cp_;
    ModuleMetaBlk< CPManagerSuperBlock > sb_;

    using ConsumerMap = std::unordered_map< CPConsumer, shared< CPCallbacks > >;
    ConsumerMap consumers_;
    mutable folly::SharedMutex consumers_mtx_;

    // State maintanence
    bool cp_shutdown_initiated_{false};
    bool in_flush_phase_{false};
    bool pending_trigger_cp_{false};
    folly::SharedPromise< bool > pending_trigger_cp_comp_;

    // Periodic CP-trigger timer.
    iomanager::CoroTimer cp_timer_;

public:
    // Per-thread CP stacks owned by this manager.  CPGuard pushes/pops on the stack for the calling thread; the
    // owned_stacks_ vector's lifetime equals the manager's, so destroying the manager (e.g. between gtest runs that
    // recreate it) drops every thread's stack and the next manager starts with empty stacks for all threads.
    //
    // The mutex protects only the once-per-(thread × manager) slow path that allocates a new entry.  Steady state
    // CPGuard creation hits a thread_local fast path in thread_stack() with no locking.
    struct ThreadStackInfo {
        CPManager* mgr;
        std::stack< CP* > stk;
    };

    /// Returns the CP stack owned by this manager for the calling thread, allocating it on first touch.
    /// Hot path: one TLS load + one pointer compare.  Slow path (first call from this thread for this manager,
    /// or after the previous manager was destroyed): take owned_stacks_mtx_ to allocate a new entry.
    std::stack< CP* >& thread_stack();

private:
    std::vector< std::unique_ptr< ThreadStackInfo > > owned_stacks_;
    std::mutex owned_stacks_mtx_;

public:

    /// Factory: construct and self-register via Managers::init_cp_mgr(). Call start() separately after recovery.
    static shared< CPManager > create();

    CPManager();
    virtual ~CPManager();

    /// @brief Start the CPManager, which opens or recovers the CP superblock and creates the first cp session.
    /// @param first_time_boot
    folly::coro::Task< void > start(bool first_time_boot);

    /// @brief Start the cp timer so that periodic cps are started
    void start_timer();

    /// @brief Shutdown the checkpoint manager services. It will trigger a flush, wait for the CP to be flushed
    /// and does a clean shutdown
    folly::coro::Task< void > shutdown();

    /// @brief Register a CP consumer. The consumer is immediately notified via on_switchover_cp(nullptr, cur_cp)
    /// so it can initialize its internal per-CP state. Each registered consumer receives all future CP lifecycle
    /// callbacks. Consumers own their per-CP state; nothing is stored in the CP object itself.
    /// @param consumer_id Consumer identifier a string that uniquely identifies the consumer (e.g. "IndexService")
    /// @param callbacks   Consumer's callbacks implementation (shared ownership, passed as shared<CPCallbacks>)
    void register_consumer(const CPConsumer& consumer_id, shared< CPCallbacks > callbacks);

    CPCallbacks* get_consumer(const CPConsumer& consumer_id);

    /// @brief Call this method before every IO that needs to be checkpointed. It marks the entrance of critical section
    /// of the returned CP and ensures that until it is exited, flush of the CP will not happen.
    ///
    /// @return Current CP that entered into critical section
    CP* cp_io_enter();

    /// @brief Counterpart to cp_io_enter. Once IO is done and critical section is completed, caller needs to call this.
    /// If CP flush is triggered for this CP, upon exiting the cp_io_exit and if there is no pending cp_io critical
    /// section will trigger the flush. NOTE: It is NOT required that cp_io_exit needs to be called from same thread as
    /// cp_io_enter.
    /// @param cp : Current CP that needs to exit from critical section
    void cp_io_exit(CP* cp);

    /// @brief RAII for cp_io_enter() and cp_io_exit(). This method returns a holder, which needs to be kept in context
    /// till the caller is in cp critical section. The CPHolder can be moved in that case, until it is accessed again,
    /// and releases, it will continue to be in critical section.
    /// @return CPHolder: Holder class of cp
    CPGuard cp_guard();

    /// @brief Get the current cp session.
    /// @return Returns the current CP
    CP* get_cur_cp();

    /// @brief Trigger a checkpoint flush on all subsystems registered. There is only 1 checkpoint per checkpoint
    /// manager. Checkpoint flush will wait for cp to exited all critical io sections.
    /// @param force : Do we need to force queue the checkpoint flush, in case previous checkpoint is being flushed
    /// @param reason : The reason for triggering the checkpoint, used for logging and metrics
    /// @return Returns a future which will be fulfilled when the flush is completed. The future result is true if flush
    /// is successful, false otherwise (e.g. flush failed or was not triggered because another flush is in progress and
    /// force was false).
    folly::SemiFuture< bool > trigger_cp_flush(bool force = false, CPTriggerReason reason = CPTriggerReason::Unknown);

    /// @brief Is the given cp has already finished flushing
    /// @param cp_id
    /// @return True or False if cp has flushed or not
    bool has_cp_flushed(cp_id_t cp_id) const;

private:
    void cp_ref(CP* cp);
    void create_first_cp();
    void cp_start_flush(CP* cp);
    void cleanup_cp(CP* cp);
    folly::SemiFuture< bool > do_trigger_cp_flush(bool force, bool flush_on_shutdown,
                                                  CPTriggerReason reason = CPTriggerReason::Unknown);
};

} // namespace homestore
