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
#include <charconv>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "blob/blob_dev.h"
#include "blob/raw_blk_stream.h"
#include "blob/append_blk_stream.h"
#include "blob/append_byte_stream.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// StreamType suffix strings (embedded in MetaBlk names)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr std::string_view kRawBlkSuffix = "_rawblk_";
static constexpr std::string_view kAppendBlkSuffix = "_appendblk_";
static constexpr std::string_view kAppendByteSuffix = "_appendbyte_";

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

BlobDev::BlobDev(std::string dev_name, shared< VirtualDev > vdev, MetaClient& meta_client) :
        dev_name_{std::move(dev_name)}, vdev_{std::move(vdev)}, meta_client_{meta_client} {
}

BlobDev::~BlobDev() = default;

VirtualDev& BlobDev::vdev() const {
    return *vdev_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream accessors
// ─────────────────────────────────────────────────────────────────────────────

template < typename T >
static std::vector< shared< T > > collect_streams(const folly::SharedMutex& mtx,
                                                   const std::map< uint64_t, shared< T > >& m) {
    std::shared_lock lk{mtx};
    std::vector< shared< T > > out;
    out.reserve(m.size());
    for (auto& [_, s] : m) {
        out.push_back(s);
    }
    return out;
}

std::vector< shared< RawBlkStream > > BlobDev::raw_blk_streams() const {
    return collect_streams(streams_mutex_, raw_blk_streams_);
}

std::vector< shared< AppendBlkStream > > BlobDev::append_blk_streams() const {
    return collect_streams(streams_mutex_, append_blk_streams_);
}

std::vector< shared< AppendByteStream > > BlobDev::append_byte_streams() const {
    return collect_streams(streams_mutex_, append_byte_streams_);
}

// ─────────────────────────────────────────────────────────────────────────────
// MetaBlk name helpers
// ─────────────────────────────────────────────────────────────────────────────

// Format: <dev>_<type_name>_<stream_id>_<chunk_id>
// e.g.    "Index_rawblk_3_42"
std::string BlobDev::chunk_mblk_name(StreamType type, uint64_t stream_id, uint32_t chunk_id) const {
    std::string_view type_name{};
    switch (type) {
    case StreamType::RawBlk:
        type_name = kRawBlkSuffix;
        break;
    case StreamType::AppendBlk:
        type_name = kAppendBlkSuffix;
        break;
    case StreamType::AppendByte:
        type_name = kAppendByteSuffix;
        break;
    }
    return fmt::format("{}{}{}_{}", dev_name_, type_name, stream_id, chunk_id);
}

std::optional< BlobDev::ParsedChunkMblk > BlobDev::parse_chunk_mblk_name(const std::string_view& dev_name,
                                                                         const std::string_view& name) {
    // Each name has the form: <dev_name>_<type_name>_<stream_id>_<chunk_id>
    static const std::pair< std::string_view, StreamType > kPatterns[] = {
        {kRawBlkSuffix, StreamType::RawBlk},
        {kAppendBlkSuffix, StreamType::AppendBlk},
        {kAppendByteSuffix, StreamType::AppendByte},
    };

    for (auto& [suffix, type] : kPatterns) {
        // Expected prefix: <dev_name><suffix>
        std::string prefix{dev_name};
        prefix += suffix;

        if (name.substr(0, prefix.size()) != prefix) {
            continue;
        }

        // Remainder is "<stream_id>_<chunk_id>"
        std::string_view rest = name.substr(prefix.size());

        // Find the underscore separating stream_id and chunk_id.
        auto sep = rest.find('_');
        if (sep == std::string_view::npos) {
            continue;
        }

        uint64_t stream_id{};
        auto res1 = std::from_chars(rest.data(), rest.data() + sep, stream_id);
        if (res1.ec != std::errc{}) {
            continue;
        }

        std::string_view chunk_part = rest.substr(sep + 1);
        uint32_t chunk_id{};
        auto res2 = std::from_chars(chunk_part.data(), chunk_part.data() + chunk_part.size(), chunk_id);
        if (res2.ec != std::errc{}) {
            continue;
        }

        return ParsedChunkMblk{type, stream_id, chunk_id};
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// CP lifecycle
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDev::cp_flush(CP* cp) {
    // Collect streams under shared lock, then flush outside the lock.
    auto rbs = raw_blk_streams();
    auto abs = append_blk_streams();

    for (auto& s : rbs) {
        if (s->is_dirty(cp->id())) {
            co_await s->cp_flush(cp);
        }
    }
    for (auto& s : abs) {
        if (s->is_dirty(cp->id())) {
            co_await s->cp_flush(cp);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream creation
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< RawBlkStream > > BlobDev::create_raw_blk_stream(uint64_t chunk_size) {
    auto sid = next_stream_id();
    auto stream = co_await RawBlkStream::create(sid, meta_client_, dev_name_, vdev_, chunk_size);
    {
        std::unique_lock lk{streams_mutex_};
        raw_blk_streams_.emplace(sid, stream);
    }
    co_return stream;
}

folly::coro::Task< shared< AppendBlkStream > > BlobDev::create_append_blk_stream(uint64_t chunk_size) {
    auto sid = next_stream_id();
    auto stream = co_await AppendBlkStream::create(sid, meta_client_, dev_name_, vdev_, chunk_size);
    {
        std::unique_lock lk{streams_mutex_};
        append_blk_streams_.emplace(sid, stream);
    }
    co_return stream;
}

folly::coro::Task< shared< AppendByteStream > > BlobDev::create_append_byte_stream(uint64_t chunk_size) {
    auto sid = next_stream_id();
    auto stream = co_await AppendByteStream::create(sid, meta_client_, dev_name_, vdev_, chunk_size);
    {
        std::unique_lock lk{streams_mutex_};
        append_byte_streams_.emplace(sid, stream);
    }
    co_return stream;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recovery load
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDev::load(StreamMblkMap&& raw_blk, StreamMblkMap&& append_blk,
                                        StreamMblkMap&& append_byte) {
    uint64_t max_sid = 0;

    for (auto& [sid, mblks] : raw_blk) {
        raw_blk_streams_[sid] = co_await RawBlkStream::load(sid, meta_client_, dev_name_, vdev_, std::move(mblks));
        max_sid = std::max(max_sid, sid);
    }
    for (auto& [sid, mblks] : append_blk) {
        append_blk_streams_[sid] =
            co_await AppendBlkStream::load(sid, meta_client_, dev_name_, vdev_, std::move(mblks));
        max_sid = std::max(max_sid, sid);
    }
    for (auto& [sid, mblks] : append_byte) {
        append_byte_streams_[sid] =
            co_await AppendByteStream::load(sid, meta_client_, dev_name_, vdev_, std::move(mblks));
        max_sid = std::max(max_sid, sid);
    }

    // Set next_stream_id_ past the highest recovered stream_id.
    if (!raw_blk.empty() || !append_blk.empty() || !append_byte.empty()) {
        next_stream_id_.store(max_sid + 1, std::memory_order_relaxed);
    }

    co_await reconcile_chunks();
}

// ─────────────────────────────────────────────────────────────────────────────
// reconcile_chunks
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDev::reconcile_chunks() {
    // Build the set of chunk_ids claimed by all loaded streams.
    std::unordered_set< uint32_t > claimed;
    auto collect = [&](const StreamBase* stream) {
        if (!stream) {
            return;
        }
        auto acc = stream->chunks();
        for (auto& chunk : *acc) {
            claimed.insert(chunk->chunk_id());
        }
    };
    for (auto& [_, s] : raw_blk_streams_) {
        collect(s.get());
    }
    for (auto& [_, s] : append_blk_streams_) {
        collect(s.get());
    }
    for (auto& [_, s] : append_byte_streams_) {
        collect(s.get());
    }

    // Any chunk in the VDev that is not claimed is orphaned.
    auto all_chunks = vdev_->get_chunks();
    for (auto& chunk : all_chunks) {
        if (!claimed.count(chunk->chunk_id())) {
            co_await vdev_->shrink(ChunkToShrink::Specific, chunk->chunk_id());
        }
    }
}

} // namespace homestore
