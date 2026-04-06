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
                                   const shared< VirtualDev >& vdev, uint64_t chunk_size, ChunkMblkMap&& mblks) :
        StreamBase{stream_id, vdev, meta_client, std::move(dev_name), chunk_size, std::move(mblks)} {
}

// ─────────────────────────────────────────────────────────────────────────────
// create / load
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< AppendByteStream > > AppendByteStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                                         const std::string& dev_name,
                                                                         const shared< VirtualDev >& vdev,
                                                                         uint64_t chunk_size) {
    auto stream = shared< AppendByteStream >{
        new AppendByteStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size}};
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< AppendByteStream > > AppendByteStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                                       const std::string& dev_name,
                                                                       const shared< VirtualDev >& vdev,
                                                                       ChunkMblkMap&& mblks) {
    // Sum bytes_written from each recovered payload to reconstruct tail_offset.
    uint64_t recovered_tail = 0;
    for (auto& [cid, entry] : mblks) {
        if (entry.second.size() >= sizeof(AppendByteChunkMeta)) {
            const auto* meta = reinterpret_cast< const AppendByteChunkMeta* >(entry.second.bytes());
            recovered_tail += meta->bytes_written;
        }
    }

    const uint64_t chunk_sz = vdev->chunk_size_bytes();
    const uint32_t blk_sz = vdev->block_size();
    auto stream = shared< AppendByteStream >{
        new AppendByteStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz, std::move(mblks)}};
    stream->tail_offset_.store(recovered_tail, std::memory_order_release);

    // If the recovered tail is not block-aligned, read the partial tail block from disk so subsequent appends can
    // continue from the exact byte offset within that block.  The cached TailBlock is consumed by the first
    // alloc_write_unit call, which seeds a WriteUnit from it — no zero-padding gap is introduced.
    if (recovered_tail > 0 && (recovered_tail % blk_sz) != 0) {
        const size_t tail_chunk_idx = to_size((recovered_tail - 1) / chunk_sz);
        const uint64_t tail_in_chunk = ((recovered_tail - 1) % chunk_sz) + 1; // bytes used in the last chunk
        const uint32_t blk_offset = to_u32(((tail_in_chunk - 1) / blk_sz) * blk_sz);
        const uint32_t sub_blk_used = to_u32(tail_in_chunk - blk_offset);

        chunk_num_t cid{};
        {
            auto acc = stream->chunks();
            cid = static_cast< chunk_num_t >((*acc)[tail_chunk_idx]->chunk_id());
        }

        // Read the partial block from disk.
        auto [ec, rbuf] = co_await stream->read_blocks(cid, blk_offset / blk_sz, 1);
        if (!ec) {
            stream->tail_block_ = TailBlock{std::move(rbuf), cid, blk_offset, sub_blk_used};
        }
    }

    co_return stream;
}

// ─────────────────────────────────────────────────────────────────────────────
// alloc_write_unit
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< AppendByteStream::WriteUnit* > AppendByteStream::alloc_write_unit(FlushSession& session) {
    // If a TailBlock is cached (from a previous flush or restart recovery), seed the first WriteUnit from it.  The
    // WriteUnit's buffer already contains the partial block's data; used_bytes is set to the sub-block valid count so
    // new appends continue from the exact byte offset.
    if (session.all_units.empty() && tail_block_) {
        auto tb = std::move(*tail_block_);
        tail_block_.reset();

        session.mark_chunk_dirty(tb.chunk_id);
        // write_cursor advances past this single-block WriteUnit.
        const uint64_t tail = tail_offset_.load(std::memory_order_relaxed);
        session.write_cursor = tail - (tail % to_u64(block_size())) + to_u64(block_size());

        auto wu = std::make_unique< WriteUnit >(tb.chunk_id, tb.offset_in_chunk, std::move(tb.buf));
        wu->used_bytes.store(tb.valid_bytes, std::memory_order_relaxed);
        auto* ptr = wu.get();
        session.all_units.push_back(std::move(wu));
        co_return ptr;
    }

    // On first allocation of this session (no tail block), initialize write_cursor from the current tail.  At this
    // point tail_offset_ is guaranteed to be block-aligned (either from create, or because the previous flush
    // saved the partial block into tail_block_ which was consumed above, or tail was already aligned).
    if (session.all_units.empty()) {
        session.write_cursor = tail_offset_.load(std::memory_order_relaxed);
    }

    const size_t chunk_idx = to_size(session.write_cursor / chunk_size());
    const uint64_t offset_in_chunk = session.write_cursor % chunk_size();

    // Ensure the chunk exists; expand if needed.
    co_await expand_to(chunk_idx);

    // Cap buffer size to not cross the chunk boundary, then round down to block alignment.
    const uint64_t remaining_in_chunk = chunk_size() - offset_in_chunk;
    uint32_t buf_capacity = to_u32(std::min(to_u64(kMaxWriteUnitBytes), remaining_in_chunk));
    buf_capacity = (buf_capacity / block_size()) * block_size();

    // Look up the chunk_id for this chunk index.
    chunk_num_t cid{};
    {
        auto acc = chunks();
        cid = static_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
    }

    session.mark_chunk_dirty(cid);
    session.write_cursor += buf_capacity;

    IOBuffer buf{buf_capacity, block_size()};
    auto wu = std::make_unique< WriteUnit >(cid, to_u32(offset_in_chunk), std::move(buf));
    auto* ptr = wu.get();
    session.all_units.push_back(std::move(wu));
    co_return ptr;
}

