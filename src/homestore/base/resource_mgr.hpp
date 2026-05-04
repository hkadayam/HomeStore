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

#include <cstdint>
#include <optional>

#include "common/defs.h" // shared<>

namespace homestore {

// ──────────────────────────────────────────────────────────────────────────────
// ResourceMgr
//
// Single source of truth for the process-wide IO and memory budgets. Created during HomeStore startup once
// DeviceManager has reported total device capacity; either the caller passes an explicit memory cap or it is
// derived from system memory using resource_limits.sys_mem_use_percent.
//
// Currently only the cache budget is exposed. Other resource-tracking responsibilities (recovery memory, journal
// vdev watermarks, dirty-buffer queue depth, free-blk accounting) are intentionally commented out below until the
// new model needs them.
// ──────────────────────────────────────────────────────────────────────────────
class ResourceMgr {
public:
    /// Construct, install into Managers, and compute the cache budget.
    ///
    /// dev_capacity: total capacity across all PhysicalDevs as reported by DeviceManager::total_capacity().
    /// mem_cap:      optional memory budget for the homestore process. When std::nullopt, derived as
    ///               (system_total_memory * resource_limits.sys_mem_use_percent / 100).
    ///
    /// cache_size = mem_cap * resource_limits.cache_size_percent / 100.
    static void start(uint64_t dev_capacity, std::optional< uint64_t > mem_cap = std::nullopt);

    /// Drop the singleton.
    static void stop();

    /// Total physical RAM on the host, in bytes.  0 if the platform query failed.  Used by HomeStore::start to resolve
    /// a ProportionalMem InputParams.mem_size into a concrete byte budget.
    static uint64_t total_system_memory();

    uint64_t dev_capacity() const { return dev_capacity_; }
    uint64_t mem_cap() const { return mem_cap_; }
    uint64_t cache_size() const { return cache_size_; }

private:
    ResourceMgr(uint64_t dev_capacity, uint64_t mem_cap, uint64_t cache_size) :
            dev_capacity_{dev_capacity}, mem_cap_{mem_cap}, cache_size_{cache_size} {}

    const uint64_t dev_capacity_;
    const uint64_t mem_cap_;
    const uint64_t cache_size_;
};

#if 0
// ─────────────────────────────────────────────────────────────────────────────
// Legacy ResourceMgr surface — kept here for reference while the new model is
// brought up. Re-enable individual pieces as they get wired into the new layering.
// ─────────────────────────────────────────────────────────────────────────────

class RsrcMgrMetrics : public sisl::MetricsGroup {
public:
    explicit RsrcMgrMetrics() : sisl::MetricsGroup("resource_mgr", "resource_mgr") {
        REGISTER_COUNTER(index_dirty_size, "Total Index cache dirty buffer size", sisl::PublishAs::Gauge);
        REGISTER_COUNTER(free_blk_size_in_cp, "Total free blks size accumulated in a cp",
                         sisl::PublishAs::Gauge);
        REGISTER_COUNTER(free_blk_cnt_in_cp, "Total free blks cnt accumulated in a cp",
                         sisl::PublishAs::Gauge);
        REGISTER_COUNTER(alloc_blk_cnt_in_cp, "Total alloc blks cnt accumulated in a cp",
                         sisl::PublishAs::Gauge);
        register_me_to_farm();
    }
    ~RsrcMgrMetrics() { deregister_me_from_farm(); }
};

typedef std::function< void(int64_t /* dirty_buf_cnt */, bool /* critical */) > exceed_limit_cb_t;
const uint32_t max_qd_multiplier = 32;

class ResourceMgrLegacy {
public:
    void inc_mem_used_in_recovery(int size);
    void dec_mem_used_in_recovery(int size);
    bool can_add_mem_in_recovery(int size) const;
    int64_t cur_mem_used_in_recovery() const;
    int64_t get_mem_used_in_recovery_limit() const;

    bool check_journal_vdev_size(uint64_t used_size, uint64_t total_size);
    bool check_journal_descriptor_size(uint64_t used_size) const;
    void register_journal_vdev_exceed_cb(exceed_limit_cb_t cb);
    uint32_t get_journal_vdev_size_limit() const;
    uint32_t get_journal_vdev_size_critical_limit() const;
    uint32_t get_journal_descriptor_size_limit() const;

    void check_chunk_free_size_and_trigger_cp(uint64_t free_size, uint64_t alloc_size);
    uint32_t get_dirty_buf_qd() const;
    void increase_dirty_buf_qd();
    void reset_dirty_buf_qd();
    void trigger_truncate();

private:
    void start_timer();

    std::atomic< int64_t > m_hs_fb_size;
    std::atomic< int64_t > m_hs_ab_cnt;
    std::atomic< int64_t > m_memory_used_in_recovery;
    std::atomic< uint32_t > m_flush_dirty_buf_q_depth{64};
    exceed_limit_cb_t m_journal_vdev_exceed_cb;
    RsrcMgrMetrics m_metrics;
    iomgr::timer_handle_t m_res_audit_timer_hdl{iomgr::null_timer_handle};
};
#endif // 0

} // namespace homestore
