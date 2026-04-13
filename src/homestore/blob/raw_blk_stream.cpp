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

#include "checkpoint/cp.h"     // CP, cp_id_t
#include "checkpoint/cp_mgr.h" // CPGuard
#include "blob/blob_dev.h"     // StreamType
#include "blob/raw_blk_stream.h"
#include "blkalloc/blk_allocator.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"
#include "managers.h" // meta_mgr()

namespace homestore {

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

    const uint64_t chunk_sz = vdev->chunk_size_bytes();
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
    return vdev().alloc_contiguous_blks(nblks, hints, out_blkid);
}

BlkAllocStatus RawBlkStream::alloc_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkIds& out_blkids) {
    return vdev().alloc_blks(nblks, hints, out_blkids);
}

BlkAllocStatus RawBlkStream::commit_blk(CP* cp, const BlkId& bid) {
    const auto status = vdev().commit_blk(bid);
    if (status == BlkAllocStatus::SUCCESS) {
        cp_session(cp->id()).mark_chunk_dirty(bid.chunk_num());
    }
    return status;
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

folly::coro::Task< void > RawBlkStream::write(const BlkId& bid, const IOBuffer& buf, bool buffered) {
    if (buffered) {
        // TODO: Impl buffered write path — read() must return buffered data for overlapping BlkIds, which requires a
        // concurrent hashmap keyed by BlkId rather than a simple vector.
        assert(false && "buffered write not yet implemented");
        co_return;
    }
    co_await vdev().write(buf, bid);
}

folly::coro::Task< void > RawBlkStream::writev(const std::vector< IOBuffer >& bufs, const BlkId& bid) {
    co_await vdev().writev(bufs, bid);
}

folly::coro::Task< std::error_code > RawBlkStream::read(IOBuffer& buf, const BlkId& bid) {
    blk_read_tracker_.insert(bid);
    auto ec = co_await vdev().read(buf, bid);
    blk_read_tracker_.remove(bid);
    co_return ec;
}

folly::coro::Task< std::error_code > RawBlkStream::readv(std::vector< IOBuffer >& bufs, const BlkId& bid) {
    blk_read_tracker_.insert(bid);
    auto ec = co_await vdev().readv(bufs, bid);
    blk_read_tracker_.remove(bid);
    co_return ec;
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
