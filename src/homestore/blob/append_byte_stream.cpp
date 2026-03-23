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

#include "checkpoint/cp.h"     // CP, cp_id_t
#include "checkpoint/cp_mgr.h" // CPManager

#include "blob/append_byte_stream.h"
#include "blob/blob_dev.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"
#include "managers.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// Private constructor
// ─────────────────────────────────────────────────────────────────────────────

AppendByteStream::AppendByteStream(MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev,
                                   uint64_t chunk_size, ChunkMblkMap&& mblks) :
        StreamBase{
            enum_value(StreamType::AppendByte), vdev, meta_client, std::move(dev_name), chunk_size, std::move(mblks)} {
}

// ─────────────────────────────────────────────────────────────────────────────
// create / load
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< AppendByteStream > > AppendByteStream::create(MetaClient& meta_client,
                                                                         const std::string& dev_name,
                                                                         const shared< VirtualDev >& vdev,
                                                                         uint64_t chunk_size) {
    auto stream =
        shared< AppendByteStream >{new AppendByteStream{meta_client, std::string{dev_name}, vdev, chunk_size}};
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< AppendByteStream > > AppendByteStream::load(MetaClient& meta_client,
                                                                       const std::string& dev_name,
                                                                       const shared< VirtualDev >& vdev,
                                                                       ChunkMblkMap&& mblks) {
    // Sum bytes_written from each recovered payload to reconstruct tail_offset.
    uint64_t recovered_tail = 0;
    for (auto& [cid, entry] : mblks) {
        if (entry.second.size() >= sizeof(AppendByteChunkMeta)) {
            const auto* meta = reinterpret_cast< const AppendByteChunkMeta* >(entry.second.cbytes());
            recovered_tail += meta->bytes_written;
        }
    }

    const uint64_t chunk_sz = vdev->chunk_size_bytes();
    auto stream = shared< AppendByteStream >{
        new AppendByteStream{meta_client, std::string{dev_name}, vdev, chunk_sz, std::move(mblks)}};
    stream->tail_offset_.store(recovered_tail, std::memory_order_release);
    co_return stream;
}

folly::coro::Task< AppendByteStream::WriteUnit* > AppendByteStream::alloc_write_unit(AppendByteCPSession& session,
                                                                                     cp_id_t cp_id) {
    const uint64_t csz = chunk_size();
    const uint32_t blk_sz = block_size();

    // On first allocation of this CP epoch, initialize write_cursor from the current tail.
    if (session.all_units.empty()) {
        session.write_cursor = tail_offset_.load(std::memory_order_relaxed);
    }

    const size_t chunk_idx = static_cast< size_t >(session.write_cursor / csz);
    uint64_t offset_in_chunk = session.write_cursor % csz;

    // If exactly at a chunk boundary, advance to the next chunk.
    if (offset_in_chunk == 0 && session.write_cursor > 0) {
        // write_cursor is at the start of a new chunk; chunk_idx is already correct.
    }

    // Ensure the chunk exists; expand if needed.
    co_await expand_to(chunk_idx);

    // Cap buffer size to not cross the chunk boundary, then round down to block alignment.
    const uint64_t remaining_in_chunk = csz - offset_in_chunk;
    uint32_t buf_capacity =
        static_cast< uint32_t >(std::min(static_cast< uint64_t >(kMaxWriteUnitBytes), remaining_in_chunk));
    buf_capacity = (buf_capacity / blk_sz) * blk_sz; // round down to block alignment

    // Look up the chunk_id for this chunk index.
    chunk_num_t cid{};
    {
        auto acc = chunks();
        cid = static_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
    }

    cp_session(cp_id).mark_chunk_dirty(cid);
    session.write_cursor += buf_capacity;

    IOBuffer buf{buf_capacity, blk_sz};
    auto wu = std::make_unique< WriteUnit >(cid, static_cast< uint32_t >(offset_in_chunk), std::move(buf));
    auto* ptr = wu.get();
    session.all_units.push_back(std::move(wu));
    co_return ptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// append
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< uint64_t > AppendByteStream::append(cp_id_t cp_id, sisl::Blob data) {
    const uint32_t len = data.size();
    AppendByteCPSession& session = cp_session_[cp_id % CPManager::max_concurent_cps];

    // Hot path: shared lock + CAS to reserve space in the active (last) WriteUnit.
    {
        auto lock = co_await append_mutex_.co_scoped_lock_shared();
        if (!session.all_units.empty()) {
            WriteUnit* wu = session.all_units.back().get();
            uint32_t cur = wu->used_bytes.load(std::memory_order_relaxed);
            while (cur + len <= wu->buf.size()) {
                if (wu->used_bytes.compare_exchange_weak(cur, cur + len, std::memory_order_relaxed)) {
                    std::memcpy(wu->buf.bytes() + cur, data.cbytes(), len);
                    co_return tail_offset_.fetch_add(len, std::memory_order_relaxed);
                }
            }
        }
    }

    // Cold path: exclusive lock — allocate a new WriteUnit if the last one is still full.
    {
        auto lock = co_await append_mutex_.co_scoped_lock();
        WriteUnit* wu = nullptr;

        // Re-check: another thread may have allocated a new WriteUnit while we waited for the exclusive lock.
        if (!session.all_units.empty()) {
            wu = session.all_units.back().get();
            uint32_t cur = wu->used_bytes.load(std::memory_order_relaxed);
            while (cur + len <= wu->buf.size()) {
                if (wu->used_bytes.compare_exchange_weak(cur, cur + len, std::memory_order_relaxed)) {
                    std::memcpy(wu->buf.bytes() + cur, data.cbytes(), len);
                    co_return tail_offset_.fetch_add(len, std::memory_order_relaxed);
                }
            }
        }

        // Still no room — allocate a new WriteUnit.
        wu = co_await alloc_write_unit(session, cp_id);
        wu->used_bytes.store(len, std::memory_order_relaxed);
        std::memcpy(wu->buf.bytes(), data.cbytes(), len);
        co_return tail_offset_.fetch_add(len, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// read
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< std::pair< std::error_code, IOBuffer > > AppendByteStream::read(IOBuffer buf, uint64_t byte_offset,
                                                                                   size_t len) {
    const uint32_t blk_sz = block_size();
    const uint64_t csz = chunk_size();

    const size_t chunk_idx = static_cast< size_t >(byte_offset / csz);
    const uint64_t offset_in_chunk = byte_offset % csz;
    const uint32_t blk_num = static_cast< uint32_t >(offset_in_chunk / blk_sz);
    const blk_count_t nblks = static_cast< blk_count_t >((len + blk_sz - 1) / blk_sz);

    chunk_num_t cid{};
    {
        auto acc = chunks();
        if (chunk_idx >= acc->size()) {
            co_return {std::make_error_code(std::errc::invalid_argument), IOBuffer{}};
        }
        cid = static_cast< chunk_num_t >((*acc)[chunk_idx]->chunk_id());
    }

    const BlkId bid{blk_num, nblks, cid};
    co_return co_await vdev().read(std::move(buf), bid);
}

// ─────────────────────────────────────────────────────────────────────────────
// truncate
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > AppendByteStream::truncate() {
    tail_offset_.store(0, std::memory_order_release);
    for (auto& sess : cp_session_) {
        sess.reset();
    }

    // Reset bytes_written in every chunk MetaBlk to 0.
    AppendByteChunkMeta meta{0};
    IOBuffer meta_buf{sizeof(AppendByteChunkMeta)};
    std::memcpy(meta_buf.bytes(), &meta, sizeof(meta));

    auto lock = co_await mblk_mutex_.co_scoped_lock();
    for (auto& [cid, mblk] : chunk_mblks_) {
        co_await meta_client_.write_meta_blk(mblk, meta_buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CP hooks
// ─────────────────────────────────────────────────────────────────────────────

void AppendByteStream::on_cp_switchover(CP* /*cur_cp*/, CP* new_cp) {
    cp_session_[new_cp->id() % CPManager::max_concurent_cps].reset();
}

folly::coro::Task< bool > AppendByteStream::cp_flush(CP* cp) {
    AppendByteCPSession& session = cp_session_[cp->id() % CPManager::max_concurent_cps];
    auto units = std::move(session.all_units);

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
        const uint32_t blk_aligned = used_nblks * blk_sz;
        if (blk_aligned > used) {
            std::memset(wu->buf.bytes() + used, 0, blk_aligned - used);
        }

        const uint32_t blk_num = wu->offset_in_chunk / blk_sz;
        const BlkId bid{blk_num, used_nblks, wu->chunk_id};
        co_await vdev().write(wu->buf, bid);

        // Track the high-water mark for this chunk.
        auto& hw = chunk_bytes_written[wu->chunk_id];
        hw = std::max(hw, static_cast< uint64_t >(wu->offset_in_chunk + used));
    }

    // Persist bytes_written MetaBlk only for chunks dirtied during this CP epoch.
    auto dirty = cp_session(cp->id()).gather_dirty_chunks();
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
        IOBuffer meta_buf{sizeof(AppendByteChunkMeta)};
        std::memcpy(meta_buf.bytes(), &meta, sizeof(meta));
        co_await meta_client_.write_meta_blk(it->second, meta_buf);
    }

    co_return true;
}

} // namespace homestore