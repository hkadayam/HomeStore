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
#include "resource_mgr.h"
#include "common/async.h"

#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "sisl/logging/logging.h"

#include "hs_runtime_config.h"
#include "homestore/managers.h"
#include "event_manager.h"
#include "homestore/device/device_manager.h"        // device_mgr().total_capacity_by_type
#include "homestore/logstore/log_store_mgr.h"        // log_store_mgr().footprint_bytes / truncate
#include "homestore/replication/repl_manager.h"      // repl_mgr().truncate
#include "iomanager/iomanager.h"

namespace homestore {

uint64_t ResourceMgr::total_system_memory() {
#ifdef __linux__
    long pages = ::sysconf(_SC_PHYS_PAGES);
    long page_size = ::sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0) {
        return 0;
    }
    return to_u64(pages) * to_u64(page_size);
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    if (::sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0) != 0) {
        return 0;
    }
    return mem;
#else
    return 0;
#endif
}

void ResourceMgr::start(std::vector< DevInfo > const& devs) {
    uint64_t fast_capacity = 0;
    uint64_t data_capacity = 0;
    for (auto const& d : devs) {
        (d.dev_type == HSDevType::Fast ? fast_capacity : data_capacity) += d.dev_size;
    }

    // Process memory budget: an explicit absolute budget (process_mem_budget_bytes) wins; 0 means derive it as a
    // fraction (sys_mem_use_percent) of total system RAM.
    const uint64_t budget_bytes = HS_RUNTIME_CONFIG(resource_limits.process_mem_budget_bytes);
    const uint64_t resolved_mem_cap =
        budget_bytes > 0 ? budget_bytes
                         : (total_system_memory() * HS_RUNTIME_CONFIG(resource_limits.sys_mem_use_percent)) / 100;
    const uint64_t cache_size = (resolved_mem_cap * HS_RUNTIME_CONFIG(resource_limits.cache_size_percent)) / 100;

    LOGINFO("ResourceMgr starting: fast_capacity={} data_capacity={} mem_cap={} (absolute={}) cache_size={}",
            fast_capacity, data_capacity, resolved_mem_cap, budget_bytes > 0, cache_size);

    auto mgr = shared< ResourceMgr >{new ResourceMgr{fast_capacity, data_capacity, resolved_mem_cap, cache_size}};
    Managers::init_resource_mgr(mgr);

    EventManager::subscribe< ResourceEvent >(
        [raw = mgr.get()](ResourceEvent const& ev) { raw->on_resource_event(ev); });
    // Poll timer is NOT started here — start_timer() is called at go-live.  This keeps the boot window (and tests)
    // free of autonomous truncations.
}

void ResourceMgr::start_timer() {
    if (!Managers::has_resource_mgr()) {
        return;
    }
    // Proactive storage-pressure poll.  Fires on any IO reactor at resource_audit_timer_ms cadence and evaluates
    // the pressure rules (currently: log-stream footprint vs its fast-tier budget).  The emergency ResourceEvent
    // subscription short-circuits this cadence when a lower module hits a wall between ticks.
    ResourceMgr* self = &resource_mgr();
    self->poll_timer_.start(iomanager::ReactorTarget::any(),
                            std::chrono::milliseconds(HS_RUNTIME_CONFIG(resource_limits.resource_audit_timer_ms)),
                            iomanager::TimerKind::Recurring,
                            [self]() -> Async< void > { co_await self->poll_tick(); });
}

Async< void > ResourceMgr::prepare_shutdown() {
    if (!Managers::has_resource_mgr()) {
        co_return;
    }
    ResourceMgr& rm = resource_mgr();
    if (rm.stopping_.exchange(true)) {
        co_return; // already prepared
    }
    // Drain the poll timer (this also joins any poll-driven reclaim, since the tick awaits reclaim_log_space).
    co_await rm.poll_timer_.stop();
    // Then wait for any in-flight EMERGENCY reclaim (spawn_detached from on_resource_event, which the timer stop
    // does not join).  Acquiring the gate blocks until the running pass releases it; stopping_ prevents new passes.
    { auto lk = co_await rm.reclaim_gate_.co_scoped_lock(); }
}

Async< void > ResourceMgr::stop() {
    // Quiesce (idempotent) then drop the subscription + singleton.  A lone stop() therefore still quiesces first.
    co_await prepare_shutdown();
    EventManager::reset();
    Managers::reset_resource_mgr();
}

ResourceMgr::ResourceMgr(uint64_t fast_capacity, uint64_t data_capacity, uint64_t mem_cap, uint64_t cache_size) :
        fast_capacity_{fast_capacity}, data_capacity_{data_capacity}, mem_cap_{mem_cap}, cache_size_{cache_size} {
}

ResourceMgr::~ResourceMgr() = default;

