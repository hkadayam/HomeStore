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
#include <cstring>
#include <stdexcept>

#include "blob/append_byte_stream.h"
#include "blob/blob_dev.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"
#include "managers.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// Private constructor
// ─────────────────────────────────────────────────────────────────────────────

AppendByteStream::AppendByteStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                                   const shared< VirtualDev >& vdev, uint64_t chunk_size, bool concurrent_safe,
                                   ChunkMblkMap&& mblks) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, 0, std::move(mblks)},
        concurrent_safe_{concurrent_safe} {}

// ─────────────────────────────────────────────────────────────────────────────
// create / load
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< AppendByteStream > > AppendByteStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                                         const std::string& dev_name,
                                                                         const shared< VirtualDev >& vdev,
                                                                         uint64_t chunk_size, bool concurrent_safe) {
    auto stream = shared< AppendByteStream >{
        new AppendByteStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size, concurrent_safe}};
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< AppendByteStream > > AppendByteStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                                       const std::string& dev_name,
                                                                       const shared< VirtualDev >& vdev,
                                                                       ChunkMblkMap&& mblks, bool concurrent_safe) {
    // Sum bytes_written from each recovered payload to reconstruct tail_offset.
    uint64_t recovered_tail = 0;
    for (auto& [cid, entry] : mblks) {
        if (entry.second.size() >= sizeof(AppendByteChunkMeta)) {
            const auto* meta = r_cast< const AppendByteChunkMeta* >(entry.second.bytes());
            recovered_tail += meta->bytes_written;
        }
    }

    const uint64_t chunk_sz = vdev->chunk_size_bytes();
    auto stream = shared< AppendByteStream >{new AppendByteStream{stream_id, meta_client, std::string{dev_name}, vdev,
                                                                   chunk_sz, concurrent_safe, std::move(mblks)}};
    stream->tail_offset_ = recovered_tail;

    // If the recovered tail is not block-aligned, read the partial tail block from disk so subsequent appends can
    // continue from the exact byte offset within that block.
    const uint32_t blk_sz = stream->block_size();
    if (recovered_tail > 0 && (recovered_tail % blk_sz) != 0) {
        const size_t tail_chunk_idx = to_size((recovered_tail - 1) / chunk_sz);
        const uint64_t tail_in_chunk = ((recovered_tail - 1) % chunk_sz) + 1;
        const uint32_t blk_offset = to_u32(((tail_in_chunk - 1) / blk_sz) * blk_sz);
        const uint32_t sub_blk_used = to_u32(tail_in_chunk - blk_offset);

        chunk_num_t cid{};
        {
            auto acc = stream->chunks();
            cid = s_cast< chunk_num_t >((*acc)[tail_chunk_idx]->chunk_id());
        }

        auto [ec, rbuf] = co_await stream->read_blocks(cid, blk_offset / blk_sz, 1);
        if (!ec) {
            stream->tail_block_ = TailBlock{std::move(rbuf), tail_chunk_idx, blk_offset, sub_blk_used};
        }
    }

    co_return stream;
}

// ─────────────────────────────────────────────────────────────────────────────
// append
// ─────────────────────────────────────────────────────────────────────────────

