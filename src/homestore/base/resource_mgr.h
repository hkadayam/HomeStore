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
#include "common/async.h"
#include <optional>
#include <vector>

#include "common/defs.h"                   // shared<>
#include "homestore/base/homestore_decl.h" // DevInfo, HSDevType
#include "iomanager/coro_timer.h"
#include "homestore/base/resource_event.h"

namespace homestore {

// ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ResourceManager
//
// Decision-maker, not a query oracle.  Information flows IN (poll loop + ResourceEvent subscription);
// decisions go OUT as direct calls to cp_mgr().trigger_cp_flush(), log_store_mgr().force_truncate(),
// device_mgr().chunk_pool().resize_to_fit(), and similar.  Other modules don't query RM — RM acts on them.
//
// Owns:
//   • Per-tier device capacity (Fast vs Data) — derived from the DevInfo list.  When no Fast device is configured,
//     the fast-tier internal accessor falls back to data values transparently.
//   • Memory budget (mem_cap), derived cache budget.
//   • A periodic poll loop that evaluates resource-pressure rules and triggers actions directly.
//   • A subscription on EventManager for ResourceEvent — the rare emergency push channel from lower modules.
//
// The only public accessor is cache_size() — needed once at startup by cache implementations to size themselves.
// ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class ResourceMgr {
public:
    /// Construct, install into Managers, compute the cache budget, subscribe to ResourceEvent, and start the poll
    /// loop.  `devs` is the same vector handed to HomeStore::start (per-tier capacity is summed from it).
    /// `mem_cap`: when nullopt, falls back to (total_system_memory * resource_limits.sys_mem_use_percent / 100).
    static void start(std::vector< DevInfo > const& devs, std::optional< uint64_t > mem_cap = std::nullopt);

    /// Stop the poll loop, drop event subscriptions, drop the singleton.  Idempotent.
    static void stop();

    /// Total physical RAM on the host, in bytes.  0 if the platform query failed.  Utility for HomeStore::start to
    /// resolve a ProportionalMem InputParams.mem_size into a concrete byte budget.
    static uint64_t total_system_memory();

    /// Cache budget in bytes — passed to TwoQEvictor / similar caches at construction.  Fixed at start as
    /// `mem_cap * cache_size_percent / 100`.  This is the only runtime accessor RM exposes.
    uint64_t cache_size() const { return cache_size_; }

    // Destructor must be public for shared_ptr's deleter; construction is still gated by start() (private ctor).
    ~ResourceMgr();

private:
    ResourceMgr(uint64_t fast_capacity, uint64_t data_capacity, uint64_t mem_cap, uint64_t cache_size);

    /// Periodic rule evaluation — runs on poll_timer_'s EB thread.
    Async< void > poll_tick();

    /// ResourceEvent subscriber — invoked synchronously from EventManager::publish() on the publisher's thread.
    /// Heavy follow-up work is dispatched via spawn_detached.
    void on_resource_event(ResourceEvent const& ev);

    // ── Internal helpers (private — RM doesn't expose free/ratio queries) ────────────────────────────────────────
    /// Effective fast capacity: returns data_capacity when no separate Fast tier is configured.
    uint64_t fast_capacity_with_fallback() const { return fast_capacity_ > 0 ? fast_capacity_ : data_capacity_; }
    uint64_t fast_free_bytes() const;
    uint64_t data_free_bytes() const;
    uint64_t mem_free_bytes() const;

    const uint64_t fast_capacity_; // raw — 0 if no Fast device configured
    const uint64_t data_capacity_;
    const uint64_t mem_cap_;
    const uint64_t cache_size_;

    iomanager::CoroTimer poll_timer_;
};

} // namespace homestore
