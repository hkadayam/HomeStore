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

#include <algorithm>
#include "common/async.h"
#include <optional>
#include <utility>

#include <fmt/format.h>
#include "sisl/logging/logging.h"

#include "homestore/blob/stream_base.h"
#include "homestore/device/chunk.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/base/event_manager.h"  // EventManager::publish
#include "homestore/base/resource_event.h" // ResourceEvent
#include "homestore/meta/meta_client.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// Constructors and Destructors
// ─────────────────────────────────────────────────────────────────────────────

StreamBase::StreamBase(uint64_t stream_id, const shared< VirtualDev >& vdev, MetaClient& meta_client,
                       std::string dev_name, uint64_t chunk_size, uint32_t stream_blk_size, ChunkMblkMap&& mblks) :
        meta_client_{meta_client},
        stream_id_{stream_id},
        vdev_{vdev},
        dev_name_{std::move(dev_name)},
        chunk_size_{chunk_size},
        blk_size_{(stream_blk_size == 0) ? vdev_->block_size() : stream_blk_size},
        blk_multiplier_{blk_size_ / vdev_->block_size()} {
    if (mblks.empty()) {
        return;
    }

    // Extract chunks from VDev by chunk_id and sort by vdev_order.
    std::vector< shared< Chunk > > sorted;
    sorted.reserve(mblks.size());
    for (auto& [cid, entry] : mblks) {
        auto chunk = vdev_->get_chunk(cid);
        if (!chunk) {
            LOGWARN("StreamBase: chunk_id={} not found in VDev — skipping", cid);
            continue;
        }
        sorted.push_back(std::move(chunk));
        chunk_mblks_.emplace(cid, std::move(entry.first));
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a->vdev_order() < b->vdev_order(); });
    chunks_.make_and_exchange(std::move(sorted));
}

Async< void > StreamBase::destroy() {
    auto lock = co_await expand_mutex_.co_scoped_lock();

    std::vector< shared< Chunk > > old_list;
    {
        auto acc = chunks_.get();
        old_list = *acc;
    }

    // Install empty list so any concurrent readers immediately see no chunks.
    chunks_.make_and_exchange(std::vector< shared< Chunk > >{});

    for (auto& chunk : old_list) {
        co_await remove_chunk_mblk(chunk->chunk_id());
        co_await vdev_->shrink(ChunkToShrink::Specific, chunk->chunk_id());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Expansion and Truncation
// ─────────────────────────────────────────────────────────────────────────────

Async< void > StreamBase::expand_to(size_t n) {
    LOGTRACEMOD(blob_dev, "expand_to(n={}) stream={}: acquiring expand_mutex", n, dev_name_);
    auto lock = co_await expand_mutex_.co_scoped_lock();

    // Snapshot current list — release the RCU guard before any co_await.
    std::vector< shared< Chunk > > new_list;
    {
        auto acc = chunks_.get();
        new_list = *acc; // copy; acc (rcu_reader) drops at end of block
    }

    LOGTRACEMOD(blob_dev, "expand_to(n={}) stream={}: got lock, cur_chunks={}", n, dev_name_, new_list.size());
    if (new_list.size() > n) {
        co_return; // already has enough chunks
    }

    // Expand until we have at least n+1 chunks.  vdev_->expand() throws when the device physically has no room for
    // another chunk — the true, single source of write-path space exhaustion.  Signal DiskFullOnWrite so
    // ResourceMgr can reclaim space (e.g. truncate the log stream), then re-throw: this expand still fails and the
    // caller's write aborts, to be retried once reclaim frees chunks.
    const size_t old_size = new_list.size();
    while (new_list.size() <= n) {
        shared< Chunk > chunk;
        try {
            chunk = co_await vdev_->expand(chunk_size_);
        } catch (const std::exception& e) {
            LOGERROR("StreamBase[{}]: chunk expand failed ({}) — device out of space, raising DiskFullOnWrite",
                     dev_name_, e.what());
            EventManager::publish(ResourceEvent{ResourceEvent::Kind::DiskFullOnWrite, dev_name_, chunk_size_});
            throw;
        }
        new_list.push_back(std::move(chunk));
    }

    // Keep references to newly added chunks before the vector is moved.
    std::vector< shared< Chunk > > added(new_list.begin() + static_cast< ptrdiff_t >(old_size), new_list.end());

    // Atomically install the new list, waiting for any in-flight RCU readers.
    chunks_.make_and_exchange(std::move(new_list));

    // Create a MetaBlk for each newly added chunk.
    for (auto& chunk : added) {
        co_await init_chunk_mblk(chunk);
    }
}

Async< void > StreamBase::init_chunk_mblk(const shared< Chunk >& chunk) {
    const uint32_t cid = chunk->chunk_id();
    auto name = fmt::format("{}_{}_{}_{}_{}", dev_name_, stream_type_name(), stream_id_, cid, blk_size_);
    auto blk = co_await meta_client_.create_meta_blk(name, std::nullopt);

    auto lock = co_await mblk_mutex_.co_scoped_lock();
    chunk_mblks_.emplace(cid, std::move(blk));
}

Async< void > StreamBase::remove_chunk_mblk(uint32_t chunk_id) {
    MetaBlk blk;
    {
        auto lock = co_await mblk_mutex_.co_scoped_lock();
        auto it = chunk_mblks_.find(chunk_id);
        if (it == chunk_mblks_.end())
            co_return;
        blk = std::move(it->second);
        chunk_mblks_.erase(it);
    }
    co_await meta_client_.remove_meta_blk(blk);
}

void StreamBase::install_chunks(std::vector< shared< Chunk > > chunks) {
    std::sort(chunks.begin(), chunks.end(),
              [](const auto& a, const auto& b) { return a->vdev_order() < b->vdev_order(); });
    chunks_.make_and_exchange(std::move(chunks));
}

Async< void > StreamBase::truncate_before(size_t nchunks) {
    auto lock = co_await expand_mutex_.co_scoped_lock();

    std::vector< shared< Chunk > > old_list;
    {
        auto acc = chunks_.get();
        old_list = *acc;
    }

    if (nchunks == 0 || nchunks > old_list.size()) {
        co_return;
    }

    // Install the trimmed list atomically first so new readers never see the to-be-removed chunks.
    std::vector< shared< Chunk > > new_list(old_list.begin() + static_cast< ptrdiff_t >(nchunks), old_list.end());
    chunks_.make_and_exchange(new_list);

    // Now release the removed chunks: drop their MetaBlks then return them to the vdev.
    for (size_t i = 0; i < nchunks; ++i) {
        co_await remove_chunk_mblk(old_list[i]->chunk_id());
        co_await vdev_->shrink(ChunkToShrink::Specific, old_list[i]->chunk_id());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Read-side (lock-free)
// ─────────────────────────────────────────────────────────────────────────────
sisl::Rcu::access_ptr< std::vector< shared< Chunk > > > StreamBase::chunks() const {
    return chunks_.get();
}

size_t StreamBase::num_chunks() const {
    auto acc = chunks_.get();
    return acc->size();
}

} // namespace homestore