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

#include <cassert>
#include <cstring>
#include <stdexcept>

#include "common/defs.h"
#include "homestore/meta/meta_client.h"
#include "homestore/meta/meta_blk_manager.h" // META_SUPER_HEADER_SIZE
#include "homestore/device/virtual_dev.h"

namespace homestore {

using sisl::IoBufOwn;

// ──────────────────────────────────────────────────────────────────────────────
// Factory methods: create/load
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< MetaClient > MetaClient::create(std::string name, uint8_t client_id, shared< VirtualDev > vdev) {
    MetaClientInfo info = MetaClientInfo::make_free();
    info.client_id = client_id;
    info.set_allocated();
    info.set_client_name(std::move(name));
    info.first_blkid = BlkId{};
    info.update_crc();

    auto state = std::make_shared< MetaClientState >();
    state->info = std::move(info);
    state->tail_blkid = BlkId{};

    MetaClient client;
    client.meta_vdev_ = std::move(vdev);
    client.info_bid_ = calc_info_bid(client_id, *client.meta_vdev_);
    client.state_ = std::move(state);

    // Persist the client info slot immediately.
    co_await client.write_client_info(client.state_->info);

    co_return client;
}

folly::coro::Task< MetaClient > MetaClient::load(MetaClientInfo info, shared< VirtualDev > vdev) {
    const uint8_t client_id = info.client_id;
    BlkId info_bid = calc_info_bid(client_id, *vdev);

    std::unordered_map< BlkId, MetaBlk > meta_blks;
    BlkId tail_blkid{};

    // Walk the chain starting at info.first_blkid.
    BlkId current_bid = info.first_blkid;
    BlkId prev_bid{};

    META_LOG(DEBUG, "load: client_id={} first_blk_num={} walking chain", client_id,
             current_bid.is_valid() ? current_bid.blk_num() : 0);
    const uint32_t blk_sz = to_u32(vdev->block_size());
    while (current_bid.is_valid()) {
        // Read only the first block of the extent — that's the header + inline data we cache.
        BlkId first_blk{current_bid.blk_num(), 1, current_bid.chunk_num()};
        auto one_blk = sisl::make_io_buf_shared(blk_sz);
        auto err = co_await vdev->read(*one_blk, first_blk);
        if (err) break;

        const auto& hdr = *reinterpret_cast< const MetaBlkHeader* >(one_blk->cbytes());
        if (!hdr.is_valid()) break;

        const BlkId next_bid = hdr.next_bid;
        META_LOG(DEBUG, "load: client_id={} chain blk_num={} name={} data_size={} overflow={} next_valid={}", client_id,
                 current_bid.blk_num(), hdr.get_name(), hdr.data_size, hdr.overflow_bid.is_valid(),
                 next_bid.is_valid());

        tail_blkid = current_bid;

        // Reserve the header block and any overflow blocks in the allocator so they are not reallocated.
        vdev->commit_blk(current_bid);
        if (hdr.overflow_bid.is_valid()) { vdev->commit_blk(hdr.overflow_bid); }

        meta_blks.emplace(current_bid, MetaBlk::load(current_bid, prev_bid, std::move(one_blk)));

        prev_bid = current_bid;
        current_bid = next_bid;
    }
    META_LOG(DEBUG, "load: client_id={} chain walk done, {} blocks loaded", client_id, meta_blks.size());

    auto state = std::make_shared< MetaClientState >();
    state->info = std::move(info);
    state->meta_blks = std::move(meta_blks);
    state->tail_blkid = tail_blkid;

    MetaClient client;
    client.meta_vdev_ = std::move(vdev);
    client.info_bid_ = info_bid;
    client.state_ = std::move(state);

    co_return client;
}

// ──────────────────────────────────────────────────────────────────────────────
// Queries
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< uint8_t > MetaClient::client_id() const {
    auto lock = co_await state_->mutex.co_scoped_lock();
    co_return state_->info.client_id;
}

folly::coro::Task< std::string > MetaClient::client_name() const {
    auto lock = co_await state_->mutex.co_scoped_lock();
    co_return state_->info.get_client_name();
}

folly::coro::Task< size_t > MetaClient::num_meta_blks() const {
    auto lock = co_await state_->mutex.co_scoped_lock();
    co_return state_->meta_blks.size();
}

// ──────────────────────────────────────────────────────────────────────────────
// Meta Blk Management Public APIs
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< MetaBlk > MetaClient::create_meta_blk(std::string_view name,
                                                         std::optional< size_t > estimated_data_size) {
    const uint32_t blk_sz = to_u32(meta_vdev_->block_size());

    // Allocate one block for the header + inline data.
    blk_alloc_hints hints{};
    BlkId out_bid{};
    BlkAllocStatus st = meta_vdev_->alloc_contiguous_blks(1, hints, out_bid);
    if (st != BlkAllocStatus::SUCCESS) { throw std::runtime_error{"MetaClient::create_meta_blk: alloc failed"}; }

    co_return MetaBlk::create(out_bid, blk_sz, name);
}

folly::coro::Task< std::optional< MetaBlk > > MetaClient::get_meta_blk(std::string_view name) {
    auto lock = co_await state_->mutex.co_scoped_lock();
    for (const auto& [id, blk] : state_->meta_blks) {
        if (blk.header().get_name() == name) { co_return blk; }
    }
    co_return std::nullopt;
}

folly::coro::Task< void > MetaClient::write_meta_blk(MetaBlk& mblk, const sisl::IoBufShared& data) {
    // Write data to disk (inline or overflow). Done *before* acquiring state lock so I/O doesn't hold up other callers.
    co_await mblk.write_data(data, *meta_vdev_);

    auto lock = co_await state_->mutex.co_scoped_lock();
    const BlkId key = mblk.blkid;

    // ── In-place update (block already in chain) ──────────────────────────────
    auto it = state_->meta_blks.find(key);
    if (it != state_->meta_blks.end()) {
        assert(state_->info.first_blkid.is_valid());
        it->second = mblk;
        co_return;
    }

    // ── New block: append to the tail ─────────────────────────────────────────
    if (!state_->tail_blkid.is_valid()) {
        // First block in the chain.
        state_->info.first_blkid = key;
        co_await write_client_info(state_->info);
        state_->meta_blks.emplace(key, mblk);
        state_->tail_blkid = key;
    } else {
        // Link current tail → new block.
        const BlkId tail_bid = state_->tail_blkid;
        auto tail_it = state_->meta_blks.find(tail_bid);
        if (tail_it != state_->meta_blks.end()) {
            mblk.prev_bid = tail_bid;
            co_await tail_it->second.update_next_bid(key, *meta_vdev_);
        }
        state_->meta_blks.emplace(key, mblk);
        state_->tail_blkid = key;
    }
}

folly::coro::Task< sisl::IoBufView > MetaClient::read_meta_blk(const MetaBlk& mblk) {
    {
        auto lock = co_await state_->mutex.co_scoped_lock();
        if (!state_->meta_blks.count(mblk.blkid)) {
            throw std::runtime_error{"MetaClient::read_meta_blk: block not found"};
        }
    }
    co_return co_await mblk.read_data(*meta_vdev_);
}

folly::coro::Task< void > MetaClient::remove_meta_blk(const MetaBlk& mblk) {
    auto lock = co_await state_->mutex.co_scoped_lock();

    const BlkId key = mblk.blkid;
    auto it = state_->meta_blks.find(key);
    if (it == state_->meta_blks.end()) {
        // Block was allocated (create_meta_blk) but never committed to the chain via write_meta_blk — just free its
        // allocated storage on the vdev.  This is a valid state for subclasses that lazily persist MetaBlks.
        MetaBlk tmp = mblk;
        co_await tmp.free(*meta_vdev_);
        co_return;
    }

    // Extract the block from the map.
    MetaBlk removed = std::move(it->second);
    state_->meta_blks.erase(it);

    const BlkId prev_bid = removed.prev_bid;
    const BlkId next_bid = removed.header().next_bid;

    if (prev_bid.is_valid()) {
        // Not the head — update prev block's next pointer.
        auto pit = state_->meta_blks.find(prev_bid);
        if (pit != state_->meta_blks.end()) { co_await pit->second.update_next_bid(next_bid, *meta_vdev_); }

        if (!next_bid.is_valid()) {
            // Removed block was the tail.
            state_->tail_blkid = prev_bid;
        } else {
            // Update next block's prev pointer (in-memory only).
            auto nit = state_->meta_blks.find(next_bid);
            if (nit != state_->meta_blks.end()) { nit->second.prev_bid = prev_bid; }
        }
    } else {
        // Removing the head block.
        if (!next_bid.is_valid()) {
            // Chain is now empty.
            state_->info.first_blkid = BlkId{};
            state_->tail_blkid = BlkId{};
        } else {
            // Advance head to next block.
            state_->info.first_blkid = next_bid;
            auto nit = state_->meta_blks.find(next_bid);
            if (nit != state_->meta_blks.end()) { nit->second.prev_bid = BlkId{}; }
        }
        co_await write_client_info(state_->info);
    }

    // Free the block's storage on the vdev.
    co_await removed.free(*meta_vdev_);
}

// ──────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ──────────────────────────────────────────────────────────────────────────────

/// Block offset of this client's info slot:
///   [ super-header blocks ] + [ client_id * info blocks ]
BlkId MetaClient::calc_info_bid(uint8_t client_id, const VirtualDev& vdev) {
    const size_t blk_sz = vdev.block_size();

    const size_t n_super_blks = (META_SUPER_HEADER_SIZE + blk_sz - 1) / blk_sz;

    const size_t info_nblks = (MetaClientInfo::SIZE + blk_sz - 1) / blk_sz;

    const uint32_t blk_num = to_u32(client_id * info_nblks + n_super_blks);

    const chunk_num_t chunk_id = vdev.get_nth_chunk(0)->chunk_id();

    return BlkId{blk_num, static_cast< blk_count_t >(info_nblks), chunk_id};
}

folly::coro::Task< void > MetaClient::write_client_info(const MetaClientInfo& info) {
    // Work on a copy so we can refresh the CRC without touching the caller's copy.
    MetaClientInfo updated = info;
    updated.update_crc();

    IoBufOwn buf{to_u32(MetaClientInfo::SIZE)};
    std::memcpy(buf.bytes(), &updated, MetaClientInfo::SIZE);
    co_await meta_vdev_->write(buf, info_bid_);
}

} // namespace homestore