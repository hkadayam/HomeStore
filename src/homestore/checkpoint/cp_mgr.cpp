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
#include <folly/io/async/EventBaseManager.h>
#include "common/async.h"
#include <folly/io/async/Request.h>
#include "sisl/fds/rcu.h"

#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/base/homestore_assert.h"
#include "homestore/base/homestore_config.h"
#include "homestore/managers.h"
// TODO: re-enable once HomeStore singleton and crash_simulator are ported to new iomanager
// #include "homestore/homestore.h"
// #include "homestore/base/resource_mgr.h"
// #ifdef _PRERELEASE
// #include "homestore/base/crash_simulator.h"
// #endif

#include "iomanager/iomanager.h"

namespace homestore {

using namespace iomanager;
using sisl::IoBufOwn;

////////////////////////////////////////////////////////////////////////////
// CPGuard — per-thread CP stack owned by CPManager
////////////////////////////////////////////////////////////////////////////

// Cached pointer to the calling thread's ThreadStackInfo.  Lives for the program's lifetime per thread, but is
// re-checked against the current manager so a stale entry from a destroyed manager is replaced transparently.
static thread_local CPManager::ThreadStackInfo* t_cp_info_{nullptr};

std::stack< CP* >& CPManager::thread_stack() {
    if (t_cp_info_ && t_cp_info_->mgr == this) {
        return t_cp_info_->stk;
    }

    // Slow path: first touch from this thread for this manager (or previous manager was destroyed).
    auto info = std::make_unique< ThreadStackInfo >();
    info->mgr = this;
    auto* raw = info.get();
    {
        std::lock_guard lg{owned_stacks_mtx_};
        owned_stacks_.push_back(std::move(info));
    }
    t_cp_info_ = raw;
    return raw->stk;
}

////////////////////////////////////////////////////////////////////////////
// CPManager
////////////////////////////////////////////////////////////////////////////

shared< CPManager > CPManager::create() {
    auto mgr = shared< CPManager >(new CPManager());
    Managers::init_cp_mgr(mgr);
    return mgr;
}

CPManager::CPManager() : metrics_{std::make_unique< CPMgrMetrics >()}, wd_cp_{std::make_unique< CPWatchdog >(this)} {
}

CPManager::~CPManager() {
    HS_REL_ASSERT(!cur_cp_, "CPManager is tiering down without calling shutdown");
}

Async< void > CPManager::start(bool first_time_boot) {
    sb_ = co_await ModuleMetaBlk< CPManagerSuperBlock >::open("CPSuperBlock");
    create_first_cp();
    if (first_time_boot) {
        co_await sb_.write();
    }
    co_return;
}

void CPManager::start_timer() {
    const auto interval = std::chrono::microseconds(HS_DYNAMIC_CONFIG(generic.cp_timer_us));
    LOGINFO("cp timer is set to {} usec", interval.count());
    cp_timer_.start(ReactorTarget::any(), interval, iomanager::TimerKind::Recurring, [this]() -> Async< void > {
        trigger_cp_flush(false, CPTriggerReason::Timer);
        co_return;
    });
}

void CPManager::create_first_cp() {
    cur_cp_ = new CP(this);
    cur_cp_->cp_status_ = cp_status_t::cp_io_ready;
    cur_cp_->cp_id_ = sb_->m_last_flushed_cp + 1;
}

Async< void > CPManager::shutdown() {
    // Request cancellation of the periodic timer (non-blocking). The timer coroutine will exit on its own.
    folly::SemiFuture< bool > wd_done = folly::SemiFuture< bool >::makeEmpty();
    cp_timer_.request_stop();

    // Request the watchdog to stop (non-blocking). We co_await its completion after the flush.
    if (wd_cp_) {
        wd_done = wd_cp_->stop();
    }

    {
        std::unique_lock< std::mutex > lk(trigger_cp_mtx_);
        cp_shutdown_initiated_ = true;
    }

    // TODO: re-enable crash_simulator guard once HomeStore singleton is ported
    // #ifdef _PRERELEASE
    //     if (!hs()->crash_simulator().is_in_crashing_phase()) {
    // #endif
    LOGINFO("Trigger cp flush at CP shutdown");
    auto success = co_await do_trigger_cp_flush(/*force=*/true, /*flush_on_shutdown=*/true, CPTriggerReason::Timer);
    HS_REL_ASSERT_EQ(success, true, "CP Flush failed");
    LOGINFO("Trigger cp done");
    // #ifdef _PRERELEASE
    //     }
    // #endif

    // Wait for watchdog and timer coroutines to exit before tearing down state.
    if (wd_done.valid()) {
        co_await std::move(wd_done);
    }
    co_await cp_timer_.stop();

    // Don't reset wd_cp_ here: the co_await awaiter above still holds a Future
    // referencing done_promise_'s Core. Destroying wd_cp_ would drop the
    // SharedPromise refcount, and the awaiter's destructor would then crash
    // with a double-detach. Let ~CPManager handle wd_cp_ lifetime instead.

    auto* old_cp = sisl::Rcu::xchg_pointer(&cur_cp_, static_cast< CP* >(nullptr));
    sisl::Rcu::synchronize();
    delete old_cp;

    metrics_.reset();
}

void CPManager::register_consumer(const CPConsumer& consumer, shared< CPCallbacks > callbacks) {
    // Notify consumer of the current CP so it can initialize its own state.
    callbacks->on_switchover_cp(nullptr, cur_cp_);

    std::unique_lock lk(consumers_mtx_);
    consumers_.emplace(consumer, std::move(callbacks));
}

CPCallbacks* CPManager::get_consumer(const CPConsumer& consumer) {
    std::shared_lock lk(consumers_mtx_);
    auto it = consumers_.find(consumer);
    return (it != consumers_.end()) ? it->second.get() : nullptr;
}

[[nodiscard]] CPGuard CPManager::cp_guard() {
    return CPGuard{this};
}

CP* CPManager::cp_io_enter() {
    sisl::Rcu::read_guard guard;
    auto cp = get_cur_cp();

    HS_DBG_ASSERT_NE((void*)cp, nullptr, "get_cur_cp returned null, cp_io_enter() after shutdown?");
    if (!cp) {
        return nullptr;
    }
    cp_ref(cp);
    return cp;
}

void CPManager::cp_ref(CP* cp) {
    cp->enter_cnt_.increment(1);
#ifndef NDEBUG
    auto status = cp->cp_status_.load();
    HS_DBG_ASSERT((status == cp_status_t::cp_io_ready || status == cp_status_t::cp_trigger ||
                   status == cp_status_t::cp_flush_prepare),
                  "cp status {}", status);
#endif
}

void CPManager::cp_io_exit(CP* cp) {
    HS_DBG_ASSERT_NE(cp->cp_status_, cp_status_t::cp_flushing);
    if (cp->enter_cnt_.decrement_testz(1) && (cp->cp_status_ == cp_status_t::cp_flush_prepare)) {
        wd_cp_->set_cp(cp);
        cp_start_flush(cp);
    }
}

CP* CPManager::get_cur_cp() {
    return sisl::Rcu::dereference(cur_cp_);
}

folly::SemiFuture< bool > CPManager::trigger_cp_flush(bool force, CPTriggerReason reason) {
    return do_trigger_cp_flush(force, /*flush_on_shutdown=*/false, reason);
}

folly::SemiFuture< bool > CPManager::do_trigger_cp_flush(bool force, bool flush_on_shutdown, CPTriggerReason reason) {
    std::unique_lock< std::mutex > lk(trigger_cp_mtx_);

    if (in_flush_phase_) {
        // If we are already flushing, we create a back-to-back CP queue only if force is set and if we are not in
        // shutdown phase. Triggering a back-2-back CP in shutdown state is dangerous, as it can cause the CPManager to
        // be destructed while back-2-back CP is triggered.
        if (force && (!cp_shutdown_initiated_ || flush_on_shutdown)) {
            if (!pending_trigger_cp_) {
                pending_trigger_cp_ = true;
                pending_trigger_cp_comp_ = std::move(folly::SharedPromise< bool >{});
            }
            return pending_trigger_cp_comp_.getSemiFuture();
        } else {
            return folly::makeSemiFuture< bool >(false);
        }
    }
    in_flush_phase_ = true;

    folly::SemiFuture< bool > ret_fut = folly::SemiFuture< bool >::makeEmpty();
    auto cur_cp = cp_guard();
    cur_cp->cp_status_ = cp_status_t::cp_trigger;
    cur_cp->is_on_shutdown_ = flush_on_shutdown;
    CP_PERIODIC_LOG(INFO, cur_cp->id(), "Time to flush the CP {}", cur_cp->to_string());
    COUNTER_INCREMENT(*metrics_, cp_cnt, 1);
    wd_cp_->set_cp(cur_cp.get());

    // Allocate a new cp and ask consumers to switchover to new cp.
    auto new_cp = new CP(this);
    new_cp->cp_id_ = cur_cp->cp_id_ + 1;

    CP_PERIODIC_LOG(DEBUG, new_cp->id(), "Create New CP session");
    {
        std::shared_lock lk(consumers_mtx_);
        for (auto& [_, cb] : consumers_) {
            cb->on_switchover_cp(cur_cp.get(), new_cp);
        }
    }

    if (pending_trigger_cp_) {
        // Triggered because of back-2-back CP, use the pending promise/future.
        cur_cp->comp_promise_ = std::move(pending_trigger_cp_comp_);
        pending_trigger_cp_ = false;
    } else {
        cur_cp->comp_promise_ = std::move(folly::SharedPromise< bool >{});
    }
    ret_fut = cur_cp->comp_promise_.getSemiFuture();

    cur_cp->cp_status_ = cp_status_t::cp_flush_prepare;
    new_cp->cp_status_ = cp_status_t::cp_io_ready;
    sisl::Rcu::xchg_pointer(&cur_cp_, new_cp);
    sisl::Rcu::synchronize();

    // Unlock before cp_guard goes out of scope: exiting the CP critical section may trigger cp_start_flush,
    // and we must not hold the mutex at that point.
    lk.unlock();

    HS_PERIODIC_LOG(DEBUG, cp, "Active CP switch completed");
    return ret_fut;
}

void CPManager::cp_start_flush(CP* cp) {
    CP_PERIODIC_LOG(INFO, cp->id(), "Starting CP flush");
    cp->cp_status_ = cp_status_t::cp_flushing;

    spawn_detached(ReactorTarget::any(), [this, cp]() -> Async< void > {
        // Flush all consumers one at a time; sequential ordering is intentional.
        // Snapshot callbacks under shared lock, then release before co_await.
        std::vector< shared< CPCallbacks > > cbs;
        {
            std::shared_lock lk(consumers_mtx_);
            for (auto& [_, cb] : consumers_) {
                cbs.push_back(cb);
            }
        }
        for (auto& cb : cbs) {
            co_await cb->cp_flush(cp);
        }

        // Persist superblock with updated last-flushed CP id.
        HS_DBG_ASSERT_EQ(cp->cp_status_, cp_status_t::cp_flushing);
        cp->cp_status_ = cp_status_t::cp_flush_done;
        ++(sb_->m_last_flushed_cp);
        co_await sb_.write();

        CP_PERIODIC_LOG(INFO, cp->id(), "CP Flush completed");
        cleanup_cp(cp);

        // Move the promise out before deleting cp: fulfilling it may wake shutdown and destroy CPManager.
        auto promise = std::move(cp->comp_promise_);
        wd_cp_->reset_cp();
        bool is_shutdown_cp = cp->is_on_shutdown_;
        delete cp;

        bool trigger_back_2_back_cp{false};
        {
            std::unique_lock< std::mutex > lk(trigger_cp_mtx_);
            in_flush_phase_ = false;
            trigger_back_2_back_cp = pending_trigger_cp_;
        }

        promise.setValue(true);
        if (!is_shutdown_cp) {
            // Do not access CPManager state after this unless trigger_back_2_back_cp is true:
            // fulfilling the promise above may allow shutdown to destroy CPManager.
            if (trigger_back_2_back_cp) {
                HS_PERIODIC_LOG(INFO, cp, "Triggering back to back CP");
                COUNTER_INCREMENT(*metrics_, back_to_back_cps, 1);
                trigger_cp_flush(false, CPTriggerReason::Timer);
            }
            // TODO: re-enable crash_simulator guard once HomeStore singleton is ported
            // #ifdef _PRERELEASE
            //             if (hs()->crash_simulator().is_in_crashing_phase()) {
            //                 hs()->crash_simulator().crash_now();
            //             }
            // #endif
        }
    });
}

void CPManager::cleanup_cp(CP* cp) {
    cp->cp_status_ = cp_status_t::cp_cleaning;
    std::shared_lock lk(consumers_mtx_);
    for (auto& [id, cb] : consumers_) {
        cb->cp_cleanup(cp);
    }
}

bool CPManager::has_cp_flushed(cp_id_t cp_id) const {
    return (sb_->m_last_flushed_cp >= cp_id);
}

////////////////////////////////////////////////////////////////////////////
// CPGuard
////////////////////////////////////////////////////////////////////////////

CPGuard::CPGuard(CPManager* mgr) {
    if (mgr == nullptr) {
        return;
    }

    auto& stk = mgr->thread_stack();
    if (stk.empty()) {
        cp_ = mgr->cp_io_enter();
    } else {
        cp_ = stk.top();
        cp_->cp_mgr_->cp_ref(cp_);
    }
    stk.push(cp_);
    pushed_ = true;
}

CPGuard::~CPGuard() {
    if (pushed_ && cp_) {
        auto& stk = cp_->cp_mgr_->thread_stack();
        if (!stk.empty()) {
            stk.pop();
        }
    }
    if (cp_) {
        cp_->cp_mgr_->cp_io_exit(cp_);
    }
}

CPGuard::CPGuard(const CPGuard& other) : cp_{other.cp_}, pushed_{false} {
    if (cp_) {
        cp_->cp_mgr_->cp_ref(cp_);
    }
}

CPGuard CPGuard::operator=(const CPGuard& other) {
    if (this != &other) {
        cp_ = other.cp_;
        pushed_ = false;
        if (cp_) {
            cp_->cp_mgr_->cp_ref(cp_);
        }
    }
    return *this;
}

CP* CPGuard::operator->() {
    return get();
}

CP* CPGuard::get() {
    if (!pushed_ && cp_) {
        cp_->cp_mgr_->thread_stack().push(cp_);
        pushed_ = true;
    }
    return cp_;
}

////////////////////////////////////////////////////////////////////////////
// CPWatchdog
////////////////////////////////////////////////////////////////////////////

CPWatchdog::CPWatchdog(CPManager* cp_mgr) :
        cp_{nullptr}, cp_mgr_{cp_mgr}, timer_sec_{HS_DYNAMIC_CONFIG(generic.cp_watchdog_timer_sec)} {
    LOGINFO("CP watchdog timer setting to : {} seconds", timer_sec_);
    wd_eb_ = iomgr().reactor_for(0);
    attachEventBase(wd_eb_);
    wd_eb_->runInEventBaseThread([this]() { scheduleTimeout(timer_sec_ * 1000); });
}

void CPWatchdog::reset_cp() {
    std::unique_lock< std::shared_mutex > lk{cp_mtx_};
    cp_ = nullptr;
    progress_pct_ = 0;
}

void CPWatchdog::set_cp(CP* cp) {
    std::unique_lock< std::shared_mutex > lk{cp_mtx_};
    cp_ = cp;
    last_state_ch_time_ = Clock::now();
}

folly::SemiFuture< bool > CPWatchdog::stop() {
    stopped_.store(true);
    wd_eb_->runInEventBaseThread([this]() {
        cancelTimeout();
        done_promise_.setValue(true);
    });
    return done_promise_.getSemiFuture();
}

void CPWatchdog::timeoutExpired() noexcept {
    if (stopped_.load()) {
        done_promise_.setValue(true);
        return;
    }
    watch_cp();
    scheduleTimeout(timer_sec_ * 1000);
}

void CPWatchdog::watch_cp() {
    std::unique_lock< std::shared_mutex > lk{cp_mtx_};

    if (cp_ == nullptr) {
        return;
    }
    const auto status = cp_->get_status();
    if ((status != cp_status_t::cp_flush_prepare) || (status != cp_status_t::cp_flushing)) {
        return;
    }

    uint32_t cum_pct{0};
    uint32_t count{0};
    {
        std::shared_lock lk(cp_mgr_->consumers_mtx_);
        for (auto& [id, cb] : cp_mgr_->consumers_) {
            ++count;
            cum_pct += cb->cp_progress_percent();
        }
    }
    if (progress_pct_ > cum_pct / count) {
        progress_pct_ = cum_pct / count;
        return;
    }

    if (get_elapsed_time_ms(last_state_ch_time_) >= timer_sec_ * 1000) {
        LOGINFO("cp progress percent {} is not changed. time elapsed {}, cp state={} ", progress_pct_,
                get_elapsed_time_ms(last_state_ch_time_), cp_->to_string());
    }

    uint32_t max_time_multiplier = 12;
    if (get_elapsed_time_ms(last_state_ch_time_) < max_time_multiplier * timer_sec_ * 1000) {
        uint32_t repair_attempted{0};
        std::shared_lock lk2(cp_mgr_->consumers_mtx_);
        for (auto& [id, cb] : cp_mgr_->consumers_) {
            const auto pct = cb->cp_progress_percent();
            if (pct != 100) {
                cb->repair_slow_cp();
                ++repair_attempted;
            }
            if (repair_attempted) {
                return;
            }
        }

        HS_REL_ASSERT(0, "cp seems to be stuck. CP State={} total time elapsed {}", cp_->to_string(),
                      get_elapsed_time_ms(last_state_ch_time_));
    }
}

} // namespace homestore
