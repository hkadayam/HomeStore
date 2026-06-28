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
#include <cstdlib>
#include <stdexcept>

#include "homestore/checkpoint/cp.h"     // CP, cp_id_t
#include "homestore/checkpoint/cp_mgr.h" // CPGuard
#include "homestore/blob/blob_dev.h"     // StreamType
#include "homestore/blob/raw_blk_stream.h"
#include "homestore/blkalloc/blk_allocator.h"
#include "homestore/device/chunk.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/managers.h" // meta_mgr()

namespace homestore {

using sisl::IoBuf;

// ─────────────────────────────────────────────────────────────────────────────
//                              Factory and Constructor
// ─────────────────────────────────────────────────────────────────────────────
folly::coro::Task< shared< RawBlkStream > > RawBlkStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                                 const std::string& dev_name,
                                                                 const shared< VirtualDev >& vdev,
                                                                 uint64_t chunk_size, uint32_t blk_size) {
    auto stream =
        shared< RawBlkStream >{new RawBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size, blk_size}};
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< RawBlkStream > > RawBlkStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                               const std::string& dev_name,
                                                               const shared< VirtualDev >& vdev, uint32_t blk_size,
                                                               ChunkMblkMap&& mblks) {
    // Load each chunk's block allocator from the recovered bitmap before the constructor consumes the map.
    for (auto& [cid, entry] : mblks) {
        vdev->load_blk_allocator(cid, entry.second.extract());
    }

    const uint64_t chunk_sz = vdev->initial_chunk_size();
    auto stream = shared< RawBlkStream >{
        new RawBlkStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz, blk_size, std::move(mblks)}};
    co_return stream;
}

RawBlkStream::RawBlkStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                           const shared< VirtualDev >& vdev, uint64_t chunk_size, uint32_t blk_size,
                           ChunkMblkMap&& mblks) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, blk_size, std::move(mblks)} {
}

// ─────────────────────────────────────────────────────────────────────────────
// Block management
// ─────────────────────────────────────────────────────────────────────────────

BlkAllocStatus RawBlkStream::alloc_blk(blk_count_t nblks, const blk_alloc_hints& hints, BlkId& out_blkid) {
    // Pin allocation to THIS stream's chunks via chunk_id_hint.  Without the hint, the vdev's chunk_selector
    // would pick from any chunk in the vdev — including chunks owned by sibling streams that share the vdev.
    blk_alloc_hints h = hints;
    auto chunks_ro = chunks();
    BlkAllocStatus last = BlkAllocStatus::SPACE_FULL;
    for (auto const& chunk : *chunks_ro) {
        h.chunk_id_hint = chunk->chunk_id();
        last = vdev().alloc_contiguous_blks(nblks, h, out_blkid);
        if (last == BlkAllocStatus::SUCCESS) {
            return last;
        }
    }
    return last;
}

BlkAllocStatus RawBlkStream::alloc_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkIds& out_blkids) {
    // Same chunk-scoping as alloc_blk: try each of our chunks in turn so blocks come only from this stream.
    blk_alloc_hints h = hints;
    auto chunks_ro = chunks();
    BlkAllocStatus last = BlkAllocStatus::SPACE_FULL;
    for (auto const& chunk : *chunks_ro) {
        h.chunk_id_hint = chunk->chunk_id();
        last = vdev().alloc_blks(nblks, h, out_blkids);
        if (last == BlkAllocStatus::SUCCESS) {
            return last;
        }
    }
    return last;
}

BlkAllocStatus RawBlkStream::commit_blk(CP* cp, const BlkId& bid) {
    const auto status = vdev().commit_blk(bid);
    if (status == BlkAllocStatus::SUCCESS) {
        cp_session(cp->id()).mark_chunk_dirty(bid.chunk_num());
    }
    return status;
}

BlkAllocStatus RawBlkStream::commit_blks(CP* cp, BlkIds const& bids) {
    for (auto const& b : bids) {
        auto status = commit_blk(cp, b);
        if (status != BlkAllocStatus::SUCCESS) {
            return status;
        }
    }
    return BlkAllocStatus::SUCCESS;
}

folly::coro::Task< void > RawBlkStream::invalidate(CP* cp, const BlkId& bid) {
    // Wait for any in-flight reads on this block to complete before freeing.
    co_await blk_read_tracker_.wait_on(bid);
    vdev().free_blk(bid);
    cp_session(cp->id()).mark_chunk_dirty(bid.chunk_num());
}

folly::coro::Task< void > RawBlkStream::expand() {
    co_await expand_to(num_chunks());
}

// ─────────────────────────────────────────────────────────────────────────────
// I/O
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > RawBlkStream::write(const BlkId& bid, const IoBuf& buf, bool buffered) {
    if (buffered) {
        // TODO: Impl buffered write path — read() must return buffered data for overlapping BlkIds, which requires a
        // concurrent hashmap keyed by BlkId rather than a simple vector.
        assert(false && "buffered write not yet implemented");
        co_return;
    }
    co_await vdev().write(buf, bid);
}