// ──────────────────────────────────────────── Internal free helpers ──────────────────────────────────────────────────
//
// These pull dynamic state at call time — never cached.  Used only by poll_tick() and on_resource_event() rule code.
//
// TODO: once DeviceManager exposes free_capacity_by_type(HSDevType), wire those in.  For now total minus 0.

uint64_t ResourceMgr::fast_free_bytes() const {
    return fast_capacity_with_fallback();
}

uint64_t ResourceMgr::data_free_bytes() const {
    return data_capacity_;
}

uint64_t ResourceMgr::mem_free_bytes() const {
    // Placeholder — wire in process_resident_bytes() reader (Linux: /proc/self/statm; jemalloc: mallctl).
    return mem_cap_;
}

// ────────────────────────────────────────────────── Poll loop ────────────────────────────────────────────────────────

Async< void > ResourceMgr::poll_tick() {
    // Log-stream storage pressure.  The log stream lives on the fast tier; when the chunks it holds exceed the
    // configured share of FAST-device capacity, force a truncation pass.  We read actual device capacity (not the
    // summed DevInfo sizes, which are 0 when device sizes are auto-assigned).  Guarded because the poll timer can
    // fire before the log-store manager has been constructed during boot.
    if (Managers::has_log_store_mgr()) {
        const uint64_t fast_cap = device_mgr().total_capacity_by_type(HSDevType::Fast);
        const uint64_t limit = (fast_cap * HS_RUNTIME_CONFIG(resource_limits.logstream_size_limit_pct)) / 100;
        const uint64_t footprint = log_store_mgr().footprint_bytes();
        if ((limit > 0) && (footprint >= limit)) {
            LOGINFO("ResourceMgr: log-stream footprint {} >= fast-tier limit {} — reclaiming log space", footprint,
                    limit);
            co_await reclaim_log_space();
        }
    }
    // TODO: additional rule bodies as the getters land (btree dirty bytes, chunk-pool right-sizing).
    co_return;
}

// ──────────────────────────────────────────────── Event handler ──────────────────────────────────────────────────────

void ResourceMgr::on_resource_event(ResourceEvent const& ev) {
    LOGINFO("ResourceMgr received ResourceEvent kind={} source={} payload={}", static_cast< int >(ev.kind), ev.source,
            ev.payload);
    switch (ev.kind) {
    case ResourceEvent::Kind::LogStreamSpaceExhausted:
    case ResourceEvent::Kind::DiskFullOnWrite:
        // Emergency reclaim without waiting for the next poll tick.  on_resource_event runs synchronously on the
        // publisher's (mid-write) thread, so we must not block here — hand the async reclaim to a reactor.  Capture
        // a shared_ptr (self) so the detached task keeps ResourceMgr alive until it finishes, and skip once we're
        // stopping so teardown isn't chased by a fresh reclaim.
        if (!stopping_.load(std::memory_order_acquire)) {
            iomanager::spawn_detached(iomanager::ReactorTarget::any(),
                                      [self = shared_from_this()]() -> Async< void > { co_await self->reclaim_log_space(); });
        }
        break;
    case ResourceEvent::Kind::BlkAllocFailed:
    case ResourceEvent::Kind::MemAllocFailed:
        // TODO: trigger CP / drop caches / throttle writers.
        break;
    }
}

// ───────────────────────────────────────────── Storage-pressure truncation ───────────────────────────────────────────

Async< void > ResourceMgr::reclaim_log_space() {
    // Don't start a reclaim once teardown has begun.
    if (stopping_.load(std::memory_order_acquire)) {
        co_return;
    }
    // Collapse overlapping triggers (poll tick + emergency event) into a single pass.
    if (truncation_in_flight_.exchange(true)) {
        co_return;
    }
    // Hold the gate for the whole pass so prepare_shutdown() can wait for us to finish before LogStore/Repl are
    // torn down.  Re-check stopping_ under the gate: a pass that slipped past the check above must not proceed if
    // prepare_shutdown() set stopping_ while we were queued on the gate.
    auto lk = co_await reclaim_gate_.co_scoped_lock();
    if (stopping_.load(std::memory_order_acquire)) {
        truncation_in_flight_.store(false);
        co_return;
    }
    try {
        // Replicated groups first: repl truncate takes a snapshot and compacts, which advances each raft log's head
        // so the underlying log-store records become truncatable.  Then the log-store truncate reclaims the stream
        // chunks (this is also the whole job for a non-replicated deployment).
        if (Managers::has_repl_mgr()) {
            co_await repl_mgr().truncate();
        }
        if (Managers::has_log_store_mgr()) {
            co_await log_store_mgr().truncate();
        }
    } catch (const std::exception& e) {
        LOGERROR("ResourceMgr: log-space reclaim failed: {}", e.what());
    }
    truncation_in_flight_.store(false);
}

} // namespace homestore
