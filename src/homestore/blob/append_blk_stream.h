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
#include <vector>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <homestore/blk.h>               // BlkId, BlkAllocStatus, blk_count_t, blk_alloc_hints
#include <homestore/homestore_decl.hpp>  // shared<>, unique<>
#include "blob/stream_base.h"            // StreamBase, CPSession, cp_id_t, CPManager::max_concurent_cps
#include "iomanager/drive_interface.hpp" // IOBuffer

namespace homestore {

class Chunk;
class VirtualDev;

// ─────────────────────────────────────────────────────────────────────────────
// WriteUnit
//
// A pre-allocated contiguous BlkId range backing one logical append region.
// All appends are serialised by folly::coro::Mutex; when the range is exhausted a new WriteUnit is created.
//
// finalize() issues the batched writev to VDev and frees any unwritten tail
// blocks.  After finalize() the WriteUnit is read-only.
// ─────────────────────────────────────────────────────────────────────────────
struct WriteUnit {
    BlkId alloc_blkid;            // full pre-allocated range
    uint32_t used_nblks{0};       // blocks actually written
    std::vector< IOBuffer > bufs; // pending buffers in write order

    explicit WriteUnit(BlkId bid) : alloc_blkid{bid} {}
    WriteUnit(const WriteUnit&) = delete;
    WriteUnit& operator=(const WriteUnit&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// AppendBlkCPSession
//
// Holds all append state accumulated within one CP epoch (indexed by cp_id%2).
// Switched over atomically by on_cp_switchover(); flushed by flush(cp).
// ─────────────────────────────────────────────────────────────────────────────
struct AppendBlkCPSession : public StreamBase::CPSession {
    // Active (open) WriteUnit per segment.  MAX_SEGMENTS is an upper bound on
    // the number of independent segments a caller uses concurrently.
    static constexpr uint16_t MAX_SEGMENTS = 64;
    std::array< WriteUnit*, MAX_SEGMENTS > active{}; // nullptr = no open unit

    // All WriteUnits created this epoch (owns the objects via unique_ptr).
    std::vector< unique< WriteUnit > > all_units;

    void reset() {
        active.fill(nullptr);
        all_units.clear();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AppendBlkStream : StreamBase
//
// Append-only block stream.  Callers supply a segment_id and a buffer; the
// stream internally allocates BlkIds and returns them for future reference.
//
// append(segment_id, buf) — acquires folly::coro::Mutex, tries the active WriteUnit for the segment. If the unit is
// full or missing, allocates a new WriteUnit via VDev::alloc_contiguous_blks(), expanding the stream by one chunk if
// allocation fails.
//
// Persistence:
//   Per chunk: one ModuleMetaBlk<uint8_t> storing the full allocator bitmap.
//   On flush: finalize all WriteUnits → writev → commit_blk; then write
//   modified-chunk bitmaps to their MetaBlks.
//
// CP integration:
//   BlobDeviceManager calls on_cp_switchover() / flush(cp) — no direct
//   CPCallbacks registration per stream.
// ─────────────────────────────────────────────────────────────────────────────
class AppendBlkStream : public StreamBase {
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    static folly::coro::Task< shared< AppendBlkStream > > create(MetaClient& meta_client, const std::string& dev_name,
                                                                 const shared< VirtualDev >& vdev, uint64_t chunk_size);

    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, IOBuffer > >;
    static folly::coro::Task< shared< AppendBlkStream > > load(MetaClient& meta_client, const std::string& dev_name,
                                                               const shared< VirtualDev >& vdev, ChunkMblkMap&& mblks);

    AppendBlkStream(const AppendBlkStream&) = delete;
    AppendBlkStream& operator=(const AppendBlkStream&) = delete;
    AppendBlkStream(AppendBlkStream&&) = delete;
    AppendBlkStream& operator=(AppendBlkStream&&) = delete;
    ~AppendBlkStream() override = default;

    // ── IO ─────────────────────────────────────────────────────────────

    /// Append buf into the given segment.  Returns the BlkId of the written block(s).
    folly::coro::Task< BlkId > append(CP* cp, uint16_t segment_id, const IOBuffer& buf);

    /// Invalidate (free) a previously-appended block.  Marks owning chunk dirty.
    void invalidate(const BlkId& bid);

    folly::coro::Task< std::error_code > read(IOBuffer& buf, const BlkId& bid);

    // ── CP hooks ──────────────────────────────────────────────────────────────

    /// Initialize the new CP session for accumulating appends.
    void on_cp_switchover(CP* cur_cp, CP* new_cp);

    /// Finalize all WriteUnits for this CP, issue writev, write dirty bitmaps.
    folly::coro::Task< bool > cp_flush(CP* cp);

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "appendblk"; }

private:
    AppendBlkStream(MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev,
                    uint64_t chunk_size, ChunkMblkMap&& mblks = {});

    /// Try to alloc nblks; expand by one chunk and retry once on failure.
    folly::coro::Task< BlkId > alloc_or_expand(blk_count_t nblks, const blk_alloc_hints& hints);

private:
    // Serialises all append() and on_cp_switchover() mutations. folly::coro::Mutex so it can be held across co_await
    // (e.g. alloc_or_expand).
    folly::coro::Mutex append_mutex_;
    AppendBlkCPSession cp_session_[CPManager::max_concurent_cps];

    // WriteUnit pre-alloc sizes (tunable; min guarantees at least one write).
    static constexpr blk_count_t kMaxWriteUnitBlks = 256;
    static constexpr blk_count_t kMinWriteUnitBlks = 1;
};

} // namespace homestore
