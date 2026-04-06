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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/coro/Task.h>

#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include <homestore/checkpoint/cp_mgr.h> // CPCallbacks

namespace homestore {

class BlobDev;
class MetaClient;
class VirtualDev;
struct VDevParameters;
struct CP;

// ─────────────────────────────────────────────────────────────────────────────
// BlobDevManager
//
// Singleton that owns all BlobDev instances.  Registers ONE CPCallbacks with CPManager for the entire blob subsystem;
// all stream flush/switchover calls are routed through here.
//
// MetaBlk name patterns (used for recovery scan):
//   RawBlkStream     — "<dev>_rawblk_<stream_id>_<chunk_id>"
//   AppendBlkStream  — "<dev>_appendblk_<stream_id>_<chunk_id>"
//   AppendByteStream — "<dev>_appendbyte_<stream_id>_<chunk_id>"
// ─────────────────────────────────────────────────────────────────────────────
class BlobDevManager : public CPCallbacks, public std::enable_shared_from_this< BlobDevManager > {
public:
    // ---------------------------------- Lifecycle ───────────────────────────────

    /// First-time boot: create a fresh manager, register the single MetaClient, and install into Managers.
    static folly::coro::Task< void > create();

    /// Recovery boot: scan all recovered MetaBlks, reconstruct BlobDevs, and install into Managers.
    static folly::coro::Task< void > load();

    BlobDevManager(const BlobDevManager&) = delete;
    BlobDevManager& operator=(const BlobDevManager&) = delete;
    BlobDevManager(BlobDevManager&&) = delete;
    BlobDevManager& operator=(BlobDevManager&&) = delete;
    ~BlobDevManager() override = default;

    /// Shutdown: release all BlobDevs (and their VDev references) before DeviceManager is destroyed.
    void shutdown();

    // -----------------------------Device Management ───────────────────────────────

    /// Create a new BlobDev backed by a new VirtualDev.
    folly::coro::Task< shared< BlobDev > > create_blob_dev(std::string&& dev_name, VDevParameters&& params);

    /// Look up an existing BlobDev by name.  Returns nullptr if not found.
    shared< BlobDev > get_blob_dev(const std::string& dev_name) const;

    // ── CPCallbacks (one registration for the entire blob subsystem) ──────────
    void on_switchover_cp(CP* cur_cp, CP* new_cp) override;
    folly::coro::Task< bool > cp_flush(CP* cp) override;
    void cp_cleanup(CP* cp) override;
    int cp_progress_percent() override;

    /// Reference to the single MetaClient for all blob metadata.
    MetaClient& meta_client() { return meta_client_; }

private:
    explicit BlobDevManager(MetaClient meta_client) : meta_client_{std::move(meta_client)} {}

    MetaClient meta_client_;
    mutable std::mutex devices_mutex_;
    std::unordered_map< std::string, shared< BlobDev > > devices_;
};

} // namespace homestore
