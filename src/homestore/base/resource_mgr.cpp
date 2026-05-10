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

#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "sisl/logging/logging.h"

#include "homestore_config.h"
#include "homestore/managers.h"
#include "event_manager.h"
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

void ResourceMgr::start(std::vector< DevInfo > const& devs, std::optional< uint64_t > mem_cap) {
    uint64_t fast_capacity = 0;
    uint64_t data_capacity = 0;
    for (auto const& d : devs) {
        (d.dev_type == HSDevType::Fast ? fast_capacity : data_capacity) += d.dev_size;
    }

    const uint64_t resolved_mem_cap = mem_cap.value_or(
        (total_system_memory() * HS_DYNAMIC_CONFIG(resource_limits.sys_mem_use_percent)) / 100);
    const uint64_t cache_size = (resolved_mem_cap * HS_DYNAMIC_CONFIG(resource_limits.cache_size_percent)) / 100;

    LOGINFO("ResourceMgr starting: fast_capacity={} data_capacity={} mem_cap={} (caller_provided={}) cache_size={}",
            fast_capacity, data_capacity, resolved_mem_cap, mem_cap.has_value(), cache_size);

    auto mgr = shared< ResourceMgr >{new ResourceMgr{fast_capacity, data_capacity, resolved_mem_cap, cache_size}};
    Managers::init_resource_mgr(mgr);

    EventManager::subscribe< ResourceEvent >(
        [raw = mgr.get()](ResourceEvent const& ev) { raw->on_resource_event(ev); });

    // TODO: start poll_timer_ once the manager getters it depends on are in place
    // (cow_btree_mgr().total_dirty_bytes(), log_store_mgr().total_log_bytes(),
    // device_mgr().free_capacity_by_type(), chunk_pool().resize_to_fit()).  For now the poll loop
    // infrastructure is wired but the rule bodies are stubs.
}

void ResourceMgr::stop() {
    EventManager::reset();
    Managers::reset_resource_mgr();
}

ResourceMgr::ResourceMgr(uint64_t fast_capacity, uint64_t data_capacity, uint64_t mem_cap, uint64_t cache_size) :
        fast_capacity_{fast_capacity},
        data_capacity_{data_capacity},
        mem_cap_{mem_cap},
        cache_size_{cache_size} {}

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

folly::coro::Task< void > ResourceMgr::poll_tick() {
    // TODO: implement rule bodies once the per-manager getters land:
    //   • check_btree_dirty()    — cow_btree_mgr().total_dirty_bytes() vs mem-derived limit
    //   • check_log_size()       — log_store_mgr().total_log_bytes() vs disk-free-derived limit
    //   • check_disk_used()      — fast_free_bytes() / data_free_bytes() vs threshold
    //   • check_chunk_pool()     — device_mgr().chunk_pool().resize_to_fit(...)
    co_return;
}

// ──────────────────────────────────────────────── Event handler ──────────────────────────────────────────────────────

void ResourceMgr::on_resource_event(ResourceEvent const& ev) {
    LOGINFO("ResourceMgr received ResourceEvent kind={} source={} payload={}", static_cast< int >(ev.kind), ev.source,
            ev.payload);
    // TODO: per-kind reactions:
    //   • LogStreamSpaceExhausted → spawn cp_mgr().trigger_cp_flush(force=true) + log_store_mgr().force_truncate()
    //   • DiskFullOnWrite         → propagate to status manager / app callback
    //   • BlkAllocFailed          → trigger CP, retry hint
    //   • MemAllocFailed          → drop caches, throttle writers
}

} // namespace homestore
