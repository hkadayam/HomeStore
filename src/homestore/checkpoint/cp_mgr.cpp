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
#include <urcu.h>

#include <folly/coro/Sleep.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/async/Request.h>

#include <homestore/homestore.hpp>
#include <homestore/checkpoint/cp_mgr.h>
#include "common/homestore_assert.hpp"
#include "common/homestore_config.hpp"
#include "common/resource_mgr.hpp"
#ifdef _PRERELEASE
#include "common/crash_simulator.hpp"
#endif

#include <iomanager/iomanager.h>

namespace homestore {

////////////////////////////////////////////////////////////////////////////
// CPGuard — per-coroutine CP stack via RequestContext
////////////////////////////////////////////////////////////////////////////

// Per-coroutine stack stored in the current RequestContext (saved/restored on every co_await).
class CPStackData : public folly::RequestData {
public:
    std::stack< CP* > stack;
    bool hasCallback() override { return false; }
};

folly::RequestToken CPGuard::s_token{"homestore.cp_stack"};

std::stack< CP* >& CPGuard::cp_stack() {
    auto ctx = folly::RequestContext::get();
    auto* data = static_cast< CPStackData* >(ctx->getContextData(s_token));
    if (!data) {
        ctx->setContextData(s_token, std::make_unique< CPStackData >());
        data = static_cast< CPStackData* >(ctx->getContextData(s_token));
    }
    return data->stack;
}

////////////////////////////////////////////////////////////////////////////
// CPManager
////////////////////////////////////////////////////////////////////////////

CPManager::CPManager() : metrics_{std::make_unique< CPMgrMetrics >()}, wd_cp_{std::make_unique< CPWatchdog >(this)} {
}

CPManager::~CPManager() {
    HS_REL_ASSERT(!cur_cp_, "CPManager is tiering down without calling shutdown");
}

folly::coro::Task< void > CPManager::start(bool first_time_boot) {
    sb_ = co_await ModuleMetaBlk< CPManagerSuperBlock >::open("CPSuperBlock");
    create_first_cp();
    if (first_time_boot) {
        co_await sb_.write();
    }
}

void CPManager::start_timer() {
    LOGINFO("cp timer is set to {} usec", HS_DYNAMIC_CONFIG(generic.cp_timer_us));
    cp_timer_started_ = true;
    spawn_detached(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
        const auto interval = std::chrono::microseconds(HS_DYNAMIC_CONFIG(generic.cp_timer_us));
        while (true) {
            try {
                co_await folly::coro::co_withCancellation(cp_timer_cancel_src_.getToken(),
                                                          folly::coro::sleep(interval));
            } catch (const folly::OperationCancelled&) { break; }
            trigger_cp_flush(false, CPTriggerReason::Timer);
        }
        cp_timer_done_baton_.post();
    });
}

void CPManager::create_first_cp() {
    cur_cp_ = new CP(this);
    cur_cp_->cp_status_ = cp_status_t::cp_io_ready;
    cur_cp_->cp_id_ = sb_->m_last_flushed_cp + 1;
}

void CPManager::shutdown() {
    // Cancel the periodic timer before touching any shared CP state.
    if (cp_timer_started_) {
        cp_timer_cancel_src_.requestCancellation();
        cp_timer_done_baton_.wait();
    }

    {
        std::unique_lock< std::mutex > lk(trigger_cp_mtx_);
        cp_shutdown_initiated_ = true;
    }

#ifdef _PRERELEASE
    if (!hs()->crash_simulator().is_in_crashing_phase()) {
#endif
        LOGINFO("Trigger cp flush at CP shutdown");
        auto success = do_trigger_cp_flush(/*force=*/true, /*flush_on_shutdown=*/true, CPTriggerReason::Timer).get();
        HS_REL_ASSERT_EQ(success, true, "CP Flush failed");
        LOGINFO("Trigger cp done");
#ifdef _PRERELEASE
    }
#endif

    delete (cur_cp_);
    rcu_xchg_pointer(&cur_cp_, nullptr);

    metrics_.reset();
    if (wd_cp_) {
        wd_cp_->stop();
        wd_cp_.reset();
    }
}

