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

#include <algorithm>
#include "common/async.h"
#include <cstring>
#include <stdexcept>

#include <fmt/format.h>

#include "homestore/blob/append_byte_stream.h"
#include "homestore/base/crash_simulator.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/device/chunk.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/managers.h"
#include "homestore/meta/meta_client.h"

namespace homestore {

// Byte-stream flush tracing on the blob_dev module (mirrors VDEV_LOG / LSTREAM_LOG), keyed by stream id so a
// stream's flush phases group under `--log_mods blob_dev:trace`.
#define BLOB_STREAM_LOG(level, ...) HS_SUBMOD_LOG(level, blob_dev, , "stream", stream_id(), ##__VA_ARGS__)

using sisl::IoBufOwn;

// ─────────────────────────────────────────────────────────────────────────────
// Private constructor
// ─────────────────────────────────────────────────────────────────────────────

AppendByteStream::AppendByteStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                                   const shared< VirtualDev >& vdev, uint64_t chunk_size, bool concurrent_safe) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, 0, {} /* no per-chunk mblks */},
        concurrent_safe_{concurrent_safe} {
}

std::string AppendByteStream::sb_mblk_name(const std::string& dev, uint64_t stream_id) {
    return fmt::format("{}_appendbyte_sb_{}", dev, stream_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// create / load
// ─────────────────────────────────────────────────────────────────────────────

Async< shared< AppendByteStream > > AppendByteStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                             const std::string& dev_name,
                                                             const shared< VirtualDev >& vdev, uint64_t chunk_size,
                                                             bool concurrent_safe) {
    auto stream = shared< AppendByteStream >{
        new AppendByteStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size, concurrent_safe}};

    // Allocate the single per-stream sb MetaBlk.  Its payload is written with the initial empty state below.
    stream->sb_mblk_ = co_await meta_client.create_meta_blk(sb_mblk_name(dev_name, stream_id), std::nullopt);
    co_await stream->persist_stream_sb();

    // Ensure at least one chunk exists so the stream has somewhere to write on first append.
    co_await stream->expand_to(0);
    co_return stream;
}

Async< shared< AppendByteStream > > AppendByteStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                           const std::string& dev_name,
                                                           const shared< VirtualDev >& vdev, MetaBlk&& sb,
                                                           sisl::IoBufView sb_payload, bool concurrent_safe) {
    // Parse {chunk_size, head, tail, chunk_ids} from the stream sb payload.  The sb is always written by create()
    // and persist_stream_sb(), so any stream that was created should have a valid payload here.
    if (sb_payload.size() < sizeof(AppendByteStreamSb)) {
        throw std::runtime_error(
            fmt::format("AppendByteStream::load: sb payload too small for stream {} on {}", stream_id, dev_name));
    }
    const auto* s = r_cast< const AppendByteStreamSb* >(sb_payload.bytes());
    if (s->chunk_size == 0) {
        throw std::runtime_error(
            fmt::format("AppendByteStream::load: sb has chunk_size=0 for stream {} on {}", stream_id, dev_name));
    }
    const uint64_t chunk_sz = s->chunk_size;
    const uint64_t recovered_head = s->head_offset;
    const uint64_t recovered_tail = s->tail_offset;
    std::vector< uint32_t > chunk_ids;
    chunk_ids.reserve(s->n_chunks);
    for (uint32_t i = 0; i < s->n_chunks; ++i) {
        chunk_ids.push_back(s->chunk_ids()[i]);
    }

    auto stream = shared< AppendByteStream >{
        new AppendByteStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz, concurrent_safe}};
    stream->sb_mblk_ = std::move(sb);
    stream->head_offset_ = recovered_head;
    stream->offset_in_first_chunk_ = recovered_head % chunk_sz;
    stream->init_crc_ = s->init_crc;

    // Look up chunks from the vdev by their ids and install into the stream.  StreamBase::install_chunks sorts
    // them by vdev_order before publishing.
    std::vector< shared< Chunk > > chunks;
    chunks.reserve(chunk_ids.size());
    for (auto cid : chunk_ids) {
        if (auto c = vdev->get_chunk(cid)) {
            chunks.push_back(std::move(c));
        }
    }
    stream->install_chunks(std::move(chunks));

    co_await stream->resume_writes_at(recovered_tail);

    co_return stream;
}

