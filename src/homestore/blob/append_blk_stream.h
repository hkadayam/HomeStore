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
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <homestore/base/blk.h>               // BlkId, BlkAllocStatus, blk_count_t, blk_alloc_hints
#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include "blob/stream_base.h"              // StreamBase, CPSessionBase, cp_id_t, CPManager::max_concurent_cps

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
    std::vector< sisl::ByteArray > bufs; // pending buffers in write order (shared ownership)

    explicit WriteUnit(BlkId bid) : alloc_blkid{bid} {}
    WriteUnit(const WriteUnit&) = delete;
    WriteUnit& operator=(const WriteUnit&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// CPSession
//
// Holds all append state accumulated within one CP epoch (indexed by cp_id%2).
// Switched over atomically by on_cp_switchover(); flushed by flush(cp).
// ─────────────────────────────────────────────────────────────────────────────
struct CPSession : public StreamBase::FlushSessionBase {
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
// Two append paths:
//   quick_append() — synchronous, takes mu_, tries the active WriteUnit.
//   append()       — async (coro::Task), flushes filled WriteUnits to disk,
//                    allocates a new WriteUnit if needed, then appends.
//
// Persistence:
//   Per chunk: one MetaBlk storing the full allocator bitmap.
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

    static folly::coro::Task< shared< AppendBlkStream > > create(uint64_t stream_id, MetaClient& meta_client,
                                                                 const std::string& dev_name,
                                                                 const shared< VirtualDev >& vdev, uint64_t chunk_size,
                                                                 uint32_t blk_size = 0);

    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, sisl::ByteView > >;
    static folly::coro::Task< shared< AppendBlkStream > > load(uint64_t stream_id, MetaClient& meta_client,
                                                               const std::string& dev_name,
                                                               const shared< VirtualDev >& vdev, uint32_t blk_size,
                                                               ChunkMblkMap&& mblks);

    AppendBlkStream(const AppendBlkStream&) = delete;
    AppendBlkStream& operator=(const AppendBlkStream&) = delete;
    AppendBlkStream(AppendBlkStream&&) = delete;
    AppendBlkStream& operator=(AppendBlkStream&&) = delete;
    ~AppendBlkStream() override = default;

    // ── IO ─────────────────────────────────────────────────────────────

    /// Synchronous fast-path: locks mu_, tries the active WriteUnit for the segment.  On success, moves buf out and
    /// returns the BlkId.  On failure (no active unit or unit full), returns std::nullopt and leaves buf untouched —
    /// caller should fall back to append().
    std::optional< BlkId > quick_append(CP* cp, uint16_t segment_id, sisl::ByteArray& buf);

    /// Async append: allocates a new WriteUnit (possibly expanding the stream), installs it, and appends buf.
    /// No disk I/O — caller must call flush() separately to write filled WriteUnits to disk.
    folly::coro::Task< BlkId > append(CP* cp, uint16_t segment_id, sisl::ByteArray&& buf);

    /// Grab all filled WriteUnits and write them to disk (writev + commit + free excess).
    /// Caller can fire on an executor and collectAll later to overlap I/O with CPU work.
    folly::coro::Task< void > flush(CP* cp);

    /// Invalidate (free) a previously-appended block.  Marks owning chunk dirty.
    void invalidate(CP* cp, const BlkId& bid);

    folly::coro::Task< std::error_code > read(sisl::IOBuffer& buf, const BlkId& bid);

    // ── CP hooks ──────────────────────────────────────────────────────────────

    /// Initialize the new CP session for accumulating appends.
    void on_cp_switchover(CP* cur_cp, CP* new_cp);

    /// Finalize all remaining WriteUnits for this CP, issue writev, write dirty bitmaps.
    folly::coro::Task< bool > cp_flush(CP* cp);

    /// Returns true if any chunks were dirtied during the given CP epoch.
    bool is_dirty(cp_id_t cp_id) { return cp_session(cp_id).has_dirty_chunks(); }

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "appendblk"; }

private:
    AppendBlkStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev,
                    uint64_t chunk_size, uint32_t blk_size = 0, ChunkMblkMap&& mblks = {});

    /// Core append logic (called under mu_).  If new_wu is provided, installs it into the session first.  Then tries
    /// the active WriteUnit for the segment — on success moves buf and returns BlkId, else returns nullopt.
    std::optional< BlkId > do_quick_append(CPSession& session, uint16_t segment_id, unique< WriteUnit > new_wu,
                                           sisl::ByteArray& buf);

    /// Swap out all WriteUnits from the session (under mu_).  Clears active and all_units.
    std::vector< unique< WriteUnit > > grab_write_units(CPSession& session);

    /// Flush a batch of WriteUnits to disk: writev used portions, commit used blocks, free excess.
    folly::coro::Task< void > flush_write_units(const std::vector< unique< WriteUnit > >& units);

    /// Try to alloc nblks; expand by one chunk and retry once on failure.
    folly::coro::Task< BlkId > alloc_or_expand(blk_count_t nblks, const blk_alloc_hints& hints);

    CPSession& cp_session(cp_id_t cp_id) { return cp_session_[cp_id % CPManager::max_concurent_cps]; }

private:
    std::mutex mu_;                // protects do_append / grab_write_units (session mutation)
    folly::coro::Mutex flush_mu_; // serialises async path (flush + alloc + append) and cp_flush
    CPSession cp_session_[CPManager::max_concurent_cps];

    // WriteUnit pre-alloc sizes (tunable; min guarantees at least one write).
    static constexpr blk_count_t kMaxWriteUnitBlks = 256;
    static constexpr blk_count_t kMinWriteUnitBlks = 1;
};

} // namespace homestore
