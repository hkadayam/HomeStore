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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/coro/SharedMutex.h>
#include <folly/coro/Task.h>

#include <homestore/blk.h>              // BlkId, blk_count_t, chunk_num_t
#include <homestore/homestore_decl.hpp> // shared<>, unique<>
#include <sisl/fds/buffer.h>            // sisl::Blob

#include "blob/stream_base.h"            // StreamBase, CPSession, cp_id_t, CPManager::max_concurent_cps
#include "iomanager/drive_interface.hpp" // IOBuffer

namespace homestore {

class Chunk;
class VirtualDev;

// ─────────────────────────────────────────────────────────────────────────────
// AppendByteChunkMeta
//
// On-disk payload for one chunk's MetaBlk.  Stores only how many bytes have been written into this chunk so far.
// Recovery sums bytes_written across all chunks (ordered by creation_order) to reconstruct tail_offset.
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(8) AppendByteChunkMeta {
    uint64_t bytes_written{0};
};
static_assert(std::is_trivially_copyable_v< AppendByteChunkMeta >);
static_assert(std::is_standard_layout_v< AppendByteChunkMeta >);

// ─────────────────────────────────────────────────────────────────────────────
// AppendByteStream : StreamBase
//
// Byte-stream append.  Callers never see block addresses; the stream tracks a logical tail_offset in bytes.  No block
// allocator — bytes are written sequentially into chunks; when a chunk is full the stream expands to the next one.
//
// append(cp_id, data) — hot path takes folly::coro::SharedMutex shared lock, CAS-bumps an atomic offset on the active
// WriteUnit, and memcpys caller bytes into the pre-allocated aligned IOBuffer.  When the buffer is full, the cold path
// takes an exclusive lock, allocates a new WriteUnit, and retries.  Each WriteUnit's buffer never crosses a chunk
// boundary.
//
// Chunk MetaBlk payload: AppendByteChunkMeta (bytes_written per chunk).
// Recovery: sum bytes_written across all chunks (ordered by creation_order).
//
// Truncate: reset tail_offset to 0; existing chunks remain allocated for reuse.
//
// CP integration:
//   on_cp_switchover(cp)  — initialize the new CP session.
//   cp_flush(cp)          — single writer; writes used portions of WriteUnits to VDev, updates MetaBlks.
// ─────────────────────────────────────────────────────────────────────────────
class AppendByteStream : public StreamBase {
public:
    // ── WriteUnit ────────────────────────────────────────────────────────────
    // A pre-allocated block-aligned IOBuffer positioned at a fixed offset within one chunk.  Multiple appenders reserve
    // space concurrently via CAS on used_bytes; each then memcpys into their reserved slot.  The buffer never crosses a
    // chunk boundary.
    struct WriteUnit {
        chunk_num_t chunk_id;                  // which chunk this unit writes into
        uint32_t offset_in_chunk;              // starting byte offset within the chunk
        IOBuffer buf;                          // pre-allocated, block-aligned
        std::atomic< uint32_t > used_bytes{0}; // CAS bump allocator

        WriteUnit(chunk_num_t cid, uint32_t offset, IOBuffer io_buf) :
                chunk_id{cid}, offset_in_chunk{offset}, buf{std::move(io_buf)} {}
        WriteUnit(const WriteUnit&) = delete;
        WriteUnit& operator=(const WriteUnit&) = delete;
    };

    // ── AppendByteCPSession ──────────────────────────────────────────────────
    // Per-CP-epoch state.  All WriteUnits created during this epoch are owned here.  write_cursor tracks the byte
    // offset at which the next WriteUnit starts; initialized lazily from tail_offset on first allocation.
    struct AppendByteCPSession : public StreamBase::CPSession {
        std::vector< unique< WriteUnit > > all_units;
        uint64_t write_cursor{0}; // byte offset past the end of the last allocated WriteUnit's capacity

        void reset() {
            all_units.clear();
            write_cursor = 0;
        }
    };

    // ── Factories ─────────────────────────────────────────────────────────────

    static folly::coro::Task< shared< AppendByteStream > >
    create(MetaClient& meta_client, const std::string& dev_name, const shared< VirtualDev >& vdev, uint64_t chunk_size);

    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, IOBuffer > >;
    static folly::coro::Task< shared< AppendByteStream > > load(MetaClient& meta_client, const std::string& dev_name,
                                                                const shared< VirtualDev >& vdev, ChunkMblkMap&& mblks);

    AppendByteStream(const AppendByteStream&) = delete;
    AppendByteStream& operator=(const AppendByteStream&) = delete;
    AppendByteStream(AppendByteStream&&) = delete;
    AppendByteStream& operator=(AppendByteStream&&) = delete;
    ~AppendByteStream() override = default;

    // ── IO ───────────────────────────────────────────────────────────────────

    /// Append data bytes into the stream.  Returns the byte offset at which the data was written.  Hot path is
    /// lock-free (shared lock + CAS); cold path (buffer full) takes exclusive lock and allocates a new WriteUnit.
    folly::coro::Task< uint64_t > append(cp_id_t cp_id, sisl::Blob data);

    /// Read len bytes starting at byte_offset into buf.
    folly::coro::Task< std::pair< std::error_code, IOBuffer > > read(IOBuffer buf, uint64_t byte_offset, size_t len);

    /// Reset tail_offset to 0.  Existing chunks are retained and reused.
    folly::coro::Task< void > truncate();

    // ── CP hooks (called by BlobDeviceManager) ───────────────────────────────

    /// Initialize the new CP session for accumulating appends.
    void on_cp_switchover(CP* cur_cp, CP* new_cp);

    /// Write used portions of all WriteUnits to VDev, update chunk MetaBlks with bytes_written.
    folly::coro::Task< bool > cp_flush(CP* cp);

    // ── Accessors ─────────────────────────────────────────────────────────────
    uint64_t tail_offset() const { return tail_offset_.load(std::memory_order_acquire); }

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "appendbyte"; }

private:
    AppendByteStream(MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev,
                     uint64_t chunk_size, ChunkMblkMap&& mblks = {});

    /// Allocate a new WriteUnit at the current write_cursor position.  Caps the buffer to not cross the chunk boundary.
    /// Expands to a new chunk if needed.  Called under exclusive lock.
    folly::coro::Task< WriteUnit* > alloc_write_unit(AppendByteCPSession& session, cp_id_t cp_id);

private:
    // Logical byte tail.  Updated atomically by append() after each successful CAS reservation.
    std::atomic< uint64_t > tail_offset_{0};

    // SharedMutex: shared lock for hot-path CAS+memcpy; exclusive lock for cold-path new WriteUnit allocation.
    folly::coro::SharedMutex append_mutex_;
    AppendByteCPSession cp_session_[CPManager::max_concurent_cps];

    // Maximum buffer size per WriteUnit (in bytes).  Actual size may be smaller near chunk boundaries.
    static constexpr uint32_t kMaxWriteUnitBytes = 256 * 4096; // 1 MB
};

} // namespace homestore