Async< void > AppendByteStream::destroy() {
    // Remove the per-stream sb MetaBlk first, so a recovery scan can never load this stream once its chunks are gone.
    // Then invalidate our handle before freeing the chunks: StreamBase::destroy() drives remove_chunk_mblk(), which for
    // this stream type re-persists the sb — but the sb is gone now, so persist_stream_sb() must be a no-op.  Re-writing
    // it would re-append the just-removed block to the client's chain and form a cycle.
    co_await meta_client_.remove_meta_blk(sb_mblk_);
    sb_mblk_ = MetaBlk{};
    co_await StreamBase::destroy();
}

Async< void > AppendByteStream::resume_writes_at(uint64_t tail) {
    tail_offset_ = tail;
    const uint32_t blk_sz = block_size();
    if (tail == 0 || (tail % blk_sz) == 0) {
        co_return; // block-aligned (or empty) — no partial bytes to carry
    }
    const uint32_t partial = tail % blk_sz;
    auto [cid, off_in_chunk] = resolve(tail - partial);
    auto [ec, rbuf] = co_await read_blocks(cid, to_u32(off_in_chunk / blk_sz), 1);
    if (ec) {
        throw std::system_error(ec, "AppendByteStream::resume_writes_at: failed to read partial-tail block");
    }
    flush_buf_.start_offset = tail - partial;
    flush_buf_.builder.append(sisl::Blob{rbuf.bytes(), partial});
}

// ─────────────────────────────────────────────────────────────────────────────
// append
// ─────────────────────────────────────────────────────────────────────────────

uint64_t AppendByteStream::do_append(const sisl::Blob& data) {
    // Any partial-tail-block bytes from the prior flush (or load) are already sitting in flush_buf_ with its
    // start_offset set to the block-aligned position.  If flush_buf_ is empty, we're starting fresh at tail_offset_.
    if (flush_buf_.builder.empty()) {
        flush_buf_.start_offset = tail_offset_;
    }
    flush_buf_.builder.append(data);
    auto const offset = tail_offset_;
    tail_offset_ += data.size();
    return offset;
}

uint64_t AppendByteStream::append(const sisl::Blob& data) {
    if (concurrent_safe_) {
        std::lock_guard lg{append_mutex_};
        return do_append(data);
    }
    return do_append(data);
}

// ─────────────────────────────────────────────────────────────────────────────
// truncate
// ─────────────────────────────────────────────────────────────────────────────

Async< void > AppendByteStream::truncate(uint64_t upto_offset) {
    upto_offset = std::min(upto_offset, tail_offset_);
    if (upto_offset <= head_offset_)
        co_return;

    // Release chunks fully before upto_offset.  Each chunk spans chunk_size() bytes; releasable count is the number
    // of chunks whose end <= upto_offset, i.e. (upto_offset - head_offset_ + offset_in_first_chunk_) / chunk_size().
    const size_t n_release = nth_chunk(upto_offset);
    if (n_release > 0) {
        co_await truncate_before(n_release);
    }

    head_offset_ = upto_offset;
    offset_in_first_chunk_ = head_offset_ % chunk_size();

    // Fresh-start mode: stream is logically empty (head == tail).  Release any remaining chunk (the one holding the
    // partial tail) and reset positions to 0 so the next append starts a brand-new stream.  Both LogStream (no
    // records left) and cow_btree (reusable stream) rely on this.
    if (head_offset_ == tail_offset_) {
        // Stream is logically empty — release all chunks except the last one (kept as an anchor), reset positions
        // to 0.  Keeping one chunk means the stream is immediately usable for new appends without a chunk alloc and
        // guarantees non-zero chunk count for users that rely on it.  The stream's sb MetaBlk survives regardless.
        const size_t n = num_chunks();
        if (n > 1) {
            co_await truncate_before(n - 1);
        }
        head_offset_ = 0;
        tail_offset_ = 0;
        offset_in_first_chunk_ = 0;
        flush_buf_.reset();
    }

    // Crash point: chunks are physically released but the stream sb still claims the old positions —
    // recovery must tolerate a persisted head that points into freed chunks.
    if (crash_if_flip_fired("crash_after_stream_chunk_release")) {
        co_return;
    }
    co_await persist_stream_sb();
}