folly::coro::Task< uint64_t > AppendByteStream::append(const sisl::Blob& data) {
    const uint32_t len = data.size();

    // RCU read to get the current session.  The access_ptr holds a read-side guard that must be released before any
    // co_await (RCU readers must be short-lived).  We dereference once and work with the raw pointer — the session
    // remains valid for the duration of the RCU read-side critical section.
    auto do_append = [this, &data, len](FlushSession* session) -> std::optional< uint64_t > {
        if (!session->all_units.empty()) {
            WriteUnit* wu = session->all_units.back().get();
            uint32_t cur = wu->used_bytes.load(std::memory_order_relaxed);
            while (cur + len <= wu->buf.size()) {
                if (wu->used_bytes.compare_exchange_weak(cur, cur + len, std::memory_order_relaxed)) {
                    std::memcpy(wu->buf.bytes() + cur, data.cbytes(), len);
                    return tail_offset_.fetch_add(len, std::memory_order_relaxed);
                }
            }
        }
        return std::nullopt;
    };

    // Hot path: shared lock + CAS to reserve space in the active (last) WriteUnit.
    {
        auto lock = co_await append_mutex_.co_scoped_lock_shared();
        auto acc = session_.get();
        if (auto offset = do_append(acc.get())) {
            co_return *offset;
        }
    }

    // Cold path: exclusive lock — allocate a new WriteUnit if the last one is still full.
    {
        auto lock = co_await append_mutex_.co_scoped_lock();

        FlushSession* session;
        {
            auto acc = session_.get();
            session = acc.get();

            // Re-check: another thread may have allocated a new WriteUnit while we waited for the exclusive lock.
            if (auto offset = do_append(session)) {
                co_return *offset;
            }
        }
        // RCU guard released above — safe to co_await.  The session pointer remains valid because we hold the
        // exclusive append_mutex_, which prevents flush() from swapping the session while we are allocating.

        // Still no room — allocate a new WriteUnit.  If it was seeded from a TailBlock, used_bytes is already set to
        // the sub-block valid count; new data appends after the existing bytes.
        WriteUnit* wu = co_await alloc_write_unit(*session);
        const uint32_t cur = wu->used_bytes.load(std::memory_order_relaxed);
        wu->used_bytes.store(cur + len, std::memory_order_relaxed);
        std::memcpy(wu->buf.bytes() + cur, data.cbytes(), len);
        co_return tail_offset_.fetch_add(len, std::memory_order_relaxed);
    }
}

