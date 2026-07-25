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
#include <cstdint>
#include <memory>
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
class ResourceMgr : public std::enable_shared_from_this< ResourceMgr > {
public:
    /// Construct, install into Managers, compute the cache budget, and subscribe to ResourceEvent.  Does NOT start
    /// the poll timer — call start_timer() at go-live (mirrors CPManager).  `devs` is the same vector handed to
    /// HomeStore::start (per-tier capacity is summed from it).  The process memory budget comes from config:
    /// resource_limits.process_mem_budget_bytes (absolute), or a fraction (sys_mem_use_percent) of total system RAM
    /// when that is 0.
    static void start(std::vector< DevInfo > const& devs);

    /// Start the periodic storage-pressure poll timer.  Opt-in and separate from start() so tests (and the
    /// pre-go-live boot window) can run without an autonomous poll firing truncations.  Called at go-live next to
    /// cp_mgr().start_timer().  No-op if already started.
    static void start_timer();

    /// Phase 1 of teardown: stop the poll timer and drain any in-flight reclaim (poll-driven or emergency), and
    /// reject new reclaims (stopping_).  After this returns, ResourceMgr drives no more truncations, so the modules
    /// it truncates (LogStore, Repl) can be torn down safely.  Does NOT drop the singleton or event subscription.
    /// Idempotent.  Two-phase teardown calls this on ResourceMgr before tearing LogStore/Repl down.
    static Async< void > prepare_shutdown();

    /// Phase 2 of teardown: prepare_shutdown() (no-op if already run), then drop the ResourceEvent subscription and
    /// the singleton.  A lone stop() is therefore a complete quiesce-then-drop for single-call sites.  Call BEFORE
    /// any blanket Managers::reset().
    static Async< void > stop();

    /// Total physical RAM on the host, in bytes.  0 if the platform query failed.  Used by start() to resolve
    /// the proportional (sys_mem_use_percent) memory budget when no absolute process_mem_budget_bytes is set.
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

    /// Storage-pressure log truncation, shared by poll_tick() (proactive) and on_resource_event() (emergency).
    /// Replicated groups truncate via repl_mgr().truncate() (snapshot -> compaction advances raft log heads);
    /// then log_store_mgr().truncate() reclaims the underlying stream chunks.  truncation_in_flight_ collapses
    /// overlapping triggers into a single pass.
    Async< void > reclaim_log_space();

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

    // Set while a reclaim_log_space() pass is running so a poll tick and an emergency event don't stack passes.
    std::atomic< bool > truncation_in_flight_{false};

    // Set by prepare_shutdown(): once true, poll_tick() and on_resource_event() start no new reclaim.
    std::atomic< bool > stopping_{false};

    // Held for the duration of a reclaim_log_space() pass.  prepare_shutdown() acquires it to WAIT for an in-flight
    // emergency reclaim (the spawn_detached from on_resource_event, which poll_timer_.stop() does not join) to
    // finish before LogStore/Repl are torn down.  Control-plane only — never on the IO path.
    folly::coro::Mutex reclaim_gate_;
};

} // namespace homestore