// ─────────────────────────────────────────────────────────────────────────────
// read
// ─────────────────────────────────────────────────────────────────────────────

Async< std::pair< std::error_code, sisl::IoBufView > > AppendByteStream::read(uint64_t byte_offset, size_t len) {
    if (byte_offset < head_offset_ || byte_offset + len > tail_offset_) {
        co_return {std::make_error_code(std::errc::invalid_argument), sisl::IoBufView{}};
    }
    auto [cid, offset_in_chunk] = resolve(byte_offset);
    const uint32_t blk_sz = block_size();
    const uint32_t blk_num = to_u32(offset_in_chunk / blk_sz);
    const blk_count_t nblks = s_cast< blk_count_t >((offset_in_chunk + len + blk_sz - 1) / blk_sz - to_u64(blk_num));

    auto [ec, buf] = co_await read_blocks(cid, blk_num, nblks);
    if (ec) {
        co_return {ec, sisl::IoBufView{}};
    }
    // Slice the block-aligned buf so bytes() lands exactly at byte_offset and size() is len.  Zero-copy: the
    // IoBufView holds a shared_ptr to the underlying IoBufOwn via make_io_buf_shared.
    co_return {std::error_code{},
               sisl::IoBufView{sisl::make_io_buf_shared(std::move(buf)), to_u32(byte_offset % blk_sz), to_u32(len)}};
}

