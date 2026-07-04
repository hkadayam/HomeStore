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

#include "homestore/base/blk.h"              // BlkId, BlkAllocStatus, blk_count_t, blk_alloc_hints
#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include "sisl/fds/concurrent_insert_vector.h"

#include "homestore/blob/blk_read_tracker.h"
#include "homestore/blob/stream_base.h"

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
    static folly::coro::Task< shared< RawBlkStream > > create(uint64_t stream_id, MetaClient& meta_client,
                                                              const std::string& dev_name,
                                                              const shared< VirtualDev >& vdev, uint64_t chunk_size,
                                                              uint32_t blk_size = 0);

    /// Recovery: restore from recovered data (chunk_id → MetaBlk + payload).
    /// Passes stored bitmaps to vdev.load_blk_allocator() to restore allocator state.
    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, sisl::IoBufView > >;
    static folly::coro::Task< shared< RawBlkStream > > load(uint64_t stream_id, MetaClient& meta_client,
                                                            const std::string& dev_name,
                                                            const shared< VirtualDev >& vdev, uint32_t blk_size,
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
    BlkAllocStatus commit_blk(CP* cp, const BlkId& bid);

    /// Multi-BlkId commit — loops over bids and issues per-BlkId commit_blk internally.  Returns the first
    /// non-success status encountered (and stops), else SUCCESS.
    BlkAllocStatus commit_blks(CP* cp, BlkIds const& bids);

    /// Invalidate (free) a block.  Waits for any in-flight reads on the block to complete via BlkReadTracker, then
    /// frees via vdev.
    folly::coro::Task< void > invalidate_blk(CP* cp, const BlkId& bid);

    /// Bulk invalidate — sequentially invalidates each BlkId via invalidate_blk.
    folly::coro::Task< void > invalidate_blks(CP* cp, BlkIds const& bids);

    /// Expand stream by one chunk.
    folly::coro::Task< void > expand();

    // ── I/O ───────────────────────────────────────────────────────────────────

    /// Write data to the given block.  buffered=true queues for later flush; buffered=false issues VDev I/O
    /// immediately.  Both paths mark the CP session dirty via CPGuard.
    /// TODO: Impl buffered path — currently asserts buffered==false.
    folly::coro::Task< void > write(const BlkId& bid, const sisl::IoBuf& buf, bool buffered = false);

    /// Scatter-gather write — `sg.bufs` is a polymorphic IoBuf pointer list for one contiguous BlkId range.
    folly::coro::Task< void > writev(sisl::SgList const& sg, const BlkId& bid);

    /// Multi-BlkId write — slices `buf` across `bids` by each BlkId's blk_count * blk_size, issuing one
    /// per-BlkId VDev write internally.  Slicing happens via transient IoBufSpans that alias `buf`'s
    /// bytes — zero copy throughout.  `buf.size()` must equal the total bytes the bids cover.  Caller
    /// keeps `buf` alive across the await.  buffered=true is not yet supported (asserts).
    folly::coro::Task< void > write_multi(BlkIds const& bids, sisl::IoBuf const& buf, bool buffered = false);

    /// Multi-BlkId scatter-gather write — walks `sg`'s polymorphic IoBuf pointer list and routes bytes to
    /// each BlkId in `bids` by per-BlkId byte count (blk_count * blk_size).  An IoBuf that straddles a
    /// BlkId boundary is sliced via a transient IoBufSpan that aliases its underlying bytes — zero copy
    /// throughout.  `sg.total_size()` may be less than `bids` total bytes; the trailing partial BlkId
    /// gets a short write and DriveInterface pads to LBA-multiple internally (tail bytes on disk are
    /// stale until the BlkId is freed and reused — the on-disk record's value_size bounds the reader).
    folly::coro::Task< void > writev_multi(BlkIds const& bids, sisl::SgList const& sg, bool buffered = false);

    /// Read into buf.  Tracks the read via BlkReadTracker so invalidate() can wait for it.
    folly::coro::Task< std::error_code > read(sisl::IoBuf& buf, const BlkId& bid);

    /// Scatter-gather read — `sg.bufs` is a polymorphic IoBuf pointer list of destinations for one
    /// contiguous BlkId range.  Tracked via BlkReadTracker.
    folly::coro::Task< std::error_code > readv(sisl::SgList const& sg, const BlkId& bid);

    /// Multi-BlkId read — slices `buf` across `bids` by each BlkId's blk_count * blk_size, issuing one
    /// per-BlkId VDev read into the corresponding slice.  Slicing happens via transient IoBufSpans that
    /// alias `buf`'s bytes — zero copy throughout.  Returns the first non-zero error_code encountered,
    /// else {}.
    folly::coro::Task< std::error_code > read_multi(BlkIds const& bids, sisl::IoBuf& buf);

    /// Flush all physical devices backing this stream's chunks.
    folly::coro::Task< void > fsync();

    /// Drain buffered writes for the current CP to VDev. Can be called explicitly by the caller at any time.
    folly::coro::Task< void > buffered_write_flush();

    /// CP-driven flush: drains buffered writes + persists allocator bitmaps for all chunks.
    folly::coro::Task< bool > cp_flush(CP* cp);

    /// Returns true if any chunks were dirtied during the given CP epoch.
    bool is_dirty(cp_id_t cp_id) { return cp_session(cp_id).has_dirty_chunks(); }

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "rawblk"; }

private:
    RawBlkStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev,
                 uint64_t chunk_size, uint32_t blk_size = 0, ChunkMblkMap&& mblks = {});

    // Per-CP write buffer.  Extends base FlushSessionBase with a lock-free per-thread vector of pending writes.
    struct CPSession : StreamBase::FlushSessionBase {
        sisl::ConcurrentInsertVector< std::pair< BlkId, sisl::IoBuf > > writes;
    };
    CPSession cp_session_[CPManager::max_concurent_cps];
    CPSession& cp_session(cp_id_t cp_id) { return cp_session_[cp_id % CPManager::max_concurent_cps]; }

    BlkReadTracker blk_read_tracker_;
};

} // namespace homestore
