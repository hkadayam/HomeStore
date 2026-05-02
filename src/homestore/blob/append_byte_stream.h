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

#include <homestore/blk.h>                 // BlkId, blk_count_t, chunk_num_t
#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include <sisl/fds/buffer.h>               // sisl::Blob, sisl::BufBuilder

#include "blob/stream_base.h" // StreamBase, FlushSessionBase, sisl::Rcu

namespace homestore {

class Chunk;
class VirtualDev;

// ─────────────────────────────────────────────────────────────────────────────
// AppendByteStreamSb — on-disk payload of AppendByteStream's single per-stream MetaBlk
//
// One MetaBlk per stream (name "<dev>_appendbyte_sb_<stream_id>"), NOT per-chunk.  Carries the stream's full
// persistent state: head/tail offsets + the list of chunk_ids owned by the stream.  Chunks don't carry individual
// MetaBlks (init_chunk_mblk / remove_chunk_mblk are overridden to no-ops); chunk-stream membership is recovered
// from this list on reload.  Truncate-to-zero can release every chunk cleanly — this mblk survives.
//
// Layout: { head_offset, tail_offset, n_chunks, chunk_id[n_chunks] }.  Total size grows with chunk count.
// ─────────────────────────────────────────────────────────────────────────────
struct AppendByteStreamSb {
    uint64_t chunk_size{0}; // stream's chunk_size (the size used for vdev->expand calls)
    uint64_t head_offset{0};
    uint64_t tail_offset{0};
    uint32_t n_chunks{0};
    // chain_seed is used by LogStream to root its CRC chain at a per-stream-epoch random value, so stale on-disk
    // groups from a recycled chunk (or from before a truncate-all) fail prev_crc validation against the current
    // seed and recovery stops cleanly at the first stale group.  Other AppendByteStream subclasses ignore it.
    uint32_t chain_seed{0};
    // followed by uint32_t chunk_ids[n_chunks]

    uint32_t* chunk_ids() { return reinterpret_cast< uint32_t* >(this + 1); }
    const uint32_t* chunk_ids() const { return reinterpret_cast< const uint32_t* >(this + 1); }
    static size_t size_for(uint32_t n) { return sizeof(AppendByteStreamSb) + n * sizeof(uint32_t); }
};

// ─────────────────────────────────────────────────────────────────────────────
// AppendByteStream : StreamBase
//
// Byte-stream append.  Callers never see block addresses; the stream tracks a logical tail_offset in bytes.
//
// append(data) — synchronous.  Memcpys caller bytes into a growable in-memory buffer.  If concurrent_safe is true
// (default), a mutex serialises appends; otherwise the caller must guarantee single-threaded access.
//
// flush() — async.  Swaps the buffer, expands chunks as needed, writes to disk, persists the stream sb.
//
// Persistence: ONE MetaBlk per stream carrying {head, tail, chunk_ids[]}.  Chunks themselves don't get per-chunk
// MetaBlks — init_chunk_mblk / remove_chunk_mblk are overridden to no-ops, and chunk membership is stored in the
// single stream sb.  This lets truncate-to-zero release every chunk without losing the stream's identity.
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

    // ── ReadCursor ───────────────────────────────────────────────────────────
    class ReadCursor {
    public:
        folly::coro::Task< std::pair< sisl::IOBuffer, uint32_t > > next(size_t max_bytes);

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
                                                                  const shared< VirtualDev >& vdev, uint64_t chunk_size,
                                                                  bool concurrent_safe = true);

    /// Load from a previously persisted stream sb MetaBlk and its decoded payload.
    static folly::coro::Task< shared< AppendByteStream > > load(uint64_t stream_id, MetaClient& meta_client,
                                                                const std::string& dev_name,
                                                                const shared< VirtualDev >& vdev, MetaBlk&& sb,
                                                                sisl::ByteView sb_payload,
                                                                bool concurrent_safe = true);

    /// Name of the per-stream sb MetaBlk: "<dev>_appendbyte_sb_<stream_id>".
    static std::string sb_mblk_name(const std::string& dev, uint64_t stream_id);

    AppendByteStream(const AppendByteStream&) = delete;
    AppendByteStream& operator=(const AppendByteStream&) = delete;
    AppendByteStream(AppendByteStream&&) = delete;
    AppendByteStream& operator=(AppendByteStream&&) = delete;
    ~AppendByteStream() override = default;

    // ── IO ───────────────────────────────────────────────────────────────────

    /// Synchronous append.  Copies data into the in-memory buffer.  No disk I/O — flush() writes to disk.
    /// If concurrent_safe_ is true, a mutex serialises appends; otherwise caller must be single-threaded.
    uint64_t append(const sisl::Blob& data);

    /// Reserve `size` contiguous bytes in the in-memory buffer and invoke fill(sisl::Blob) to populate them in place
    /// — zero memcpy from any caller-provided source.  Returns the stream byte offset at which the bytes will land.
    /// Like append(), no disk I/O — flush() writes to disk.  If concurrent_safe_ is true the fill callback runs
    /// under append_mutex_ (so a concurrent flush cannot swap the buffer mid-fill); otherwise caller must be
    /// single-threaded.
    template < typename FillFn >
    uint64_t emplace(uint32_t size, FillFn&& fill) {
        if (concurrent_safe_) {
            std::lock_guard lg{append_mutex_};
            return do_emplace(size, std::forward< FillFn >(fill));
        }
        return do_emplace(size, std::forward< FillFn >(fill));
    }