Async< std::pair< std::error_code, IoBufOwn > > AppendByteStream::read_blocks(chunk_num_t cid, uint32_t blk_num,
                                                                              blk_count_t nblks) {
    IoBufOwn buf{to_u32(nblks) * block_size(), block_size()};
    const BlkId bid{blk_num, nblks, cid};
    auto ec = co_await vdev().read(buf, bid);
    co_return {ec, std::move(buf)};
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadCursor
// ─────────────────────────────────────────────────────────────────────────────

AppendByteStream::ReadCursor::ReadCursor(AppendByteStream& stream, uint64_t start, uint64_t end) :
        stream_{stream}, pos_{start}, end_{end} {
}

AppendByteStream::ReadCursor AppendByteStream::open_cursor(uint64_t start_offset) const {
    return ReadCursor{const_cast< AppendByteStream& >(*this), start_offset, tail_offset()};
}

AppendByteStream::ReadCursor AppendByteStream::open_cursor(uint64_t start_offset, uint64_t end_offset) const {
    return ReadCursor{const_cast< AppendByteStream& >(*this), start_offset, end_offset};
}

Async< std::pair< sisl::IoBufView, uint32_t > > AppendByteStream::ReadCursor::next(size_t max_bytes) {
    if (pos_ >= end_) {
        co_return {sisl::IoBufView{}, 0};
    }

    auto [cid, offset_in_chunk] = stream_.resolve(pos_);
    const uint32_t blk_sz = stream_.block_size();
    const uint64_t remaining_in_chunk = stream_.chunk_size() - offset_in_chunk;
    const uint64_t read_len = std::min({to_u64(max_bytes), end_ - pos_, remaining_in_chunk});

    const uint32_t blk_num = to_u32(offset_in_chunk / blk_sz);
    const blk_count_t nblks =
        s_cast< blk_count_t >((offset_in_chunk + read_len + blk_sz - 1) / blk_sz - to_u64(blk_num));
    const uint32_t valid = to_u32(read_len);

    auto [ec, buf] = co_await stream_.read_blocks(cid, blk_num, nblks);
    if (ec) {
        co_return {sisl::IoBufView{}, 0};
    }

    // Slice so the returned IoBufView's bytes() lands at pos_ — same logic as AppendByteStream::read().  Zero-copy.
    const uint32_t in_buf = to_u32(offset_in_chunk % blk_sz);
    pos_ += valid;
    co_return {sisl::IoBufView{sisl::make_io_buf_shared(std::move(buf)), in_buf, valid}, valid};
}

// ─────────────────────────────────────────────────────────────────────────────
// Flush
// ─────────────────────────────────────────────────────────────────────────────

Async< bool > AppendByteStream::flush() {
    FlushBuffer old_buf;
    std::vector< sisl::IoBufOwn > bufs;
    uint64_t total = 0;

    // Under lock: swap the buffer, drain it, and if the last buf has a partial (non-block-aligned) tail, inject
    // those bytes into the NEW flush_buf_ so the next flush carries them forward, then zero-pad the last buf in
    // place so every buf in the `bufs` list is block-aligned from here on.
    auto do_swap_and_carry = [&]() {
        std::swap(old_buf, flush_buf_);
        total = old_buf.builder.total_bytes();
        bufs = old_buf.builder.move_all_bufs();

        if (bufs.empty()) {
            return;
        }

        auto& last_buf = bufs.back();
        const uint32_t partial = last_buf.size() % block_size();
        if (partial == 0) {
            return;
        }

        // Seed the next flush_buf_ with the partial tail bytes.  start_offset is the block-aligned stream
        // position where they land; next append/emplace continues from there.
        flush_buf_.start_offset = old_buf.start_offset + total - partial;
        flush_buf_.builder.append(sisl::Blob{last_buf.bytes() + last_buf.size() - partial, partial});

        // Zero-pad the last buf up to the block boundary so this flush writes a full block.
        const uint32_t pad = block_size() - partial;
        std::memset(last_buf.bytes() + last_buf.size(), 0, pad);
        last_buf.set_size(last_buf.size() + pad);
    };

    if (concurrent_safe_) {
        std::lock_guard lg{append_mutex_};
        do_swap_and_carry();
    } else {
        do_swap_and_carry();
    }

    if (bufs.empty()) {
        BLOB_STREAM_LOG(TRACE, "flush: nothing to write (empty after swap)");
        co_return true;
    }

    // After the tail-carry step, total rounded up to block boundary is exactly what we're writing.
    uint64_t stream_offset = old_buf.start_offset;
    const uint64_t total_to_write = sisl::round_up(total, block_size());
    BLOB_STREAM_LOG(TRACE, "flush: {} buf(s) total={} stream_off={}; expand_to(chunk={})", bufs.size(), total,
                    stream_offset, nth_chunk(stream_offset + total_to_write - 1));
    co_await expand_to(nth_chunk(stream_offset + total_to_write - 1));
    BLOB_STREAM_LOG(TRACE, "flush: expand_to done, starting writes");

    // Every buf is block-aligned now.  Walk them, writing directly; only split on chunk-straddle (one wbuf copy
    // per straddling buf).
    auto [chunk_id, offset_in_chunk] = resolve(stream_offset);
    uint64_t remain_in_chunk = chunk_size() - offset_in_chunk;
    blk_num_t cur_blk_num = offset_in_chunk / block_size();

    for (auto& buf : bufs) {
        uint32_t buf_offset = 0;
        while (buf_offset < buf.size()) {
            if (remain_in_chunk == 0) {
                chunk_id = lookup_chunk_id(nth_chunk(stream_offset));
                cur_blk_num = 0;
                remain_in_chunk = chunk_size();
            }

            const uint32_t write_len = to_u32(std::min(to_u64(buf.size() - buf_offset), remain_in_chunk));
            const uint16_t nblks = to_u16(write_len / block_size());

            if ((buf_offset == 0) && (write_len == buf.size())) {
                // Whole buf fits in this chunk — write it directly.
                co_await vdev().write(buf, BlkId{cur_blk_num, nblks, chunk_id});
            } else {
                // Chunk-straddle: copy this chunk's slice into a fresh aligned buffer.  All quantities are
                // block-aligned so no padding is involved.
                IoBufOwn wbuf{write_len, block_size()};
                std::memcpy(wbuf.bytes(), buf.bytes() + buf_offset, write_len);
                co_await vdev().write(wbuf, BlkId{cur_blk_num, nblks, chunk_id});
            }

            buf_offset += write_len;
            stream_offset += write_len;
            remain_in_chunk -= write_len;
            cur_blk_num += nblks;
        }
    }

    BLOB_STREAM_LOG(TRACE, "flush: writes done, persisting metadata");
    co_await persist_flush_metadata();
    BLOB_STREAM_LOG(TRACE, "flush: complete");
    co_return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Metadata persistence
// ─────────────────────────────────────────────────────────────────────────────

Async< void > AppendByteStream::persist_flush_metadata() {
    co_await persist_stream_sb();
}

Async< void > AppendByteStream::persist_stream_sb() {
    // If the sb MetaBlk has already been removed (the stream is being destroyed), there is nothing to persist — and a
    // write here would re-append the removed block to the client's chain.
    if (!sb_mblk_.valid()) {
        co_return;
    }

    // Snapshot the current chunk list (chunk_ids).
    std::vector< uint32_t > cids;
    {
        auto acc = chunks();
        cids.reserve(acc->size());
        for (auto const& c : *acc) {
            cids.push_back(c->chunk_id());
        }
    }

    const size_t payload_bytes = AppendByteStreamSb::size_for(to_u32(cids.size()));
    auto buf = sisl::make_io_buf_shared(to_u32(payload_bytes));
    auto* sb = reinterpret_cast< AppendByteStreamSb* >(buf->bytes());
    sb->chunk_size = chunk_size();
    sb->head_offset = head_offset_;
    sb->tail_offset = tail_offset_;
    sb->n_chunks = to_u32(cids.size());
    sb->init_crc = init_crc_;
    // Guard against memcpy(dst, nullptr, 0): an empty stream has no chunk_ids and cids.data() is null, which is UB
    // (memcpy's src is declared nonnull) even for a zero byte count.
    if (!cids.empty()) {
        std::memcpy(sb->chunk_ids(), cids.data(), cids.size() * sizeof(uint32_t));
    }

    auto lock = co_await mblk_mutex_.co_scoped_lock();
    co_await meta_client_.write_meta_blk(sb_mblk_, buf);
}

Async< void > AppendByteStream::init_chunk_mblk(const shared< Chunk >& /*chunk*/) {
    // No per-chunk MetaBlk for appendbyte streams; the chunk list lives in the stream sb.  Update it now that the
    // chunk list grew.
    co_await persist_stream_sb();
}

Async< void > AppendByteStream::remove_chunk_mblk(uint32_t /*chunk_id*/) {
    // No per-chunk MetaBlk to remove; just refresh the stream sb with the new (shrunken) chunk list.
    co_await persist_stream_sb();
}

chunk_num_t AppendByteStream::lookup_chunk_id(size_t nth_chunk) {
    auto acc = chunks();
    return s_cast< chunk_num_t >((*acc)[nth_chunk]->chunk_id());
}

std::pair< chunk_num_t, uint64_t > AppendByteStream::resolve(uint64_t byte_offset) {
    // Distance from chunk 0's start = (byte_offset - head_offset_) + offset_in_first_chunk_.
    const uint64_t rel = byte_offset - head_offset_ + offset_in_first_chunk_;
    auto acc = chunks();
    return {s_cast< chunk_num_t >((*acc)[to_size(rel / chunk_size())]->chunk_id()), rel % chunk_size()};
}

size_t AppendByteStream::nth_chunk(uint64_t byte_offset) {
    // Distance from chunk 0's start = (byte_offset - head_offset_) + offset_in_first_chunk_.
    const uint64_t rel = byte_offset - head_offset_ + offset_in_first_chunk_;
    return to_size(rel / chunk_size());
}

} // namespace homestore
