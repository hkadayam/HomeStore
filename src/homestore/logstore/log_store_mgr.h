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
//   3. Client calls recover() — drives LogStream::recover, which dispatches on_log_found to known stores via
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
    /// (caller must invoke recover() after open_log_store calls so on_log_found dispatch can find handlers).
    static Async< void > load();

    Async< void > shutdown();

    // ── Accessors ────────────────────────────────────────────────────────────

    const shared< LogStream >& log_stream() const { return log_stream_; }

    shared< LogStore > get_log_store(logstore_id_t store_id) const;
    std::vector< shared< LogStore > > log_stores() const;

    // ── Store lifecycle ──────────────────────────────────────────────────────

    /// Allocates the next store_id, creates a fresh LogStore on the underlying LogStream, persists its sb.
    /// Returned store is not opened — caller invokes open() (or open_log_store via mgr) to attach the replay
    /// handler.
    Async< shared< LogStore > > create_log_store(bool append_mode);

    /// Looks up an existing (already-loaded) LogStore by store_id, attaches the replay handler.  Returns
    /// nullptr if no store with that id exists.
    shared< LogStore > open_log_store(logstore_id_t store_id, log_replay_cb handler);

    /// Destroy a single LogStore by id — removes its meta_blk so it won't be re-discovered on restart, then
    /// erases it from in-memory state.  Idempotent (warns and returns for unknown ids).
    Async< void > destroy_log_store(logstore_id_t store_id);

    // ── Recovery ─────────────────────────────────────────────────────────────

    /// Triggers LogStream::recover with our lookup_store callback.  on_log_found fires per record into the
    /// matching LogStore (skipped if unknown store_id).  After return, LogStores with no replay handler
    /// attached are dropped (sb removed, in-memory state freed).
    Async< void > recover();

    // ── Truncation ───────────────────────────────────────────────────────────

    /// Cross-store min aggregation, called by the client periodically (or after a batch of LogStore::truncate
    /// calls) to actually reclaim space.  Computes min(min_trunc_stream_offset) across all opened LogStores
    /// and pushes it down via log_stream_->truncate.  Stores without a trunc anchor (empty stores) don't
    /// constrain the min.  No-op if no store has a valid trunc anchor.
    Async< void > global_truncate();

private:
    /// Private ctor.  Use create() / load() factories.
    LogStoreManager(shared< MetaClient > meta_client, shared< VirtualDev > vdev, shared< LogStream > stream);

    /// Lookup callback handed to LogStream::recover.  Returns the LogStore* if known, nullptr for orphan
    /// store_ids.  Unopened-but-known stores still get the LogStore* — their on_log_found is harmless without
    /// a handler and they're dropped after recover().
    LogStore* lookup_store(logstore_id_t store_id) const;

    /// Walks log_stores_, drops any without an attached handler.
    Async< void > drop_unopened_stores();

    shared< MetaClient > meta_client_;
    shared< VirtualDev > vdev_;
    shared< LogStream > log_stream_;

    mutable folly::SharedMutex stores_mutex_;
    std::map< logstore_id_t, shared< LogStore > > log_stores_;
    std::atomic< logstore_id_t > next_store_id_{0};

    static constexpr const char* kVdevName = "logstore_vdev";
};

} // namespace homestore