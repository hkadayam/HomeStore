/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ***************************************************************************/
#include <cstdint>
#include "common/async.h"
#include <stdexcept>
#include <utility>

#include "sisl/fds/malloc_helper.h"
#include "sisl/logging/logging.h"
#include "sisl/version.h"
#include "sisl/fds/obj_life_counter.h"

#include "homestore/homestore.h"
#include "homestore/managers.h"

#include "homestore/base/homestore_assert.h"
#include "homestore/base/hs_runtime_config.h"
#include "homestore/device/device_manager.h"
#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/blob/blob_dev_mgr.h"
#include "homestore/index/cow_btree/cow_btree_mgr.h"
#include "homestore/logstore/log_store_mgr.h"
#include "homestore/replication/repl_manager.h"
#include "homestore/base/resource_mgr.h"

#ifdef _PRERELEASE
#include "homestore/common/crash_simulator.h"
#include "sisl/flip/flip.hpp"
#endif

namespace homestore {

HomeStoreSafePtr HomeStore::s_instance_{nullptr};
static const std::string s_version = PACKAGE_VERSION;

HomeStore* HomeStore::instance() {
    if (!s_instance_) {
        s_instance_ = std::make_shared< HomeStore >();
    }
    return s_instance_.get();
}

#ifdef _PRERELEASE
HomeStore& HomeStore::with_crash_simulator(std::function< void() > restart_cb) {
    crash_simulator_ = std::make_unique< CrashSimulator >(std::move(restart_cb));
    return *this;
}
#endif

bool HomeStore::is_first_time_boot() const {
    return device_mgr().is_first_time_boot();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//                                                start / boot
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

Async< bool > HomeStore::start(InputParams input) {
    if (input.devices.empty()) {
        throw std::invalid_argument("HomeStore::start: device list is empty");
    }
    input_ = std::move(input);

    // Process-level setup that runs once.
    sisl::ObjCounterRegistry::enable_metrics_reporting();
    sisl::MallocMetrics::enable();
    HomeStoreRuntimeConfig::init_settings_default();

#ifndef NDEBUG
    LOGINFO("HomeStore DEBUG version: {}", s_version);
#else
    LOGINFO("HomeStore RELEASE version: {}", s_version);
#endif
    sisl::VersionMgr::addVersion(PACKAGE_NAME, version::Semver200_version(PACKAGE_VERSION));

#ifdef _PRERELEASE
    flip::Flip::instance().start_rpc_server();
    if (!crash_simulator_) {
        crash_simulator_ = std::make_unique< CrashSimulator >(nullptr);
    }
#endif

    // DeviceManager: synchronously construct — this probes the device headers so is_first_time_boot() is
    // answerable immediately.  Copy input_.devices into DeviceManager — the vector stays in input_ so
    // ResourceMgr::start can use it later.
    auto dm =
        DeviceManager::create(std::vector< DevInfo >{input_.devices}, input_.data_open_flags, input_.fast_open_flags);
    Managers::init_device_mgr(dm);

    bool const first = dm->is_first_time_boot();
    LOGINFO("HomeStore::start — {} boot detected", first ? "first-time" : "recovery");
    co_return first;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//                                          format (first-time boot)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

Async< void > HomeStore::format() {
    HS_REL_ASSERT(device_mgr().is_first_time_boot(),
                  "HomeStore::format called when device is not in first-time-boot state");
    auto const& opts = input_.format_opts;
    LOGINFO("HomeStore: first-time boot — creating managers (meta_chunk_size={} logstore_chunk_size={} "
            "logstore_initial_num_chunks={})",
            opts.meta_chunk_size, opts.logstore_chunk_size, opts.logstore_initial_num_chunks);

    co_await device_mgr().format_devices();

    // ResourceMgr is foundational (journal throttle, index cache sizing) — up before any manager does real IO.
    ResourceMgr::start(input_.devices);

    co_await MetaBlkManager::create(opts.meta_chunk_size);
    auto cp = CPManager::create();
    co_await cp->start(/*first_time_boot=*/true);

    co_await BlobDevManager::create();
    co_await COWBtreeManager::create();
    co_await LogStoreManager::create(opts.logstore_chunk_size, opts.logstore_initial_num_chunks);

    // Replication is optional — only brought up when the application handed us a ReplApplication.  create()
    // sets up infra (executors, rpc client factory, listener object) and registers ReplCPHandler; no engines.
    if (input_.repl_app) {
        co_await ReplicationManager::create(input_.repl_app);
    }

    // Force a CP so the first-time-boot structures are durable, then commit formatting so subsequent boots are
    // treated as recovery.
    co_await cp_mgr().trigger_cp_flush(true /* force */, CPTriggerReason::SystemRestart);
    co_await device_mgr().commit_formatting();

    // A fresh store has nothing to replay, so go live right here rather than forcing the caller through
    // replay().  ReplicationManager::create() (above) already brought the replication service live (listener +
    // maintenance timers); all that remains is to start the periodic timers and mark the store initialized.
    cp_mgr().start_timer();
    ResourceMgr::start_timer();
    init_done_.store(true, std::memory_order_release);
    LOGINFO("HomeStore: first-time boot complete");
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//                                             load (recovery)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

Async< void > HomeStore::load() {
    HS_REL_ASSERT(!device_mgr().is_first_time_boot(),
                  "HomeStore::load called on a first-time-boot device — call format() instead");
    LOGINFO("HomeStore: recovery boot — loading managers");

    co_await device_mgr().load_devices();

    // ResourceMgr foundational — up before replay() drives real journal/blk IO.
    ResourceMgr::start(input_.devices);

    co_await MetaBlkManager::load();
    auto cp = CPManager::create();
    co_await cp->start(/*first_time_boot=*/false);

    co_await BlobDevManager::load();
    co_await COWBtreeManager::load(); // index self-recovers from its own CPs, independent of the log stream
    co_await LogStoreManager::load(); // reconstruct LogStore instances; NO replay yet, tail_lsn stays -1

    // Replication load(): reconstruct each ReplicaSet (ReplicaSet::load — read SB, seed watermarks,
    // open_log_store + attach on_log_found handler; NO raft engine), then register ReplCPHandler AFTER the sets
    // exist so its initial on_switchover_cp initializes each set's per-CP checkpoint tracking for the CP that
    // replay() will dirty.
    if (input_.repl_app) {
        co_await ReplicationManager::load(input_.repl_app);
    }
    // TODO(repl-consistency): when repl_app is null but persisted replica-set SBs exist on disk, those groups
    // have no host — HS_REL_ASSERT once ReplicationManager exposes a has_persisted_groups() probe.

    LOGINFO("HomeStore: recovery managers loaded (awaiting app recovery + replay())");
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//                                          replay (recovery go-live)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

Async< void > HomeStore::replay() {
    // Recovery-only — the first-time-boot path goes live inside format() and never reaches here.

    // 1. Byte-level log replay.  Populates each LogStore's records_/tail_lsn and re-drives dispatch_commit into
    //    the (already app-recovered) consumers, folding every ReplicaSet's commit_upto_lsn back to its pre-crash
    //    value.  Deposits into the currently-open CP with all consumers already registered; no flush fires here.
    co_await log_store_mgr().replay();

    // 2. Bring raft engines up + go live — ONLY after step 1 restored their commit watermark, since nuraft
    //    reads the state machine's last-committed index (== commit_upto_lsn_) at start_server.  Skipped
    //    entirely when replication is disabled.
    if (input_.repl_app) {
        co_await repl_mgr().start_engine();
    }

    // 3. Seal a fresh recovery baseline so a crash shortly after boot does not redo the whole replay from the
    //    pre-crash checkpoint.  Correctness note: this persists checkpoint_lsn, so it requires ReplCPHandler to
    //    flush LAST among CP consumers (see cp_mgr.cpp cp-ordering TODO).
    co_await cp_mgr().trigger_cp_flush(true /* force */, CPTriggerReason::SystemRestart);

    // 4. Only now are the periodic timers allowed to fire (CP flush + ResourceMgr storage-pressure poll).
    cp_mgr().start_timer();
    ResourceMgr::start_timer();

    init_done_.store(true, std::memory_order_release);
    LOGINFO("HomeStore: recovery boot complete");
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//                                                 shutdown
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

Async< void > HomeStore::shutdown() {
    if (!init_done_.exchange(false, std::memory_order_acq_rel)) {
        LOGWARN("HomeStore::shutdown called before init complete (or twice)");
        co_return;
    }

    LOGINFO("HomeStore: shutdown started");

    // ── Pass 1: quiesce the autonomous drivers, while every module is still alive ────────────────────────────────
    // Only CP and ResourceMgr drive work INTO other modules (CP flushes its consumers; ResourceMgr truncates
    // LogStore/Repl), so only they need a prepare phase before the reverse-order teardown below.  ResourceMgr
    // first, so no in-flight truncation dirties the final CP; then CP takes its final durability flush into
    // still-live consumers and stops its timer.  After this, nothing autonomously calls between modules.
    co_await ResourceMgr::prepare_shutdown();
    co_await cp_mgr().prepare_shutdown();

    // ── Pass 2: tear down in strict reverse of boot order ───────────────────────────────────────────────────────
    // Boot: Device → ResourceMgr → Meta → CP → Blob → COWBtree → LogStore → Repl.
    if (input_.repl_app) {
        co_await repl_mgr().stop();
    }
    co_await log_store_mgr().shutdown();
    cow_btree_mgr().shutdown();
    blob_dev_mgr().shutdown();
    co_await cp_mgr().shutdown();     // prepare_shutdown() already ran above; this just frees CP state
    // MetaBlkManager has no explicit shutdown — Managers::reset() drops it.
    co_await ResourceMgr::stop();     // prepare_shutdown() already ran; this drops the subscription + singleton

    co_await device_mgr().close_devices();
    Managers::reset();

#ifdef _PRERELEASE
    flip::Flip::instance().stop_rpc_server();
#endif
    LOGINFO("HomeStore: shutdown complete");
}

} // namespace homestore
