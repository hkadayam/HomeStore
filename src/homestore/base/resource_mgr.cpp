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
#include "resource_mgr.hpp"

#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <sisl/logging/logging.h>

#include "homestore_config.hpp"
#include "managers.h"

namespace homestore {

uint64_t ResourceMgr::total_system_memory() {
#ifdef __linux__
    long pages = ::sysconf(_SC_PHYS_PAGES);
    long page_size = ::sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0)
        return 0;
    return to_u64(pages) * to_u64(page_size);
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    if (::sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0) != 0)
        return 0;
    return mem;
#else
    return 0;
#endif
}

void ResourceMgr::start(uint64_t dev_capacity, std::optional< uint64_t > mem_cap) {
    const uint64_t resolved_mem_cap = mem_cap.value_or(
        (ResourceMgr::total_system_memory() * HS_DYNAMIC_CONFIG(resource_limits.sys_mem_use_percent)) / 100);
    const uint64_t cache_size = (resolved_mem_cap * HS_DYNAMIC_CONFIG(resource_limits.cache_size_percent)) / 100;

    LOGINFO("ResourceMgr starting: dev_capacity={} mem_cap={} (caller_provided={}) cache_size={}", dev_capacity,
            resolved_mem_cap, mem_cap.has_value(), cache_size);

    Managers::init_resource_mgr(shared< ResourceMgr >{new ResourceMgr{dev_capacity, resolved_mem_cap, cache_size}});
}

void ResourceMgr::stop() {
    Managers::reset_resource_mgr();
}

#if 0
// ─────────────────────────────────────────────────────────────────────────────
// Legacy implementations — kept for reference while the new model is brought up.
// ─────────────────────────────────────────────────────────────────────────────

void ResourceMgrLegacy::trigger_truncate() {
    if (hs()->has_repl_data_service()) {
        hs()->logstore_service().device_truncate();
    }
}

void ResourceMgrLegacy::start_timer() {
    auto const res_mgr_timer_ms = HS_DYNAMIC_CONFIG(resource_limits.resource_audit_timer_ms);
    if (res_mgr_timer_ms == 0) return;
    m_res_audit_timer_hdl = iomanager.schedule_global_timer(
        res_mgr_timer_ms * 1000 * 1000, true, nullptr, iomgr::reactor_regex::all_worker,
        [this](void*) { trigger_truncate(); }, true);
}

void ResourceMgrLegacy::inc_mem_used_in_recovery(int size) {
    m_memory_used_in_recovery.fetch_add(size, std::memory_order_relaxed);
}
void ResourceMgrLegacy::dec_mem_used_in_recovery(int size) {
    m_memory_used_in_recovery.fetch_sub(size, std::memory_order_relaxed);
}
bool ResourceMgrLegacy::can_add_mem_in_recovery(int size) const {
    return cur_mem_used_in_recovery() + size <= get_mem_used_in_recovery_limit();
}
int64_t ResourceMgrLegacy::cur_mem_used_in_recovery() const {
    return m_memory_used_in_recovery.load(std::memory_order_relaxed);
}
int64_t ResourceMgrLegacy::get_mem_used_in_recovery_limit() const {
    return (HS_DYNAMIC_CONFIG(resource_limits.memory_in_recovery_precent) * HS_STATIC_CONFIG(input.app_mem_size)) / 100;
}

bool ResourceMgrLegacy::check_journal_descriptor_size(uint64_t used_size) const {
    return used_size >= get_journal_descriptor_size_limit();
}
bool ResourceMgrLegacy::check_journal_vdev_size(uint64_t used_size, uint64_t total_size) {
    if (m_journal_vdev_exceed_cb) {
        const uint32_t used_pct = (100 * used_size / total_size);
        if (used_pct >= get_journal_vdev_size_limit()) {
            m_journal_vdev_exceed_cb(used_size, used_pct >= get_journal_vdev_size_critical_limit());
            return true;
        }
    }
    return false;
}
void ResourceMgrLegacy::register_journal_vdev_exceed_cb(exceed_limit_cb_t cb) {
    m_journal_vdev_exceed_cb = std::move(cb);
}
uint32_t ResourceMgrLegacy::get_journal_descriptor_size_limit() const {
    return HS_DYNAMIC_CONFIG(resource_limits.journal_descriptor_size_threshold_mb) * 1024 * 1024;
}
uint32_t ResourceMgrLegacy::get_journal_vdev_size_critical_limit() const {
    return HS_DYNAMIC_CONFIG(resource_limits.journal_vdev_size_percent_critical);
}
uint32_t ResourceMgrLegacy::get_journal_vdev_size_limit() const {
    return HS_DYNAMIC_CONFIG(resource_limits.journal_vdev_size_percent);
}

void ResourceMgrLegacy::check_chunk_free_size_and_trigger_cp(uint64_t /*free_size*/, uint64_t /*alloc_size*/) {}
uint32_t ResourceMgrLegacy::get_dirty_buf_qd() const { return m_flush_dirty_buf_q_depth; }
void ResourceMgrLegacy::increase_dirty_buf_qd() {
    auto qd = m_flush_dirty_buf_q_depth.load();
    if (qd < max_qd_multiplier * HS_DYNAMIC_CONFIG(generic.cache_max_throttle_cnt)) {
        m_flush_dirty_buf_q_depth.fetch_add(2 * qd);
    }
}
void ResourceMgrLegacy::reset_dirty_buf_qd() {
    m_flush_dirty_buf_q_depth = HS_DYNAMIC_CONFIG(generic.cache_max_throttle_cnt);
}
#endif // 0

} // namespace homestore
