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

#include "checkpoint/cp.h"     // CP, cp_id_t
#include "checkpoint/cp_mgr.h" // CPGuard

#include "blob/append_blk_stream.h"
#include "blob/blob_dev.h"
#include "blkalloc/blk_allocator.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"
#include "managers.h" // meta_mgr()

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor and Factory
// ─────────────────────────────────────────────────────────────────────────────
AppendBlkStream::AppendBlkStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                                 const shared< VirtualDev >& vdev, uint64_t chunk_size, ChunkMblkMap&& mblks) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, std::move(mblks)} {
}

folly::coro::Task< shared< AppendBlkStream > > AppendBlkStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                                       const std::string& dev_name,
                                                                       const shared< VirtualDev >& vdev,
                                                                       uint64_t chunk_size) {
    auto stream =
        shared< AppendBlkStream >{new AppendBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size}};
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< AppendBlkStream > > AppendBlkStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                                     const std::string& dev_name,
                                                                     const shared< VirtualDev >& vdev,
                                                                     ChunkMblkMap&& mblks) {
    // Load each chunk's block allocator from the recovered bitmap before the constructor consumes the map.
    for (auto& [cid, entry] : mblks) {
        vdev->load_blk_allocator(cid, entry.second.extract());
    }

    const uint64_t chunk_sz = vdev->chunk_size_bytes();
    auto stream = shared< AppendBlkStream >{
        new AppendBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz, std::move(mblks)}};
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
// IO APIs
// ─────────────────────────────────────────────────────────────────────────────
folly::coro::Task< BlkId > AppendBlkStream::append(CP* cp, uint16_t segment_id, const sisl::ByteArray& buf) {
    if (segment_id >= CPSession::MAX_SEGMENTS) {
        throw std::invalid_argument{"AppendBlkStream: segment_id out of range"};
    }

    CPSession& session = cp_session(cp->id());
    const blk_count_t nblks = static_cast< blk_count_t >((buf->size() + vdev().block_size() - 1) / vdev().block_size());

    auto lock = co_await append_mutex_.co_scoped_lock();

    // Try the active WriteUnit for this segment.
    WriteUnit* wu = session.active[segment_id];
    if (wu == nullptr || wu->used_nblks + nblks > wu->alloc_blkid.blk_count()) {
        blk_alloc_hints hints{.is_contiguous = true};
        BlkId alloc_bid = co_await alloc_or_expand(kMaxWriteUnitBlks, hints);
        session.mark_chunk_dirty(alloc_bid.chunk_num());

        auto wu_owned = std::make_unique< WriteUnit >(alloc_bid);
        wu = wu_owned.get();
        session.all_units.push_back(std::move(wu_owned));
        session.active[segment_id] = wu;
    }

    const uint32_t blk_offset = wu->alloc_blkid.blk_num() + wu->used_nblks;
    wu->used_nblks += nblks;
    wu->bufs.push_back(buf);
    co_return BlkId{blk_offset, nblks, wu->alloc_blkid.chunk_num()};
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
    CPSession& session = cp_session(cp->id());
    auto units = std::move(session.all_units);
    session.active.fill(nullptr);

    // Finalize each WriteUnit: writev pending buffers, commit used blocks, free excess.
    for (auto& wu : units) {
        if (wu->bufs.empty()) {
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

    // Persist allocator bitmaps only for chunks dirtied during this CP epoch.
    auto dirty = cp_session(cp->id()).gather_dirty_chunks();
    if (dirty.empty()) {
        co_return true;
    }

    auto lock = co_await mblk_mutex_.co_scoped_lock();
    for (auto chunk_id : dirty) {
        auto it = chunk_mblks_.find(chunk_id);
        if (it == chunk_mblks_.end()) {
            continue;
        }

        auto chunk = vdev().get_chunk(chunk_id);
        auto buf_guard = chunk->blk_allocator_mutable()->acquire_buffer();
        co_await meta_client_.write_meta_blk(it->second, buf_guard.buf());
    }

    session.reset();
    co_return true;
}

} // namespace homestore