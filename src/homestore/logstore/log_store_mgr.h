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
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <folly/SharedMutex.h>
#include "common/async.h"

#include "homestore/device/virtual_dev.h"

#include "homestore/base/homestore_decl.h"
#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/logstore/log_store.h"
#include "homestore/logstore/log_stream.h"

namespace homestore {

class MetaClient;

// ─────────────────────────────────────────────────────────────────────────────
// LogStoreManager
//
// Owns one VDev, one LogStream, and many LogStores layered on that stream.
// Lifecycle:
//   1. Boot path:
//        first run  → create(chunk_size) registers MetaClient, creates the VDev + LogStream, persists sb.
//        restart    → load() re-registers MetaClient, reconstructs the VDev + LogStream, then walks logstore sb
//                     mblks to reconstruct LogStore instances (NOT opened, no records yet).
//   2. Client calls open_log_store(sid, handler) for each LogStore it cares about.
//   3. Client calls replay() — drives LogStream::recover, which dispatches on_log_found to known stores via
//      lookup_store callback.  After return, unopened stores are dropped (sb removed).
//   4. Steady state: create_log_store / get_log_store / truncate.
//
// truncate() walks all known LogStores, mins their min_trunc_stream_offset, and pushes that down to the
// LogStream so the underlying byte stream can release storage.
// ─────────────────────────────────────────────────────────────────────────────
class LogStoreManager : public std::enable_shared_from_this< LogStoreManager > {
public:
    LogStoreManager(const LogStoreManager&) = delete;
    LogStoreManager& operator=(const LogStoreManager&) = delete;
    LogStoreManager(LogStoreManager&&) = delete;
    LogStoreManager& operator=(LogStoreManager&&) = delete;
    ~LogStoreManager() = default;

    /// First-boot path: create the VDev (initial_num_chunks of chunk_size each, expand-on-demand thereafter),
    /// the LogStream, and an empty LogStoreManager.  These params are format-time-only and not in dynamic config.
    static Async< void > create(uint64_t chunk_size, uint32_t initial_num_chunks);

    /// Restart path: discover LogStream + LogStores from MetaBlks; does NOT walk the LogStream's CRC chain
    /// (caller must invoke replay() after open_log_store calls so on_log_found dispatch can find handlers).
    static Async< void > load();

    Async< void > shutdown();

    // ── Accessors ────────────────────────────────────────────────────────────

    const shared< LogStream >& log_stream() const { return log_stream_; }

    /// Live physical footprint of the log stream in bytes (chunks it currently holds).  ResourceMgr polls this
    /// against the resource_limits.logstream_size_limit_pct budget to decide when to force a truncation pass.
    uint64_t footprint_bytes() const { return log_stream_ ? log_stream_->footprint_bytes() : 0; }

    shared< LogStore > get_log_store(logstore_id_t store_id) const;
    std::vector< shared< LogStore > > log_stores() const;

    // ── Store lifecycle ──────────────────────────────────────────────────────

    /// Allocates the next store_id, creates a fresh LogStore on the underlying LogStream, persists its sb.
    /// Returned store is not opened — caller invokes open() (or open_log_store via mgr) to attach the replay
    /// handler.  If options.auto_truncate is set, the manager's auto_truncate iteration includes this store.
    Async< shared< LogStore > > create_log_store(LogStoreOptions const& options);

    /// Looks up an existing (already-loaded) LogStore by store_id, installs `options` (updating
    /// auto_truncate_count_ if the flag changed), and attaches the replay handler.  Returns nullptr if no
    /// store with that id exists.
    shared< LogStore > open_log_store(logstore_id_t store_id, LogStoreOptions const& options, log_replay_cb handler,
                                      log_commit_watermark_cb watermark_cb = nullptr);

    /// Destroy a single LogStore by id — removes its meta_blk so it won't be re-discovered on restart, then
    /// erases it from in-memory state.  Idempotent (warns and returns for unknown ids).
    Async< void > destroy_log_store(logstore_id_t store_id);

    // ── Recovery ─────────────────────────────────────────────────────────────

    /// Triggers LogStream::recover with our lookup_store callback.  on_log_found fires per record into the
    /// matching LogStore (skipped if unknown store_id).  After return, LogStores with no replay handler
    /// attached are dropped (sb removed, in-memory state freed).  Driven by HomeStore::replay() after all
    /// consumers have opened their stores — this is where deferred replay actually happens.
    Async< void > replay();

    // ── Truncation ───────────────────────────────────────────────────────────

    /// For each store with options().auto_truncate set, invokes LogStore::truncate(MAX) so the store
    /// advances head_lsn to its own clamp (min(checkpt_lsn, tail - preserve_log_count)).  Then aggregates
    /// min(min_trunc_stream_offset) across all opened LogStores and pushes it down via log_stream_->truncate.
    /// Stores without a trunc anchor (empty stores) don't constrain the min.  No-op on the stream if no store
    /// has a valid trunc anchor.
    Async< void > truncate();

    /// Manager-level CPCallbacks.  Constructed as a shared_ptr in create()/load() and registered with
    /// cp_mgr().  Fans on_switchover_cp / cp_flush across every managed store.
    class CPHandler : public CPCallbacks {
    public:
        explicit CPHandler(shared< LogStoreManager > mgr) : mgr_{std::move(mgr)} {}
        ~CPHandler() override = default;

        void on_switchover_cp(CP* cur_cp, CP* new_cp) override;
        Async< bool > cp_flush(CP* cp) override;
        void cp_cleanup(CP* cp) override;
        int cp_progress_percent() override;

    private:
        std::weak_ptr< LogStoreManager > mgr_;
    };

private:
    /// Private ctor.  Use create() / load() factories.
    LogStoreManager(shared< MetaClient > meta_client, shared< VirtualDev > vdev, shared< LogStream > stream);

    /// Lookup callback handed to LogStream::recover.  Returns the LogStore* if known, nullptr for orphan
    /// store_ids.  Unopened-but-known stores still get the LogStore* — their on_log_found is harmless without
    /// a handler and they're dropped after recover().
    LogStore* lookup_store(logstore_id_t store_id) const;

    /// Walks log_stores_, drops any without an attached handler.
    Async< void > drop_unopened_stores();

    /// Recurring iomgr timer: calls truncate() at logstore.auto_truncate_frequency_ms cadence.  Runs on
    /// ReactorTarget::any() — same IO reactor pool the stream flush/truncate paths use.
    void start_auto_truncate_timer();

    shared< MetaClient > meta_client_;
    shared< VirtualDev > vdev_;
    shared< LogStream > log_stream_;

    mutable folly::SharedMutex stores_mutex_;
    std::map< logstore_id_t, shared< LogStore > > log_stores_;
    std::atomic< logstore_id_t > next_store_id_{0};

    // Count of log_stores currently opted into auto_truncate. Periodic auto compaction iteration
    // can short-circuit without walking log_stores_.
    std::atomic< uint32_t > num_stores_auto_truncate_{0};

    iomanager::CoroTimer auto_truncate_timer_;

    static constexpr const char* kVdevName = "logstore_vdev";
};

} // namespace homestore