void CPManager::register_consumer(CPConsumer consumer, shared< CPCallbacks > callbacks) {
    // Notify consumer of the current CP so it can initialize its own state.
    callbacks->on_switchover_cp(nullptr, cur_cp_);

    // Copy-and-swap: atomically publish the updated map.
    auto old = consumers_.load();
    auto updated = std::make_shared< ConsumerMap >(*old);
    updated->emplace(consumer, std::move(callbacks));
    consumers_.store(std::move(updated));
}

CPCallbacks* CPManager::get_consumer(CPConsumer consumer) {
    auto consumers = consumers_.load();
    auto it = consumers->find(consumer);
    return (it != consumers->end()) ? it->second.get() : nullptr;
}

[[nodiscard]] CPGuard CPManager::cp_guard() {
    return CPGuard{this};
}

CP* CPManager::cp_io_enter() {
    rcu_read_lock();
    auto cp = get_cur_cp();

    HS_DBG_ASSERT_NE((void*)cp, nullptr, "get_cur_cp returned null, cp_io_enter() after shutdown?");
    if (!cp) {
        rcu_read_unlock();
        return nullptr;
    }
    cp_ref(cp);
    rcu_read_unlock();
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
    CP* p = rcu_dereference(cur_cp_);
    return p;
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
    auto consumers = consumers_.load();
    for (auto& [id, cb] : *consumers) {
        cb->on_switchover_cp(cur_cp.get(), new_cp);
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
    rcu_xchg_pointer(&cur_cp_, new_cp);
    synchronize_rcu();

    // Unlock before cp_guard goes out of scope: exiting the CP critical section may trigger cp_start_flush,
    // and we must not hold the mutex at that point.
    lk.unlock();

    HS_PERIODIC_LOG(DEBUG, cp, "Active CP switch completed");
    return ret_fut;
}

void CPManager::cp_start_flush(CP* cp) {
    CP_PERIODIC_LOG(INFO, cp->id(), "Starting CP flush");
    cp->cp_status_ = cp_status_t::cp_flushing;

    spawn_detached(ReactorTarget::any(), [this, cp]() -> folly::coro::Task< void > {
        // Flush all consumers one at a time; sequential ordering is intentional.
        auto consumers = consumers_.load();
        for (auto& [id, cb] : *consumers) {
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
#ifdef _PRERELEASE
            if (hs()->crash_simulator().is_in_crashing_phase()) {
                hs()->crash_simulator().crash_now();
            }
#endif
        }
    });
}

void CPManager::cleanup_cp(CP* cp) {
    cp->cp_status_ = cp_status_t::cp_cleaning;
    auto consumers = consumers_.load();
    for (auto& [id, cb] : *consumers) {
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

    auto& stk = cp_stack();
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
    if (pushed_ && !cp_stack().empty()) {
        cp_stack().pop();
    }
    if (cp_) {
        cp_->cp_mgr_->cp_io_exit(cp_);
    }
}

CPGuard::CPGuard(const CPGuard& other) {
    cp_ = other.cp_;
    pushed_ = false;
    if (cp_) {
        cp_->cp_mgr_->cp_ref(cp_);
    }
}

CPGuard CPGuard::operator=(const CPGuard& other) {
    cp_ = other.cp_;
    pushed_ = false;
    if (cp_) {
        cp_->cp_mgr_->cp_ref(cp_);
    }
    return *this;
}

CP* CPGuard::operator->() {
    return get();
}

CP* CPGuard::get() {
    if (!pushed_ && cp_) {
        cp_stack().push(cp_);
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
    spawn_detached(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
        while (true) {
            try {
                co_await folly::coro::co_withCancellation(cancel_src_.getToken(),
                                                          folly::coro::sleep(std::chrono::seconds(timer_sec_)));
            } catch (const folly::OperationCancelled&) { break; }
            if (stopped_) {
                break;
            }
            cp_watchdog_timer();
        }
        done_baton_.post();
    });
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

void CPWatchdog::stop() {
    stopped_.store(true);
    cancel_src_.requestCancellation();
    done_baton_.wait();
    std::unique_lock< std::shared_mutex > lk{cp_mtx_};
    cp_ = nullptr;
}

void CPWatchdog::cp_watchdog_timer() {
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
    auto consumer = cp_mgr_->consumers();
    for (auto& [id, cb] : *consumer) {
        ++count;
        cum_pct += cb->cp_progress_percent();
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
        for (auto& [id, cb] : *consumer) {
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
