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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <homestore/base/blk.h>              // BlkId
#include "common/defs.h" // shared<>, unique<>, cshared<>

#include "meta/meta_client.h"      // MetaClient
#include "meta/meta_client_info.h" // MetaClientInfo, MAX_META_CLIENTS

namespace homestore {

class VirtualDev;

// ──────────────────────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t META_SUPER_HEADER_MAGIC = 0xABCD9876u;
static constexpr uint32_t META_SUPER_HEADER_VERSION = 0x1u;
static constexpr size_t META_SUPER_HEADER_SIZE = 512;

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkSuperHeader
//
// First block written on the meta vdev.  Exactly 512 bytes so it occupies
// one full block and the client-info slots follow at a clean offset.
// ──────────────────────────────────────────────────────────────────────────────
#pragma pack(1)
struct MetaBlkSuperHeader {
    uint32_t magic{0};
    uint32_t version{0};
    uint8_t padding[504]{};

    static constexpr size_t SIZE = META_SUPER_HEADER_SIZE;

    static MetaBlkSuperHeader make() {
        MetaBlkSuperHeader h;
        h.magic = META_SUPER_HEADER_MAGIC;
        h.version = META_SUPER_HEADER_VERSION;
        return h;
    }

    bool is_valid() const { return magic == META_SUPER_HEADER_MAGIC && version == META_SUPER_HEADER_VERSION; }
};
#pragma pack()

static_assert(sizeof(MetaBlkSuperHeader) == META_SUPER_HEADER_SIZE,
              "MetaBlkSuperHeader must be exactly META_SUPER_HEADER_SIZE bytes");

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkManager
//
// Manages the meta vdev and all registered MetaClients.
//
// Layout on disk:
//   Block 0 … N-1  : MetaBlkSuperHeader + MAX_META_CLIENTS × MetaClientInfo
//   Block N …      : MetaBlk chains (one per client)
//
// A single global instance is held via set_instance() / instance().
//
// Non-copyable, non-movable.
// ──────────────────────────────────────────────────────────────────────────────
class MetaBlkManager {
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /// Format a brand-new meta vdev and install this manager into Managers.  The vdev is created with one chunk of
    /// `chunk_size` bytes and grows in `chunk_size` increments via VirtualDev::expand() as more space is needed.
    static folly::coro::Task< void > create(uint64_t chunk_size);

    /// Load an existing meta vdev and install this manager into Managers.
    static folly::coro::Task< void > load();

    // ── Client registration ───────────────────────────────────────────────────

    /// Register a client by name.
    ///
    /// If a matching entry was recovered from disk (i.e. the system restarted), the existing MetaClient (with its full
    /// block chain) is returned. Otherwise a fresh MetaClient is created and its slot is written to disk.
    folly::coro::Task< MetaClient > register_client(std::string name);

    /// Free the client's slot so it can be reused.  The caller is responsible for removing any MetaBlks before
    /// deregistering.
    folly::coro::Task< void > deregister_client(const MetaClient& client);

    // ── Non-copyable, non-movable ─────────────────────────────────────────────
    MetaBlkManager(const MetaBlkManager&) = delete;
    MetaBlkManager& operator=(const MetaBlkManager&) = delete;
    MetaBlkManager(MetaBlkManager&&) = delete;
    MetaBlkManager& operator=(MetaBlkManager&&) = delete;

private:
    MetaBlkManager() = default;

    // ── Private helpers ───────────────────────────────────────────────────────
    folly::coro::Task< void > load_client_info_from_disk();
    folly::coro::Mutex mgmt_mutex_;

    /// Find the first free slot, mark it allocated, return its index. Throws std::runtime_error if all slots are
    /// occupied.
    static size_t reserve_slot_internal(std::vector< uint8_t >& slots);

private:
    shared< VirtualDev > meta_vdev_;
    BlkId client_info_bid_{};
    std::vector< uint8_t > client_slots_;                                 // One byte per slot: 0 = free, 1 = allocated.
    std::unordered_map< std::string, MetaClientInfo > recovered_clients_; // Populated during load();
};

} // namespace homestore