    /// Read len bytes starting at byte_offset.
    folly::coro::Task< std::pair< std::error_code, sisl::IOBuffer > > read(uint64_t byte_offset, size_t len);

    ReadCursor open_cursor(uint64_t start_offset = 0) const;
    ReadCursor open_cursor(uint64_t start_offset, uint64_t end_offset) const;

    /// Advance the logical head to upto_offset (clamped to tail_offset).  Releases any chunks fully before
    /// upto_offset back to the vdev (which may pool them).  If the stream is logically empty after the advance
    /// (head == tail), all remaining chunks are also released and positions are reset to 0 — this is the
    /// "fresh-start" mode used by reusable streams (e.g. cow_btree's incr_map).
    folly::coro::Task< void > truncate(uint64_t upto_offset);

    // ── Flush ─────────────────────────────────────────────────────────────────

    /// Swap the buffer, expand chunks as needed, write to disk, update MetaBlks.
    folly::coro::Task< bool > flush();

    // ── Accessors ─────────────────────────────────────────────────────────────
    uint64_t tail_offset() const { return tail_offset_; }
    uint64_t head_offset() const { return head_offset_; }
    void set_concurrent_safe(bool v) { concurrent_safe_ = v; }

    // ── StreamBase hook ──────────────────────────────────────────────────────
    std::string_view stream_type_name() const override { return "appendbyte"; }

protected:
    /// Per-flush durability hook.  Default: persist the stream sb (head_offset, tail_offset, chunk_ids[]) into the
    /// stream's single sb MetaBlk.  Subclasses can override to skip per-flush persistence (e.g. LogStream deduces
    /// tail on recovery and persists only on truncate / chunk-list changes).
    virtual folly::coro::Task< void > persist_flush_metadata();

    /// Write the current {head, tail, chunk_ids[]} state into the stream sb MetaBlk.  Called from flush() (via
    /// persist_flush_metadata), from truncate(), and whenever chunk membership changes (chunk add/remove overrides).
    folly::coro::Task< void > persist_stream_sb();

    /// Overrides: AppendByteStream maintains a single per-stream MetaBlk carrying the chunk list, so chunks
    /// themselves don't get per-chunk MetaBlks.  Each override updates the stream sb after the chunk-list change.
    folly::coro::Task< void > init_chunk_mblk(const shared< Chunk >& chunk) override;
    folly::coro::Task< void > remove_chunk_mblk(uint32_t chunk_id) override;

    AppendByteStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                     const shared< VirtualDev >& vdev, uint64_t chunk_size, bool concurrent_safe);

private:

    /// Core append logic (no locking).  Memcpys into the flush buffer; if the buffer is empty, start_offset is set
    /// to the current tail.  Any partial-tail-block bytes carried over from the prior flush are already sitting in
    /// flush_buf_ (injected by the prior flush() or load()).
    uint64_t do_append(const sisl::Blob& data);

    /// Core emplace logic (no locking).  Reserves `size` contiguous bytes in the flush buffer and invokes fill.
    template < typename FillFn >
    uint64_t do_emplace(uint32_t size, FillFn&& fill) {
        if (flush_buf_.builder.empty()) {
            flush_buf_.start_offset = tail_offset_;
        }
        flush_buf_.builder.emplace(size, std::forward< FillFn >(fill));
        auto const offset = tail_offset_;
        tail_offset_ += size;
        return offset;
    }

    /// Internal read helper.
    folly::coro::Task< std::pair< std::error_code, sisl::IOBuffer > > read_blocks(chunk_num_t cid, uint32_t blk_num,
                                                                                  blk_count_t nblks);

    /// Resolve the chunk_id for the n-th chunk (by vdev_order) in this stream.  Takes a brief RCU read guard.
    chunk_num_t lookup_chunk_id(size_t nth_chunk);

    /// Return the chunk index (into the current chunks() vector) containing the given absolute stream offset.
    size_t nth_chunk(uint64_t byte_offset);

    /// Resolve an absolute stream byte offset to its chunk_id and offset within that chunk.  Caller must ensure
    /// byte_offset >= head_offset_ and the stream has a chunk covering that offset.  Takes a brief RCU read guard;
    /// do not call across a co_await.
    std::pair< chunk_num_t, uint64_t > resolve(uint64_t byte_offset);

protected:
    // ── Cursor / sb state ─────────────────────────────────────────────────────
    // Promoted to protected so subclasses (LogStream) can rewrite head/tail during recovery and restore the sb
    // MetaBlk handle in their own load() factory.  Direct manipulation is intentionally restricted to the parent
    // and its subclasses; callers go through truncate(), append(), flush(), etc.
    uint64_t tail_offset_{0};
    uint64_t head_offset_{0};           // head of the logical byte stream; advances on truncate
    uint64_t offset_in_first_chunk_{0}; // offset within the first chunk where head starts
    MetaBlk sb_mblk_;                   // the stream's single sb MetaBlk, created at stream construction (or recovered at load)
    uint32_t chain_seed_{0};            // see AppendByteStreamSb::chain_seed — written/read by persist_stream_sb / load

private:
    bool concurrent_safe_;
    std::mutex append_mutex_; // used only when concurrent_safe_ is true; protects tail_offset_, flush_buf_,
                              // head_offset_
    FlushBuffer flush_buf_;
};

} // namespace homestore