uint64_t AppendByteStream::do_append(const sisl::Blob& data) {
    // Seed from tail_block_ on first append of this flush buffer.
    if (flush_buf_.builder.empty() && tail_block_) {
        auto tb = std::move(*tail_block_);
        tail_block_.reset();
        flush_buf_.start_offset = tail_offset_ - tb.valid_bytes;
        flush_buf_.builder.append(sisl::Blob{tb.buf.bytes(), tb.valid_bytes});
    }

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

folly::coro::Task< void > AppendByteStream::truncate(bool release_chunks) {
    tail_offset_ = 0;
    tail_block_.reset();
    flush_buf_.reset();

    if (release_chunks) {
        {
            auto lock = co_await mblk_mutex_.co_scoped_lock();
            for (auto& [cid, mblk] : chunk_mblks_) {
                co_await meta_client_.remove_meta_blk(mblk);
            }
            chunk_mblks_.clear();
        }
        co_await destroy();
    } else {
        AppendByteChunkMeta meta{0};
        auto meta_buf = sisl::make_byte_array(sizeof(AppendByteChunkMeta));
        std::memcpy(meta_buf->bytes(), &meta, sizeof(meta));

        auto lock = co_await mblk_mutex_.co_scoped_lock();
        for (auto& [cid, mblk] : chunk_mblks_) {
            co_await meta_client_.write_meta_blk(mblk, meta_buf);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// read
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< std::pair< std::error_code, IOBuffer > > AppendByteStream::read(uint64_t byte_offset, size_t len) {
    const size_t chunk_idx = to_size(byte_offset / chunk_size());
    const uint64_t offset_in_chunk = byte_offset % chunk_size();
    const uint32_t blk_num = to_u32(offset_in_chunk / block_size());
    const blk_count_t nblks =
        s_cast< blk_count_t >((offset_in_chunk + len + block_size() - 1) / block_size() - to_u64(blk_num));

    chunk_num_t cid{};
    {
        auto acc = chunks();
        if (chunk_idx >= acc->size()) {
            co_return {std::make_error_code(std::errc::invalid_argument), IOBuffer{}};
        }
        cid = s_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
    }

    co_return co_await read_blocks(cid, blk_num, nblks);
}

folly::coro::Task< std::pair< std::error_code, IOBuffer > >
AppendByteStream::read_blocks(chunk_num_t cid, uint32_t blk_num, blk_count_t nblks) {
    IOBuffer buf{to_u32(nblks) * block_size(), block_size()};
    const BlkId bid{blk_num, nblks, cid};
    auto ec = co_await vdev().read(buf, bid);
    co_return {ec, std::move(buf)};
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadCursor
// ─────────────────────────────────────────────────────────────────────────────

AppendByteStream::ReadCursor::ReadCursor(AppendByteStream& stream, uint64_t start, uint64_t end) :
        stream_{stream}, pos_{start}, end_{end} {}

AppendByteStream::ReadCursor AppendByteStream::open_cursor(uint64_t start_offset) const {
    return ReadCursor{const_cast< AppendByteStream& >(*this), start_offset, tail_offset()};
}

AppendByteStream::ReadCursor AppendByteStream::open_cursor(uint64_t start_offset, uint64_t end_offset) const {
    return ReadCursor{const_cast< AppendByteStream& >(*this), start_offset, end_offset};
}

folly::coro::Task< std::pair< IOBuffer, uint32_t > > AppendByteStream::ReadCursor::next(size_t max_bytes) {
    if (pos_ >= end_) { co_return {IOBuffer{}, 0}; }

    const uint32_t blk_sz = stream_.block_size();
    const uint64_t csz = stream_.chunk_size();

    const uint64_t remaining = end_ - pos_;
    const uint64_t offset_in_chunk = pos_ % csz;
    const uint64_t remaining_in_chunk = csz - offset_in_chunk;
    const uint64_t read_len = std::min({to_u64(max_bytes), remaining, remaining_in_chunk});

    const size_t chunk_idx = to_size(pos_ / csz);
    const uint32_t blk_num = to_u32(offset_in_chunk / blk_sz);
    const blk_count_t nblks =
        s_cast< blk_count_t >((offset_in_chunk + read_len + blk_sz - 1) / blk_sz - to_u64(blk_num));
    const uint32_t valid = to_u32(read_len);

    chunk_num_t cid{};
    {
        auto acc = stream_.chunks();
        if (chunk_idx >= acc->size()) { co_return {IOBuffer{}, 0}; }
        cid = s_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
    }

    auto [ec, buf] = co_await stream_.read_blocks(cid, blk_num, nblks);
    if (ec) { co_return {IOBuffer{}, 0}; }

    pos_ += valid;
    co_return {std::move(buf), valid};
}

// ─────────────────────────────────────────────────────────────────────────────
// flush
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< bool > AppendByteStream::flush() {
    // Swap the buffer under mutex so no appender is mid-write.
    FlushBuffer old_buf;
    {
        std::lock_guard lg{append_mutex_};
        std::swap(old_buf, flush_buf_);
    }

    uint64_t const total = old_buf.builder.total_bytes();
    if (total == 0) { co_return true; }

    const uint32_t blk_sz = block_size();
    const uint64_t csz = chunk_size();
    uint64_t stream_offset = old_buf.start_offset;

    // Expand chunks to cover the full range of data.
    const size_t last_chunk_idx = to_size((stream_offset + total - 1) / csz);
    co_await expand_to(last_chunk_idx);

    // Track high-water mark per chunk for MetaBlk persistence.
    std::unordered_map< uint32_t, uint64_t > chunk_bytes_written;

    // Collect buffers from the builder, then write each to disk.  Each buffer may span a chunk boundary.
    std::vector< sisl::IoBlobSafe > bufs;
    old_buf.builder.consume([&bufs](sisl::IoBlobSafe&& buf) { bufs.push_back(std::move(buf)); });

    for (auto& buf : bufs) {
        uint32_t buf_offset = 0;
        uint32_t const buf_used = buf.size(); // set by consume() to the used byte count

        while (buf_offset < buf_used) {
            const size_t chunk_idx = to_size(stream_offset / csz);
            const uint64_t offset_in_chunk = stream_offset % csz;
            const uint64_t remaining_in_chunk = csz - offset_in_chunk;
            const uint32_t write_len = to_u32(std::min(to_u64(buf_used - buf_offset), remaining_in_chunk));

            chunk_num_t cid{};
            {
                auto acc = chunks();
                cid = s_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
            }

            const uint32_t blk_num = to_u32(offset_in_chunk / blk_sz);
            const blk_count_t nblks =
                s_cast< blk_count_t >((offset_in_chunk + write_len + blk_sz - 1) / blk_sz - to_u64(blk_num));
            const uint32_t aligned_sz = to_u32(nblks) * blk_sz;

            // Common case: entire buffer fits in one chunk — write directly (zero-pad tail to block alignment).
            if (buf_offset == 0 && write_len == buf_used) {
                if (aligned_sz > write_len) {
                    std::memset(buf.bytes() + write_len, 0, aligned_sz - write_len);
                }
                buf.set_size(aligned_sz);
                co_await vdev().write(buf, BlkId{blk_num, nblks, cid});
            } else {
                // Chunk boundary crossing — copy this chunk's portion into a separate aligned buffer.
                IOBuffer wbuf{aligned_sz, blk_sz};
                std::memcpy(wbuf.bytes(), buf.bytes() + buf_offset, write_len);
                if (aligned_sz > write_len) {
                    std::memset(wbuf.bytes() + write_len, 0, aligned_sz - write_len);
                }
                co_await vdev().write(wbuf, BlkId{blk_num, nblks, cid});
            }

            auto& hw = chunk_bytes_written[cid];
            hw = std::max(hw, offset_in_chunk + write_len);

            buf_offset += write_len;
            stream_offset += write_len;
        }
    }

    // Cache the partial tail block for the next flush.
    const uint64_t total_end = old_buf.start_offset + total;
    if ((total_end % blk_sz) != 0) {
        const size_t tail_chunk_idx = to_size((total_end - 1) / csz);
        const uint64_t tail_in_chunk = ((total_end - 1) % csz) + 1;
        const uint32_t blk_offset = to_u32(((tail_in_chunk - 1) / blk_sz) * blk_sz);
        const uint32_t sub_blk_used = to_u32(tail_in_chunk - blk_offset);

        // Re-read the partial block from disk (we just wrote it).
        chunk_num_t cid{};
        {
            auto acc = chunks();
            cid = s_cast< chunk_num_t >((*acc)[tail_chunk_idx]->chunk_id());
        }
        auto [ec, rbuf] = co_await read_blocks(cid, blk_offset / blk_sz, 1);
        if (!ec) {
            tail_block_ = TailBlock{std::move(rbuf), tail_chunk_idx, blk_offset, sub_blk_used};
        }
    } else {
        tail_block_.reset();
    }

    // Persist bytes_written MetaBlk for dirtied chunks.
    if (chunk_bytes_written.empty()) { co_return true; }

    auto lock = co_await mblk_mutex_.co_scoped_lock();
    for (auto& [cid, bytes_written] : chunk_bytes_written) {
        auto it = chunk_mblks_.find(cid);
        if (it == chunk_mblks_.end()) { continue; }

        AppendByteChunkMeta meta{bytes_written};
        auto meta_buf = sisl::make_byte_array(sizeof(AppendByteChunkMeta));
        std::memcpy(meta_buf->bytes(), &meta, sizeof(meta));
        co_await meta_client_.write_meta_blk(it->second, meta_buf);
    }

    co_return true;
}

} // namespace homestore
