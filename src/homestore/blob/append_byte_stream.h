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

#include <folly/coro/Task.h>

#include <homestore/blk.h>              // BlkId, blk_count_t, chunk_num_t
#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include <sisl/fds/buffer.h>            // sisl::Blob, sisl::BufBuilder

#include "blob/stream_base.h"            // StreamBase, FlushSessionBase, sisl::Rcu
#include "iomanager/drive_interface.hpp" // IOBuffer

namespace homestore {

class Chunk;
class VirtualDev;

// ─────────────────────────────────────────────────────────────────────────────
// AppendByteChunkMeta
//
// On-disk payload for one chunk's MetaBlk.  Stores only how many bytes have been written into this chunk so far.
// Recovery sums bytes_written across all chunks (ordered by vdev_order) to reconstruct tail_offset.
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(8) AppendByteChunkMeta {
    uint64_t bytes_written{0};
};
static_assert(std::is_trivially_copyable_v< AppendByteChunkMeta >);
static_assert(std::is_standard_layout_v< AppendByteChunkMeta >);

// ─────────────────────────────────────────────────────────────────────────────
// AppendByteStream : StreamBase
//
// Byte-stream append.  Callers never see block addresses; the stream tracks a logical tail_offset in bytes.
//
// append(data) — synchronous.  Memcpys caller bytes into a growable in-memory buffer.  If concurrent_safe is true
// (default), a mutex serialises appends; otherwise the caller must guarantee single-threaded access.
//
// flush() — async.  Swaps the buffer, expands chunks as needed, writes to disk, updates chunk MetaBlks.
//
// Chunk MetaBlk payload: AppendByteChunkMeta (bytes_written per chunk).
// Recovery: sum bytes_written across all chunks (ordered by vdev_order) to reconstruct tail_offset.
//
// Truncate: reset tail_offset to 0; existing chunks remain allocated for reuse.
// ─────────────────────────────────────────────────────────────────────────────
class AppendByteStream : public StreamBase {
public:
    // ── FlushBuffer ─────────────────────────────────────────────────────────
    // Accumulates appended bytes between flushes using LargeBufBuilder (chain of aligned IOBuffers).
    struct FlushBuffer {
        sisl::LargeBufBuilder builder;
        uint64_t start_offset{0}; // stream byte offset at which the builder's first byte corresponds

        void reset() {
            builder.clear();
            start_offset = 0;
        }
    };

    // ── TailBlock ────────────────────────────────────────────────────────────
    // Cached copy of the last partial (non-block-aligned) block, carried across flush boundaries and restarts.
    // At flush time, if the buffer tail is not block-aligned, the final partial block is saved here.  At restart,
    // if per-chunk bytes_written is not block-aligned, the tail block is read from disk into this cache.  The next
    // append seeds the new FlushBuffer from this cache so the stream remains byte-contiguous.
    struct TailBlock {
        IOBuffer buf;               // one block of data (block-aligned allocation, blk_size bytes)
        size_t chunk_idx{};         // which chunk (by index); resolved to chunk_id at flush
        uint32_t offset_in_chunk{}; // block-aligned byte offset within the chunk
        uint32_t valid_bytes{};     // how many bytes in buf are real data (< blk_size)
    };

    // ── ReadCursor ───────────────────────────────────────────────────────────
    class ReadCursor {
    public:
        folly::coro::Task< std::pair< IOBuffer, uint32_t > > next(size_t max_bytes);

        bool has_more() const { return pos_ < end_; }
        uint64_t position() const { return pos_; }
        uint64_t remaining() const { return end_ - pos_; }

    private:
        friend class AppendByteStream;
        ReadCursor(AppendByteStream& stream, uint64_t start, uint64_t end);
        AppendByteStream& stream_;
        uint64_t pos_;
        uint64_t end_;
    };

    // ── Factories ─────────────────────────────────────────────────────────────

    static folly::coro::Task< shared< AppendByteStream > > create(uint64_t stream_id, MetaClient& meta_client,
                                                                  const std::string& dev_name,
                                                                  const shared< VirtualDev >& vdev,
                                                                  uint64_t chunk_size,
                                                                  bool concurrent_safe = true);

    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, sisl::ByteView > >;
    static folly::coro::Task< shared< AppendByteStream > > load(uint64_t stream_id, MetaClient& meta_client,
                                                                const std::string& dev_name,
                                                                const shared< VirtualDev >& vdev, ChunkMblkMap&& mblks,
                                                                bool concurrent_safe = true);

    AppendByteStream(const AppendByteStream&) = delete;
    AppendByteStream& operator=(const AppendByteStream&) = delete;
    AppendByteStream(AppendByteStream&&) = delete;
    AppendByteStream& operator=(AppendByteStream&&) = delete;
    ~AppendByteStream() override = default;

    // ── IO ───────────────────────────────────────────────────────────────────

    /// Synchronous append.  Copies data into the in-memory buffer.  No disk I/O — flush() writes to disk.
    /// If concurrent_safe_ is true, a mutex serialises appends; otherwise caller must be single-threaded.
    uint64_t append(const sisl::Blob& data);

    /// Read len bytes starting at byte_offset.
    folly::coro::Task< std::pair< std::error_code, IOBuffer > > read(uint64_t byte_offset, size_t len);

    ReadCursor open_cursor(uint64_t start_offset = 0) const;
    ReadCursor open_cursor(uint64_t start_offset, uint64_t end_offset) const;

    folly::coro::Task< void > truncate(bool release_chunks = false);

    // ── Flush ─────────────────────────────────────────────────────────────────

    /// Swap the buffer, expand chunks as needed, write to disk, update MetaBlks.
    folly::coro::Task< bool > flush();

    // ── Accessors ─────────────────────────────────────────────────────────────
    uint64_t tail_offset() const { return tail_offset_; }
    void set_concurrent_safe(bool v) { concurrent_safe_ = v; }

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "appendbyte"; }

private:
    AppendByteStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                     const shared< VirtualDev >& vdev, uint64_t chunk_size, bool concurrent_safe,
                     ChunkMblkMap&& mblks = {});

    /// Core append logic (no locking).  Seeds from tail_block_ on first call, then memcpys into buf.
    uint64_t do_append(const sisl::Blob& data);

    /// Internal read helper.
    folly::coro::Task< std::pair< std::error_code, IOBuffer > > read_blocks(chunk_num_t cid, uint32_t blk_num,
                                                                            blk_count_t nblks);

private:
    uint64_t tail_offset_{0};
    bool concurrent_safe_;
    std::mutex append_mutex_;  // used only when concurrent_safe_ is true; protects tail_offset_, flush_buf_, tail_block_

    FlushBuffer flush_buf_;

    // Cached partial tail block — bridges flush boundaries and restarts.
    std::optional< TailBlock > tail_block_;
};

} // namespace homestore
