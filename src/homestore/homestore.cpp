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
#include <stdexcept>
#include <utility>

#include <sisl/fds/malloc_helper.h>
#include <sisl/logging/logging.h>
#include <sisl/version.h>

#include "homestore/homestore.h"
#include "managers.h"

#include "common/homestore_assert.hpp"
#include "common/homestore_config.hpp"
#include "device/device_manager.h"
#include "checkpoint/cp_mgr.h"
#include "meta/meta_blk_manager.h"
#include "blob/blob_dev_mgr.h"
#include "index/cow_btree/cow_btree_mgr.h"
#include "logstore/log_store_mgr.h"
#include "base/resource_mgr.hpp"
#include "base/homestore_status_mgr.hpp"

#ifdef _PRERELEASE
#include "common/crash_simulator.hpp"
#include <flip/flip.hpp>
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

uint64_t HomeStore::resolve_mem_cap(AppMemSize const& mem) {
    return std::visit(
        [](auto const& v) -> uint64_t {
            using T = std::decay_t< decltype(v) >;
            if constexpr (std::is_same_v< T, AbsoluteMem >) {
                return v.bytes;
            } else { // ProportionalMem
                const uint64_t total = ResourceMgr::total_system_memory();
                return static_cast< uint64_t >(static_cast< double >(total) * v.fraction);
            }
        },
        mem);
}

folly::coro::Task< bool > HomeStore::start(InputParams input) {
    if (input.devices.empty()) {
        throw std::invalid_argument("HomeStore::start: device list is empty");
    }
    input_ = std::move(input);

    // Process-level setup that runs once.
    sisl::ObjCounterRegistry::enable_metrics_reporting();
    sisl::MallocMetrics::enable();
    HomeStoreDynamicConfig::init_settings_default();

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

    // DeviceManager: synchronously construct, then either format (first-boot, deferred to format_and_start) or load.
    auto dm = DeviceManager::create(std::move(input_.devices), input_.data_open_flags, input_.fast_open_flags);
    Managers::init_device_mgr(dm);

    if (dm->is_first_time_boot()) {
        LOGINFO("HomeStore::start — first-time boot detected; awaiting format_and_start()");
        co_return true;
    }

    LOGINFO("HomeStore: recovery boot — loading managers");
    co_await device_mgr().load_devices();
    co_await MetaBlkManager::load();

    auto cp = CPManager::create(); // self-registers via Managers::init_cp_mgr()
    co_await cp->start(/*first_time_boot=*/false);

    co_await BlobDevManager::load();
    co_await COWBtreeManager::load();
    co_await LogStoreManager::load();

    cp->start_timer();
    ResourceMgr::start(device_mgr().total_capacity(), resolve_mem_cap(input_.mem_size));

    init_done_.store(true, std::memory_order_release);
    LOGINFO("HomeStore: recovery boot complete");

    co_return false;
}

folly::coro::Task< void > HomeStore::format_and_start(FormatOpts opts) {
    HS_REL_ASSERT(device_mgr().is_first_time_boot(),
                  "format_and_start called when device is not in first-time-boot state");

    co_await device_mgr().format_devices();
    LOGINFO("HomeStore: first-time boot — creating managers (meta_chunk_size={} logstore_chunk_size={} "
            "logstore_initial_num_chunks={})",
            opts.meta_chunk_size, opts.logstore_chunk_size, opts.logstore_initial_num_chunks);

    co_await MetaBlkManager::create(opts.meta_chunk_size);
    auto cp = CPManager::create();
    co_await cp->start(/*first_time_boot=*/true);

    co_await BlobDevManager::create();
    co_await COWBtreeManager::create();
    co_await LogStoreManager::create(opts.logstore_chunk_size, opts.logstore_initial_num_chunks);

    // Force a CP so the first-time-boot state is committed before we declare success.
    co_await cp_mgr().trigger_cp_flush(true /* force */, CPTriggerReason::Timer);

    cp->start_timer();
    ResourceMgr::start(device_mgr().total_capacity(), resolve_mem_cap(input_.mem_size));

    // Commit the formatting, so from now on it will not be treated as first-time boot
    co_await device_mgr().commit_formatting();

    init_done_.store(true, std::memory_order_release);
    LOGINFO("HomeStore: first-time boot formatting complete");
}

folly::coro::Task< void > HomeStore::shutdown() {
    if (!init_done_.exchange(false, std::memory_order_acq_rel)) {
        LOGWARN("HomeStore::shutdown called before init complete (or twice)");
        co_return;
    }

    LOGINFO("HomeStore: shutdown started");
    // Reverse-of-bring-up order.
    co_await cp_mgr().shutdown();
    ResourceMgr::stop();

    co_await log_store_mgr().shutdown();
    cow_btree_mgr().shutdown();
    blob_dev_mgr().shutdown();
    // MetaBlkManager has no explicit shutdown — Managers::reset() drops it last.

    co_await device_mgr().close_devices();
    Managers::reset();

#ifdef _PRERELEASE
    flip::Flip::instance().stop_rpc_server();
#endif
    LOGINFO("HomeStore: shutdown complete");
}

} // namespace homestore