folly::coro::Task< void > AppendByteStream::truncate(bool release_chunks) {
    tail_offset_.store(0, std::memory_order_release);
    tail_block_.reset();

    // Swap in a fresh session, discarding the old one.
    session_.make_and_exchange();

    if (release_chunks) {
        // Release all chunks back to VDev (and chunk pool). Remove their MetaBlks first.
        {
            auto lock = co_await mblk_mutex_.co_scoped_lock();
            for (auto& [cid, mblk] : chunk_mblks_) {
                co_await meta_client_.remove_meta_blk(mblk);
            }
            chunk_mblks_.clear();
        }
        co_await destroy(); // Returns chunks to VDev chunk pool via shrink()
    } else {
        // Retain chunks for reuse, just reset bytes_written in every chunk MetaBlk to 0.
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
        static_cast< blk_count_t >((offset_in_chunk + len + block_size() - 1) / block_size() - to_u64(blk_num));

    chunk_num_t cid{};
    {
        auto acc = chunks();
        if (chunk_idx >= acc->size()) {
            co_return {std::make_error_code(std::errc::invalid_argument), IOBuffer{}};
        }
        cid = static_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
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
        stream_{stream}, pos_{start}, end_{end} {
}

AppendByteStream::ReadCursor AppendByteStream::open_cursor(uint64_t start_offset) const {
    return ReadCursor{const_cast< AppendByteStream& >(*this), start_offset, tail_offset()};
}

AppendByteStream::ReadCursor AppendByteStream::open_cursor(uint64_t start_offset, uint64_t end_offset) const {
    return ReadCursor{const_cast< AppendByteStream& >(*this), start_offset, end_offset};
}

folly::coro::Task< std::pair< IOBuffer, uint32_t > > AppendByteStream::ReadCursor::next(size_t max_bytes) {
    if (pos_ >= end_) {
        co_return {IOBuffer{}, 0};
    }

    const uint32_t blk_sz = stream_.block_size();
    const uint64_t csz = stream_.chunk_size();

    // Cap to remaining bytes in the stream and to the current chunk boundary.
    const uint64_t remaining = end_ - pos_;
    const uint64_t offset_in_chunk = pos_ % csz;
    const uint64_t remaining_in_chunk = csz - offset_in_chunk;
    const uint64_t read_len = std::min({to_u64(max_bytes), remaining, remaining_in_chunk});

    const size_t chunk_idx = to_size(pos_ / csz);
    const uint32_t blk_num = to_u32(offset_in_chunk / blk_sz);
    const blk_count_t nblks =
        static_cast< blk_count_t >((offset_in_chunk + read_len + blk_sz - 1) / blk_sz - to_u64(blk_num));
    const uint32_t valid = to_u32(read_len);

    chunk_num_t cid{};
    {
        auto acc = stream_.chunks();
        if (chunk_idx >= acc->size()) {
            co_return {IOBuffer{}, 0};
        }
        cid = static_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
    }

    auto [ec, buf] = co_await stream_.read_blocks(cid, blk_num, nblks);
    if (ec) {
        co_return {IOBuffer{}, 0};
    }

    pos_ += valid;
    co_return {std::move(buf), valid};
}

folly::coro::Task< bool > AppendByteStream::flush() {
    // Atomically install a new empty session.  After make_and_exchange returns, all in-flight RCU readers have
    // completed and no new append() call will touch the old session.  We take exclusive append_mutex_ first so that
    // no appender is in the cold path (which holds a raw pointer to the session across co_await).
    std::shared_ptr< FlushSession > old_session;
    {
        auto lock = co_await append_mutex_.co_scoped_lock();
        old_session = session_.make_and_exchange();
    }

    auto units = std::move(old_session->all_units);
    const uint32_t blk_sz = block_size();

    // Accumulate high-water mark (offset_in_chunk + used) per chunk for MetaBlk persistence.
    std::unordered_map< uint32_t, uint64_t > chunk_bytes_written;

    // Finalize each WriteUnit: write used portion to VDev at its positional offset within the chunk.
    for (auto& wu : units) {
        const uint32_t used = wu->used_bytes.load(std::memory_order_relaxed);
        if (used == 0) {
            continue;
        }

        const blk_count_t used_nblks = static_cast< blk_count_t >((used + blk_sz - 1) / blk_sz);

        // Zero-pad the tail of the last used block.
        const uint32_t blk_aligned = to_u32(used_nblks) * blk_sz;
        if (blk_aligned > used) {
            std::memset(wu->buf.bytes() + used, 0, blk_aligned - used);
        }

        const uint32_t blk_num = wu->offset_in_chunk / blk_sz;
        const BlkId bid{blk_num, used_nblks, wu->chunk_id};
        co_await vdev().write(wu->buf, bid);

        // Track the high-water mark for this chunk.
        auto& hw = chunk_bytes_written[wu->chunk_id];
        hw = std::max(hw, to_u64(wu->offset_in_chunk + used));
    }

    // Cache the partial tail block for the next session.  If the last WriteUnit's used_bytes is not block-aligned,
    // save the final partial block so the next alloc_write_unit can seed from it, keeping the stream contiguous.
    if (!units.empty()) {
        for (auto it = units.rbegin(); it != units.rend(); ++it) {
            const uint32_t used = (*it)->used_bytes.load(std::memory_order_relaxed);
            if (used == 0) {
                continue;
            }
            if ((used % blk_sz) != 0) {
                const uint32_t last_blk_in_wu = ((used - 1) / blk_sz) * blk_sz;
                const uint32_t sub_blk_used = used - last_blk_in_wu;

                IOBuffer saved{blk_sz, blk_sz};
                std::memcpy(saved.bytes(), (*it)->buf.bytes() + last_blk_in_wu, blk_sz);
                tail_block_ =
                    TailBlock{std::move(saved), (*it)->chunk_id, (*it)->offset_in_chunk + last_blk_in_wu, sub_blk_used};
            } else {
                tail_block_.reset();
            }
            break;
        }
    }

    // Persist bytes_written MetaBlk only for chunks dirtied during this flush session.
    auto dirty = old_session->gather_dirty_chunks();
    if (dirty.empty()) {
        co_return true;
    }

    auto lock = co_await mblk_mutex_.co_scoped_lock();
    for (auto chunk_id : dirty) {
        auto it = chunk_mblks_.find(chunk_id);
        if (it == chunk_mblks_.end()) {
            continue;
        }

        auto bw_it = chunk_bytes_written.find(chunk_id);
        const uint64_t bytes_in_chunk = (bw_it != chunk_bytes_written.end()) ? bw_it->second : 0;

        AppendByteChunkMeta meta{bytes_in_chunk};
        auto meta_buf = sisl::make_byte_array(sizeof(AppendByteChunkMeta));
        std::memcpy(meta_buf->bytes(), &meta, sizeof(meta));
        co_await meta_client_.write_meta_blk(it->second, meta_buf);
    }

    co_return true;
}

} // namespace homestore
