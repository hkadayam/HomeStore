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

#include "homestore/logstore/log_store_mgr.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string_view>

#include <fmt/format.h>
#include "sisl/logging/logging.h"

#include "homestore/base/homestore_assert.h"
#include "homestore/base/homestore_config.h" // HS_DYNAMIC_CONFIG
#include "common/defs.h"
#include "homestore/device/device_manager.h"
#include "homestore/managers.h"
#include "homestore/meta/meta_blk.h"
#include "homestore/meta/meta_client.h"

namespace homestore {

static constexpr std::string_view kLogStoreSbPrefix = "LogStore_";

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

LogStoreManager::LogStoreManager(shared< MetaClient > meta_client, shared< VirtualDev > vdev,
                                 shared< LogStream > stream) :
        meta_client_{std::move(meta_client)}, vdev_{std::move(vdev)}, log_stream_{std::move(stream)} {
}

// ─────────────────────────────────────────────────────────────────────────────
// create / load / shutdown
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > LogStoreManager::create(uint64_t chunk_size, uint32_t initial_num_chunks) {
    LOGINFO("LogStoreManager: first boot — creating fresh manager (chunk_size={} initial_num_chunks={})", chunk_size,
            initial_num_chunks);

    // register_client returns MetaClient by value (move-only).  Wrap in shared_ptr immediately so the same handle
    // can be shared across LogStream + LogStoreManager + each LogStore created later.
    auto meta_client = std::make_shared< MetaClient >(co_await meta_mgr().register_client("LogStoreManager"));

    VDevParameters params{};
    params.vdev_name = kVdevName;
    params.initial_chunk_size = chunk_size;
    params.initial_num_chunks = initial_num_chunks;
    auto vdev = co_await device_mgr().create_vdev(std::move(params));

    auto stream = co_await LogStream::create(/*stream_id=*/0, *meta_client, kVdevName, vdev, chunk_size);

    auto mgr =
        shared< LogStoreManager >(new LogStoreManager{std::move(meta_client), std::move(vdev), std::move(stream)});
    Managers::init_log_store_mgr(mgr);
    LOGINFO("LogStoreManager: ready (chunk_size={} initial_chunks={})", chunk_size, initial_num_chunks);
}

folly::coro::Task< void > LogStoreManager::load() {
    LOGINFO("LogStoreManager: starting recovery scan");

    auto meta_client = std::make_shared< MetaClient >(co_await meta_mgr().register_client("LogStoreManager"));

    // 1. Scan MetaBlks for the LogStream sb (logstream_sb_<sid>) and per-store sbs (LogStore_<sid>).  We have a
    //    single stream so collect just its mblk; collect each per-store mblk indexed by store_id.
    std::optional< MetaBlk > stream_blk;
    sisl::ByteView stream_payload;
    std::map< logstore_id_t, std::pair< MetaBlk, sisl::ByteView > > store_blks;
    logstore_id_t max_store_id = 0;
    bool any_store = false;

    co_await meta_client->for_each_recovered_block([&](MetaBlk blk, sisl::ByteView data) -> folly::coro::Task< void > {
        const auto& name = blk.name();
        // Per-store sb: "LogStore_<store_id>"
        if (name.size() > kLogStoreSbPrefix.size() &&
            std::string_view{name}.substr(0, kLogStoreSbPrefix.size()) == kLogStoreSbPrefix) {
            std::string_view sid_part{name};
            sid_part.remove_prefix(kLogStoreSbPrefix.size());
            logstore_id_t sid{};
            auto rc = std::from_chars(sid_part.data(), sid_part.data() + sid_part.size(), sid);
            if (rc.ec == std::errc{}) {
                store_blks.emplace(sid, std::make_pair(std::move(blk), std::move(data)));
                max_store_id = std::max(max_store_id, sid);
                any_store = true;
            }
            co_return;
        }
        // LogStream sb is owned by the LogStream loader; we let LogStream::load discover it via its own
        // per-stream sb naming.  Nothing to capture here — the LogStream factory does the read.
        co_return;
    });

    // 2. Reconstruct VDev for the logstore — discovered automatically by device_mgr's vdev recovery.
    auto vdev = device_mgr().get_vdev(kVdevName);
    if (!vdev) {
        throw std::runtime_error(
            fmt::format("LogStoreManager::load: VDev '{}' not found in device manager", kVdevName));
    }

    // 3. Reconstruct LogStream.  LogStream's own sb mblk is parsed inside its load() factory; we provide the
    //    MetaClient + dev_name + recovered chunk list (vdev already has its chunks).
    auto stream_mblk_name = LogStream::sb_mblk_name(kVdevName, /*stream_id=*/0);
    auto stream_blk_opt = co_await meta_client->get_meta_blk(stream_mblk_name);
    if (!stream_blk_opt) {
        throw std::runtime_error(fmt::format("LogStoreManager::load: LogStream sb '{}' not found", stream_mblk_name));
    }
    auto stream_blk_data = co_await meta_client->read_meta_blk(*stream_blk_opt);
    auto stream = co_await LogStream::load(/*stream_id=*/0, *meta_client, kVdevName, vdev, std::move(*stream_blk_opt),
                                           std::move(stream_blk_data));

    auto mgr =
        shared< LogStoreManager >(new LogStoreManager{std::move(meta_client), std::move(vdev), std::move(stream)});

    // 4. Reconstruct each LogStore.  LogStores are NOT opened here — caller drives open_log_store(...) before
    //    invoking recover().
    for (auto& [sid, blk_and_data] : store_blks) {
        auto& [blk, data] = blk_and_data;
        auto wrapper = MetaBlkWrapper::load(mgr->meta_client_, std::move(blk));
        // LogStore::load reads the sb via the wrapper internally; we already have data but pass through wrapper
        // so the LogStore owns its sb mblk handle for future writes.  (We don't reuse `data` here — it's already
        // captured inside the wrapper's lazy read.)
        auto store = co_await LogStore::load(mgr->log_stream_, std::move(wrapper));
        mgr->log_stores_.emplace(sid, std::move(store));
    }

    if (any_store) {
        mgr->next_store_id_.store(max_store_id + 1, std::memory_order_relaxed);
    }

    Managers::init_log_store_mgr(mgr);
    LOGINFO("LogStoreManager: loaded {} log_store(s)", mgr->log_stores_.size());
}

folly::coro::Task< void > LogStoreManager::shutdown() {
    if (log_stream_) {
        co_await log_stream_->stop();
    }
    {
        std::unique_lock lk{stores_mutex_};
        log_stores_.clear();
    }
    log_stream_.reset();
    vdev_.reset();
    Managers::init_log_store_mgr(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

shared< LogStore > LogStoreManager::get_log_store(logstore_id_t store_id) const {
    std::shared_lock lk{stores_mutex_};
    auto it = log_stores_.find(store_id);
    return (it != log_stores_.end()) ? it->second : nullptr;
}

std::vector< shared< LogStore > > LogStoreManager::log_stores() const {
    std::shared_lock lk{stores_mutex_};
    std::vector< shared< LogStore > > out;
    out.reserve(log_stores_.size());
    for (auto& [_, s] : log_stores_) {
        out.push_back(s);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Store lifecycle
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< LogStore > > LogStoreManager::create_log_store(bool append_mode) {
    const logstore_id_t sid = next_store_id_.fetch_add(1, std::memory_order_acq_rel);
    auto store = co_await LogStore::create(sid, meta_client_, log_stream_, append_mode);
    {
        std::unique_lock lk{stores_mutex_};
        log_stores_.emplace(sid, store);
    }
    LOGINFO("LogStoreManager: created log_store sid={} append_mode={}", sid, append_mode);
    co_return store;
}

shared< LogStore > LogStoreManager::open_log_store(logstore_id_t store_id, log_replay_cb handler) {
    auto store = get_log_store(store_id);
    if (!store) {
        LOGWARN("LogStoreManager: open_log_store sid={} — not found", store_id);
        return nullptr;
    }
    store->open(std::move(handler));
    return store;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recovery
// ─────────────────────────────────────────────────────────────────────────────

LogStore* LogStoreManager::lookup_store(logstore_id_t store_id) const {
    std::shared_lock lk{stores_mutex_};
    auto it = log_stores_.find(store_id);
    return (it != log_stores_.end()) ? it->second.get() : nullptr;
}

folly::coro::Task< void > LogStoreManager::recover() {
    LOGINFO("LogStoreManager: starting recover, {} log_store(s) registered", log_stores_.size());

    // Walk the LogStream's CRC chain; per-record on_log_found dispatches into the LogStore via lookup_store.
    co_await log_stream_->recover([this](logstore_id_t sid) { return lookup_store(sid); });

    // Drop unopened log_stores — those without a replay handler are presumed orphaned (their data is dead).
    co_await drop_unopened_stores();

    LOGINFO("LogStoreManager: recover complete, {} log_store(s) remain after orphan cleanup", log_stores_.size());
}

folly::coro::Task< void > LogStoreManager::drop_unopened_stores() {
    std::vector< shared< LogStore > > to_drop;
    {
        std::shared_lock lk{stores_mutex_};
        for (auto& [_, s] : log_stores_) {
            if (!s->is_open()) {
                to_drop.push_back(s);
            }
        }
    }
    for (auto& s : to_drop) {
        const auto sid = s->store_id();
        LOGWARN("LogStoreManager: dropping unopened log_store sid={}", sid);
        // Remove sb mblk so a subsequent restart won't re-discover this store.
        co_await meta_client_->remove_meta_blk(s->sb_blk());
        std::unique_lock lk{stores_mutex_};
        log_stores_.erase(sid);
    }
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// Truncation
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > LogStoreManager::global_truncate() {
    // Min trunc offset across opened stores; std::nullopt-yielding stores (empty stores) don't constrain.
    std::optional< uint64_t > min_off;
    {
        std::shared_lock lk{stores_mutex_};
        for (auto& [_, s] : log_stores_) {
            auto off = s->min_trunc_stream_offset();
            if (!off) {
                continue;
            }
            min_off = min_off ? std::min(*min_off, *off) : *off;
        }
    }
    if (!min_off) {
        LOGDEBUG("LogStoreManager::global_truncate: no store has a trunc anchor, no-op");
        co_return;
    }

    // Build a stream_key with only group_stream_offset populated; LogStream::truncate uses just that field.
    co_await log_stream_->truncate(stream_key{/*log_id=*/-1, /*record_off=*/*min_off, /*group_off=*/*min_off});
    LOGINFO("LogStoreManager::global_truncate: truncated stream to offset {}", *min_off);
}

} // namespace homestore