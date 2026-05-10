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

#include <stdexcept>
#include <string>

#include "homestore/checkpoint/cp.h"     // CP, cp_id_t
#include "homestore/checkpoint/cp_mgr.h" // CPGuard

#include "homestore/blob/append_blk_stream.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/blkalloc/blk_allocator.h"
#include "homestore/device/chunk.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/managers.h" // meta_mgr()

namespace homestore {

using sisl::IOBuffer;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor and Factory
// ─────────────────────────────────────────────────────────────────────────────
AppendBlkStream::AppendBlkStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                                 const shared< VirtualDev >& vdev, uint64_t chunk_size, uint32_t blk_size,
                                 ChunkMblkMap&& mblks) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, blk_size, std::move(mblks)} {
}

folly::coro::Task< shared< AppendBlkStream > > AppendBlkStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                                       const std::string& dev_name,
                                                                       const shared< VirtualDev >& vdev,
                                                                       uint64_t chunk_size, uint32_t blk_size) {
    auto stream = shared< AppendBlkStream >{
        new AppendBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size, blk_size}};
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< AppendBlkStream > > AppendBlkStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                                     const std::string& dev_name,
                                                                     const shared< VirtualDev >& vdev,
                                                                     uint32_t blk_size, ChunkMblkMap&& mblks) {
    // Load each chunk's block allocator from the recovered bitmap before the constructor consumes the map.
    for (auto& [cid, entry] : mblks) {
        vdev->load_blk_allocator(cid, entry.second.extract());
    }

    const uint64_t chunk_sz = vdev->initial_chunk_size();
    auto stream = shared< AppendBlkStream >{
        new AppendBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz, blk_size, std::move(mblks)}};
    co_return stream;
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation of Blocks
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< BlkId > AppendBlkStream::alloc_or_expand(blk_count_t nblks, const blk_alloc_hints& hints) {
    BlkId bid;
    auto status = vdev().alloc_contiguous_blks(nblks, hints, bid);
    if (status == BlkAllocStatus::SUCCESS) {
        co_return bid;
    }

    // No space — add one chunk and retry once.
    co_await expand_to(num_chunks());
    status = vdev().alloc_contiguous_blks(nblks, hints, bid);
    if (status != BlkAllocStatus::SUCCESS) {
        throw std::runtime_error{"AppendBlkStream: allocation failed even after expanding"};
    }
    co_return bid;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

std::optional< BlkId > AppendBlkStream::do_quick_append(CPSession& session, uint16_t segment_id,
                                                        unique< WriteUnit > new_wu, sisl::ByteArray& buf) {
    std::lock_guard lk{mu_};

    // Install the new WriteUnit if provided.
    if (new_wu) {
        session.mark_chunk_dirty(new_wu->alloc_blkid.chunk_num());
        WriteUnit* raw = new_wu.get();
        session.all_units.push_back(std::move(new_wu));
        session.active[segment_id] = raw;
    }

    const blk_count_t nblks = static_cast< blk_count_t >((buf->size() + block_size() - 1) / block_size());
    WriteUnit* wu = session.active[segment_id];
    if (wu == nullptr || wu->used_nblks + nblks > wu->alloc_blkid.blk_count()) {
        return std::nullopt;
    }

    const uint32_t blk_offset = wu->alloc_blkid.blk_num() + wu->used_nblks;
    wu->used_nblks += nblks;
    wu->bufs.push_back(std::move(buf));
    return BlkId{blk_offset, nblks, wu->alloc_blkid.chunk_num()};
}

std::vector< unique< WriteUnit > > AppendBlkStream::grab_write_units(CPSession& session) {
    std::lock_guard lk{mu_};
    session.active.fill(nullptr);
    return std::move(session.all_units);
}

folly::coro::Task< void > AppendBlkStream::flush_write_units(const std::vector< unique< WriteUnit > >& units) {
    for (auto& wu : units) {
        if (wu->bufs.empty()) {
            // Unused WriteUnit — free the entire allocation.
            vdev().free_blk(wu->alloc_blkid);
            continue;
        }

        const BlkId used_bid{wu->alloc_blkid.blk_num(), static_cast< blk_count_t >(wu->used_nblks),
                             wu->alloc_blkid.chunk_num()};
        co_await vdev().writev(wu->bufs, used_bid);
        vdev().commit_blk(used_bid);

        // Free any over-allocated tail blocks.
        if (wu->used_nblks < wu->alloc_blkid.blk_count()) {
            const BlkId excess{wu->alloc_blkid.blk_num() + wu->used_nblks,
                               static_cast< blk_count_t >(wu->alloc_blkid.blk_count() - wu->used_nblks),
                               wu->alloc_blkid.chunk_num()};
            vdev().free_blk(excess);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IO APIs
// ─────────────────────────────────────────────────────────────────────────────

std::optional< BlkId > AppendBlkStream::quick_append(CP* cp, uint16_t segment_id, sisl::ByteArray& buf) {
    if (segment_id >= CPSession::MAX_SEGMENTS) {
        return std::nullopt;
    }
    return do_quick_append(cp_session(cp->id()), segment_id, /*new_wu=*/nullptr, buf);
}

folly::coro::Task< BlkId > AppendBlkStream::append(CP* cp, uint16_t segment_id, sisl::ByteArray&& buf) {
    if (segment_id >= CPSession::MAX_SEGMENTS) {
        throw std::invalid_argument{"AppendBlkStream: segment_id out of range"};
    }

    // First try a quick append
    CPSession& session = cp_session(cp->id());
    auto result = do_quick_append(session, segment_id, /*new_wu=*/nullptr, buf);
    if (result) {
        co_return *result;
    }

    // No room, so allocate a new WriteUnit (may expand the stream).
    blk_alloc_hints hints{.is_contiguous = true};
    BlkId alloc_bid = co_await alloc_or_expand(kMaxWriteUnitBlks, hints);

    // Install and append under mu_.
    result = do_quick_append(session, segment_id, std::make_unique< WriteUnit >(alloc_bid), buf);
    if (!result) {
        vdev().free_blk(alloc_bid);
        throw std::runtime_error{"AppendBlkStream::append: fresh WriteUnit has no room"};
    }
    co_return *result;
}

folly::coro::Task< void > AppendBlkStream::flush(CP* cp) {
    auto flush_lock = co_await flush_mu_.co_scoped_lock();
    co_await flush_write_units(grab_write_units(cp_session(cp->id())));
}

void AppendBlkStream::invalidate(CP* cp, const BlkId& bid) {
    vdev().free_blk(bid);
    cp_session(cp->id()).mark_chunk_dirty(bid.chunk_num());
}

folly::coro::Task< std::error_code > AppendBlkStream::read(IOBuffer& buf, const BlkId& bid) {
    co_return co_await vdev().read(buf, bid);
}

// ─────────────────────────────────────────────────────────────────────────────
// CP hooks
// ─────────────────────────────────────────────────────────────────────────────
folly::coro::Task< bool > AppendBlkStream::cp_flush(CP* cp) {
    auto flush_lock = co_await flush_mu_.co_scoped_lock();
    CPSession& session = cp_session(cp->id());

    // Grab and flush all remaining WriteUnits.
    co_await flush_write_units(grab_write_units(session));

    // Persist allocator bitmaps only for chunks dirtied during this CP epoch.
    auto dirty = session.gather_dirty_chunks();
    if (dirty.empty()) {
        co_return true;
    }

    auto mblk_lock = co_await mblk_mutex_.co_scoped_lock();
    for (auto chunk_id : dirty) {
        auto it = chunk_mblks_.find(chunk_id);
        if (it == chunk_mblks_.end()) {
            continue;
        }

        auto chunk = vdev().get_chunk(chunk_id);
        auto buf_guard = chunk->blk_allocator_mutable()->acquire_buffer();
        if (!buf_guard.buf()) {
            continue;
        }
        co_await meta_client_.write_meta_blk(it->second, buf_guard.buf());
    }

    session.reset();
    co_return true;
}

} // namespace homestore