folly::coro::Task< void > RawBlkStream::writev(sisl::SgList const& sg, const BlkId& bid) {
    co_await vdev().writev(sg, bid);
}

folly::coro::Task< void > RawBlkStream::write_multi(BlkIds const& bids, sisl::IoBuf const& buf, bool buffered) {
    HS_REL_ASSERT(!buffered, "buffered multi-BlkId write not yet implemented");
    auto const blk_size = block_size();
    uint32_t off = 0;
    for (auto const& b : bids) {
        auto const this_bytes = to_u32(b.blk_count() * blk_size);
        sisl::IoBufSpan slice{buf.cbytes() + off, this_bytes, buf.is_aligned()};
        co_await vdev().write(slice, b);
        off += this_bytes;
    }
    co_return;
}

folly::coro::Task< void > RawBlkStream::writev_multi(BlkIds const& bids, sisl::SgList const& sg, bool buffered) {
    HS_REL_ASSERT(!buffered, "buffered multi-BlkId writev not yet implemented");
    auto const blk_size = block_size();

    // Backing storage for sliced IoBufSpans — an IoBuf that straddles a BlkId boundary becomes one span
    // per slice.  Worst case is one boundary crossing per BlkId, so reserve sg.bufs.size() + bids.size()
    // to guarantee pointers stay stable across emplace_back.
    std::vector< sisl::IoBufSpan > spans;
    spans.reserve(sg.bufs.size() + bids.size());

    size_t buf_idx = 0;
    uint32_t buf_off = 0; // offset into sg.bufs[buf_idx]
    for (auto const& bid : bids) {
        uint32_t want = to_u32(bid.blk_count() * blk_size);
        sisl::SgList per_bid;
        while (want > 0 && buf_idx < sg.bufs.size()) {
            auto* current = sg.bufs[buf_idx];
            uint32_t buf_remaining = current->size() - buf_off;
            uint32_t take = std::min(want, buf_remaining);
            if (buf_off == 0 && take == current->size()) {
                per_bid.bufs.push_back(current);
            } else {
                spans.emplace_back(current->bytes() + buf_off, take, current->is_aligned());
                per_bid.bufs.push_back(&spans.back());
            }
            buf_off += take;
            want -= take;
            if (buf_off == current->size()) {
                ++buf_idx;
                buf_off = 0;
            }
        }
        // sg may be shorter than the BlkId's full byte capacity — DriveInterface tail-pads to LBA-multiple.
        co_await vdev().writev(per_bid, bid);
    }
    co_return;
}

folly::coro::Task< std::error_code > RawBlkStream::read(IoBuf& buf, const BlkId& bid) {
    blk_read_tracker_.insert(bid);
    auto ec = co_await vdev().read(buf, bid);
    blk_read_tracker_.remove(bid);
    co_return ec;
}

folly::coro::Task< std::error_code > RawBlkStream::readv(sisl::SgList const& sg, const BlkId& bid) {
    blk_read_tracker_.insert(bid);
    auto ec = co_await vdev().readv(sg, bid);
    blk_read_tracker_.remove(bid);
    co_return ec;
}

folly::coro::Task< std::error_code > RawBlkStream::read_multi(BlkIds const& bids, sisl::IoBuf& buf) {
    auto const blk_size = block_size();
    uint32_t off = 0;
    for (auto const& b : bids) {
        auto const this_bytes = to_u32(b.blk_count() * blk_size);
        sisl::IoBufSpan slice{buf.bytes() + off, this_bytes, buf.is_aligned()};
        blk_read_tracker_.insert(b);
        auto ec = co_await vdev().read(slice, b);
        blk_read_tracker_.remove(b);
        if (ec) {
            co_return ec;
        }
        off += this_bytes;
    }
    co_return std::error_code{};
}

folly::coro::Task< void > RawBlkStream::fsync() {
    co_await vdev().fsync();
}

// ─────────────────────────────────────────────────────────────────────────────
// CP hooks
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > RawBlkStream::buffered_write_flush() {
    // TODO: Buffered writes are not implemented yet (because it also needs to make sure they are readable), so its
    // flush is marked unimplemented
    co_return;
}

folly::coro::Task< bool > RawBlkStream::cp_flush(CP* cp) {
    co_await buffered_write_flush();

    auto dirty = cp_session(cp->id()).gather_dirty_chunks();
    if (dirty.empty()) { co_return true; }

    // Persist allocator bitmaps only for chunks dirtied during this CP epoch.
    auto lock = co_await mblk_mutex_.co_scoped_lock();
    for (auto chunk_id : dirty) {
        auto it = chunk_mblks_.find(chunk_id);
        if (it == chunk_mblks_.end()) { continue; }

        auto chunk = vdev().get_chunk(chunk_id);
        auto buf_guard = chunk->blk_allocator_mutable()->acquire_buffer();
        co_await meta_client_.write_meta_blk(it->second, buf_guard.buf());
    }

    co_return true;
}

} // namespace homestore
