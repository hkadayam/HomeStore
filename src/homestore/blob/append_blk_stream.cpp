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
#include "common/async.h"
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

using sisl::IoBuf;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor and Factory
// ─────────────────────────────────────────────────────────────────────────────
AppendBlkStream::AppendBlkStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                                 const shared< VirtualDev >& vdev, uint64_t chunk_size, uint32_t blk_size,
                                 ChunkMblkMap&& mblks) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, blk_size, std::move(mblks)} {
}

Async< shared< AppendBlkStream > > AppendBlkStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                           const std::string& dev_name,
                                                           const shared< VirtualDev >& vdev, uint64_t chunk_size,
                                                           uint32_t blk_size) {
    auto stream = shared< AppendBlkStream >{
        new AppendBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size, blk_size}};
    co_await stream->expand_to(0);
    co_return stream;
}

Async< shared< AppendBlkStream > > AppendBlkStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                         const std::string& dev_name, const shared< VirtualDev >& vdev,
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

Async< BlkId > AppendBlkStream::alloc_or_expand(blk_count_t nblks, const blk_alloc_hints& hints) {
    BlkId bid;
    blk_alloc_hints h = hints;

    // Pin allocation to THIS stream's chunks via chunk_id_hint.  Without the hint, the vdev's chunk_selector
    // would pick from any chunk in the vdev — including chunks owned by sibling streams (e.g. cow_btree's
    // node_stream stealing blocks from incr_map_stream's chunk because they share one vdev).
    {
        auto chunks_ro = chunks();
        for (auto const& chunk : *chunks_ro) {
            h.chunk_id_hint = chunk->chunk_id();
            if (vdev().alloc_contiguous_blks(nblks, h, bid) == BlkAllocStatus::SUCCESS) {
                co_return bid;
            }
        }
    }

    // No room in any of our chunks — expand and try the newly added one.
    co_await expand_to(num_chunks());
    {
        auto chunks_ro = chunks();
        h.chunk_id_hint = (*chunks_ro).back()->chunk_id();
    }
    if (vdev().alloc_contiguous_blks(nblks, h, bid) != BlkAllocStatus::SUCCESS) {
        throw std::runtime_error{"AppendBlkStream: allocation failed even after expanding"};
    }
    co_return bid;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

std::optional< BlkId > AppendBlkStream::do_quick_append(CPSession& session, uint16_t segment_id,
                                                        unique< WriteUnit > new_wu, sisl::IoBufShared& buf) {
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

Async< void > AppendBlkStream::flush_write_units(const std::vector< unique< WriteUnit > >& units) {
    for (auto& wu : units) {
        if (wu->bufs.empty()) {
            // Unused WriteUnit — free the entire allocation.
            vdev().free_blk(wu->alloc_blkid);
            continue;
        }

        const BlkId used_bid{wu->alloc_blkid.blk_num(), static_cast< blk_count_t >(wu->used_nblks),
                             wu->alloc_blkid.chunk_num()};
        // Build the polymorphic IoBuf pointer list — each shared_ptr in wu->bufs dereferences to an IoBufOwn
        // (IS-A IoBuf), so .get() gives the IoBuf*.  SgList is non-owning; wu->bufs keeps the underlying
        // IoBufOwns alive across the await.
        sisl::SgList sg;
        sg.bufs.reserve(wu->bufs.size());
        for (auto const& b : wu->bufs) {
            sg.bufs.push_back(b.get());
        }
        co_await vdev().writev(sg, used_bid);
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

std::optional< BlkId > AppendBlkStream::quick_append(CP* cp, uint16_t segment_id, sisl::IoBufShared& buf) {
    if (segment_id >= CPSession::MAX_SEGMENTS) {
        return std::nullopt;
    }
    return do_quick_append(cp_session(cp->id()), segment_id, /*new_wu=*/nullptr, buf);
}

Async< BlkId > AppendBlkStream::append(CP* cp, uint16_t segment_id, sisl::IoBufShared&& buf) {
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

Async< void > AppendBlkStream::flush(CP* cp) {
    auto flush_lock = co_await flush_mu_.co_scoped_lock();
    co_await flush_write_units(grab_write_units(cp_session(cp->id())));
}

void AppendBlkStream::invalidate(CP* cp, const BlkId& bid) {
    vdev().free_blk(bid);
    cp_session(cp->id()).mark_chunk_dirty(bid.chunk_num());
}

Async< std::error_code > AppendBlkStream::read(IoBuf& buf, const BlkId& bid) {
    co_return co_await vdev().read(buf, bid);
}

// ─────────────────────────────────────────────────────────────────────────────
// CP hooks
// ─────────────────────────────────────────────────────────────────────────────
Async< bool > AppendBlkStream::cp_flush(CP* cp) {
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