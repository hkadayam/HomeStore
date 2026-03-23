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
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <folly/coro/Task.h>

#include <homestore/blk.h>              // BlkId, BlkAllocStatus, blk_count_t, blk_alloc_hints
#include <homestore/homestore_decl.hpp> // shared<>, unique<>
#include <sisl/fds/concurrent_insert_vector.h>

#include "blob/blk_read_tracker.h"
#include "blob/stream_base.h"
#include "iomanager/drive_interface.hpp" // IOBuffer

namespace homestore {

class Chunk;
class VirtualDev;
struct CP;

// ─────────────────────────────────────────────────────────────────────────────
// RawBlkStream : StreamBase
//
// Random-access block stream.  Callers hold BlkIds (obtained via alloc_blk)
// and can read, write, or invalidate any block at any time.
//
// Write path:
//   write(bid, buf, buffered=true)  — queues data; flushed on next flush()/cp_flush().
//   write(bid, buf, buffered=false) — immediate VDev I/O.
//   Both paths mark the current CP session dirty.
//   TODO: Impl buffered write path (read must return buffered data for overlapping BlkIds).
//
// Read path:
//   read() wraps vdev read with BlkReadTracker insert/remove so that in-flight reads prevent
//   concurrent invalidate from freeing the same blocks.
//
// Invalidate:
//   co_awaits BlkReadTracker::wait_on() to ensure no in-flight reads overlap the block, then
//   frees via vdev.
//
// Persistence:
//   Each chunk has one ModuleMetaBlk<uint8_t> storing the allocator bitmap.
//   On flush, dirty-chunk bitmaps are written to their MetaBlks.
//   On recovery, bitmaps are loaded and passed to VDev::load_blk_allocator()
//   to restore allocator state exactly.
//
// CP integration:
//   RawBlkStream does NOT register its own CPCallbacks — BlobDeviceManager
//   registers one CPCallbacks for the entire blob subsystem and calls
//   on_cp_switchover() / cp_flush() on each stream.
// ─────────────────────────────────────────────────────────────────────────────
class RawBlkStream : public StreamBase {
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /// Create a fresh stream.  Expands to one initial chunk and creates its MetaBlk.
    static folly::coro::Task< shared< RawBlkStream > > create(MetaClient& meta_client, const std::string& dev_name,
                                                              const shared< VirtualDev >& vdev, uint64_t chunk_size);

    /// Recovery: restore from recovered data (chunk_id → MetaBlk + payload).
    /// Passes stored bitmaps to vdev.load_blk_allocator() to restore allocator state.
    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, IOBuffer > >;
    static folly::coro::Task< shared< RawBlkStream > > load(MetaClient& meta_client, const std::string& dev_name,
                                                            const shared< VirtualDev >& vdev,
                                                            ChunkMblkMap&& mblks);

    RawBlkStream(const RawBlkStream&) = delete;
    RawBlkStream& operator=(const RawBlkStream&) = delete;
    RawBlkStream(RawBlkStream&&) = delete;
    RawBlkStream& operator=(RawBlkStream&&) = delete;
    ~RawBlkStream() override = default;

    // ── Block management ──────────────────────────────────────────────────────

    /// Allocate contiguous blocks from the VDev.  Returns BLK_ALLOC_NOSPACE if
    /// all current chunks are full; caller should call expand() then retry.
    BlkAllocStatus alloc_blk(blk_count_t nblks, const blk_alloc_hints& hints, BlkId& out_blkid);

    /// Allocate possibly non-contiguous blocks from the VDev.
    BlkAllocStatus alloc_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkIds& out_blkids);

    /// Commit a previously allocated block, making it durable across recovery.
    BlkAllocStatus commit_blk(const BlkId& bid);

    /// Invalidate (free) a block.  Waits for any in-flight reads on the block to complete via BlkReadTracker, then
    /// frees via vdev.
    folly::coro::Task< void > invalidate(const BlkId& bid);

    /// Expand stream by one chunk.
    folly::coro::Task< void > expand();

    // ── I/O ───────────────────────────────────────────────────────────────────

    /// Write data to the given block.  buffered=true queues for later flush; buffered=false issues VDev I/O
    /// immediately.  Both paths mark the CP session dirty via CPGuard.
    /// TODO: Impl buffered path — currently asserts buffered==false.
    folly::coro::Task< void > write(const BlkId& bid, const IOBuffer& buf, bool buffered = false);

    /// Scatter-gather write of multiple buffers to a contiguous block range.
    folly::coro::Task< void > writev(std::vector< IOBuffer >&& bufs, const BlkId& bid);

    /// Read into buf.  Tracks the read via BlkReadTracker so invalidate() can wait for it.
    folly::coro::Task< std::error_code > read(IOBuffer& buf, const BlkId& bid);

    /// Scatter-gather read of multiple buffers from a contiguous block range.  Tracked via BlkReadTracker.
    folly::coro::Task< std::error_code > readv(std::vector< IOBuffer >& bufs, const BlkId& bid);

    /// Flush all physical devices backing this stream's chunks.
    folly::coro::Task< void > fsync();

    // ── CP hooks (called by BlobDeviceManager, not registered directly) ───────

    /// Reset dirty flag for the new CP session.
    void on_cp_switchover(CP* cur_cp, CP* new_cp);

    /// Drain buffered writes for the current CP to VDev. Can be called explicitly by the caller at any time.
    folly::coro::Task< void > flush();

    /// CP-driven flush: drains buffered writes + persists allocator bitmaps for all chunks.
    folly::coro::Task< bool > cp_flush(CP* cp);

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "rawblk"; }

private:
    RawBlkStream(MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev, uint64_t chunk_size,
                 ChunkMblkMap&& mblks = {});

    /// Drain buffered writes for the given cp_id to VDev.
    folly::coro::Task< void > do_flush(cp_id_t cp_id);

    // Per-CP write buffer.  Extends base CPSession with a lock-free per-thread vector of pending writes.
    struct RawBlkCPSession : CPSession {
        sisl::ConcurrentInsertVector< std::pair< BlkId, IOBuffer > > writes;
    };
    RawBlkCPSession cp_session_[CPManager::max_concurent_cps];

    BlkReadTracker blk_read_tracker_;
};

} // namespace homestore
