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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <homestore/base/blk.h>              // BlkId
#include "common/defs.h" // shared<>, unique<>, cshared<>

#include "meta/meta_blk.h"         // MetaBlk
#include "meta/meta_client_info.h" // MetaClientInfo

namespace homestore {

class VirtualDev;

// ──────────────────────────────────────────────────────────────────────────────
// MetaClientState
//
// All mutable per-client state, protected by a single coroutine mutex.
// ──────────────────────────────────────────────────────────────────────────────
struct MetaClientState {
    folly::coro::Mutex mutex; // Protects all fields below
    MetaClientInfo info{};
    std::unordered_map< BlkId, MetaBlk > meta_blks;
    BlkId tail_blkid{};                                // Invalid = chain empty
};

// ──────────────────────────────────────────────────────────────────────────────
// MetaClient
//
// Represents one registered metadata client.  Manages a singly-rooted,
// doubly-linked chain of MetaBlks on the meta vdev.
//
// MetaClient is movable but not copyable.
// ──────────────────────────────────────────────────────────────────────────────
class MetaClient {
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /// Create a brand-new client, persist its MetaClientInfo to disk.
    static folly::coro::Task< MetaClient > create(std::string name, uint8_t client_id, shared< VirtualDev > vdev);

    /// Load an existing client from a recovered MetaClientInfo.
    /// Traverses the on-disk chain from info.first_blkid and rebuilds meta_blks.
    static folly::coro::Task< MetaClient > load(MetaClientInfo info, shared< VirtualDev > vdev);

    // ── Queries ───────────────────────────────────────────────────────────────
    folly::coro::Task< uint8_t > client_id() const;
    folly::coro::Task< std::string > client_name() const;
    folly::coro::Task< size_t > num_meta_blks() const;

    // ── Block management ──────────────────────────────────────────────────────

    /// Allocate a fresh MetaBlk (not yet in the chain). Call write_meta_blk() to actually persist and link it.
    folly::coro::Task< MetaBlk > create_meta_blk(std::string_view name, std::optional< size_t > estimated_data_size);

    /// Find a block by name.  Returns std::nullopt if not found.
    folly::coro::Task< std::optional< MetaBlk > > get_meta_blk(std::string_view name);

    /// Write data to a MetaBlk.
    ///
    /// - New block (is_fresh == true): data written, block appended to the tail, client info updated on disk,
    ///   and is_fresh set to false so subsequent calls overwrite in-place.
    /// - Existing block (is_fresh == false): data is overwritten in-place; no relinking.
    folly::coro::Task< void > write_meta_blk(MetaBlk& blk, const sisl::ByteArray& data);

    /// Read the payload from an existing MetaBlk.
    folly::coro::Task< sisl::ByteView > read_meta_blk(const MetaBlk& blk);

    /// Remove a MetaBlk from the chain and free all its blocks on the vdev.
    folly::coro::Task< void > remove_meta_blk(const MetaBlk& blk);

    // ── Recovery ──────────────────────────────────────────────────────────────

    /// Iterate over all recovered blocks lazily, one block at a time.
    ///
    /// visitor signature: folly::coro::Task<void>(const MetaBlk&, sisl::ByteView)
    ///
    /// For inline data the ByteView is a zero-copy window into the cached block buffer. For overflow data the ByteView
    /// wraps a freshly read ByteArray that is released after the visitor returns.
    template < typename Visitor >
    folly::coro::Task< void > for_each_recovered_block(Visitor visitor);

    // ── Move-only ─────────────────────────────────────────────────────────────
    MetaClient() = default;
    MetaClient(MetaClient&&) = default;
    MetaClient& operator=(MetaClient&&) = default;
    MetaClient(const MetaClient&) = delete;
    MetaClient& operator=(const MetaClient&) = delete;

private:
    shared< MetaClientState > state_;
    shared< VirtualDev > meta_vdev_;
    BlkId info_bid_{};

    folly::coro::Task< void > write_client_info(const MetaClientInfo& info);

    /// Calculate the BlkId of the on-disk slot that holds this client's
    /// MetaClientInfo (derived from client_id and the vdev's block size).
    static BlkId calc_info_bid(uint8_t client_id, const VirtualDev& vdev);
};

// ──────────────────────────────────────────────────────────────────────────────
// for_each_recovered_block — template body (must be in the header)
// ──────────────────────────────────────────────────────────────────────────────
template < typename Visitor >
folly::coro::Task< void > MetaClient::for_each_recovered_block(Visitor visitor) {
    // Snapshot the block IDs under the lock, then release before any I/O.
    std::vector< BlkId > blk_ids;
    {
        auto lock = co_await state_->mutex.co_scoped_lock();
        blk_ids.reserve(state_->meta_blks.size());
        for (const auto& [id, _] : state_->meta_blks) {
            blk_ids.push_back(id);
        }
    }

    // Read and visit one block at a time.
    META_LOG(DEBUG, "for_each_recovered_block: {} blocks to iterate", blk_ids.size());
    for (size_t i = 0; i < blk_ids.size(); ++i) {
        const BlkId& id = blk_ids[i];
        MetaBlk blk_copy;
        {
            auto lock = co_await state_->mutex.co_scoped_lock();
            auto it = state_->meta_blks.find(id);
            if (it == state_->meta_blks.end()) continue; // removed concurrently
            blk_copy = it->second; // shared_ptr refcount bump, no memcpy
        }

        META_LOG(DEBUG, "for_each_recovered_block: [{}/{}] name={} blk_num={} reading data", i, blk_ids.size(),
                 blk_copy.name(), id.blk_num());
        sisl::ByteView data = co_await blk_copy.read_data(*meta_vdev_);
        META_LOG(DEBUG, "for_each_recovered_block: [{}/{}] name={} read done, calling visitor", i, blk_ids.size(),
                 blk_copy.name());
        co_await visitor(blk_copy, std::move(data));
    }
}

} // namespace homestore
