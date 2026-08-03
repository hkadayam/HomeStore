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
#include <charconv>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <sisl/logging/logging.h>

#include "homestore/blob/blob_dev.h"
#include "homestore/blob/raw_blk_stream.h"
#include "homestore/blob/append_blk_stream.h"
#include "homestore/blob/append_byte_stream.h"
#include "homestore/device/chunk.h"
#include "homestore/device/virtual_dev.h"

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

template < typename StreamT >
static shared< StreamT > find_stream(folly::SharedMutex& mtx, std::map< uint64_t, shared< StreamT > > const& map,
                                     uint64_t stream_id) {
    std::shared_lock holder(mtx);
    auto it = map.find(stream_id);
    return (it != map.end()) ? it->second : nullptr;
}

shared< RawBlkStream > BlobDev::get_raw_blk_stream(uint64_t stream_id) const {
    return find_stream(streams_mutex_, raw_blk_streams_, stream_id);
}

shared< AppendBlkStream > BlobDev::get_append_blk_stream(uint64_t stream_id) const {
    return find_stream(streams_mutex_, append_blk_streams_, stream_id);
}

shared< AppendByteStream > BlobDev::get_append_byte_stream(uint64_t stream_id) const {
    return find_stream(streams_mutex_, append_byte_streams_, stream_id);
}

// Extract the stream from `map` under the lock (deregistering it so cp_flush won't touch it mid-teardown), then run
// its destroy() outside the lock.  Returns without touching anything if the id isn't present.
template < typename StreamT >
static Async< void > destroy_stream(folly::SharedMutex& mtx, std::map< uint64_t, shared< StreamT > >& map,
                                    uint64_t stream_id, const char* type) {
    shared< StreamT > stream;
    size_t remaining{0};
    {
        std::unique_lock lg{mtx};
        auto it = map.find(stream_id);
        if (it == map.end()) {
            LOGINFOMOD(blob_dev, "destroy_stream: {} id={} not registered — nothing to do (map has {})", type,
                       stream_id, map.size());
            co_return;
        }
        stream = std::move(it->second);
        map.erase(it);
        remaining = map.size();
    }
    LOGINFOMOD(blob_dev, "destroy_stream: {} id={} deregistered, destroying (map now has {})", type, stream_id,
               remaining);
    co_await stream->destroy();
}

Async< void > BlobDev::destroy_raw_blk_stream(uint64_t stream_id) {
    co_await destroy_stream(streams_mutex_, raw_blk_streams_, stream_id, "raw_blk");
}
Async< void > BlobDev::destroy_append_blk_stream(uint64_t stream_id) {
    co_await destroy_stream(streams_mutex_, append_blk_streams_, stream_id, "append_blk");
}
Async< void > BlobDev::destroy_append_byte_stream(uint64_t stream_id) {
    co_await destroy_stream(streams_mutex_, append_byte_streams_, stream_id, "append_byte");
}

// ─────────────────────────────────────────────────────────────────────────────
// MetaBlk name helpers
// ─────────────────────────────────────────────────────────────────────────────

// Format: <dev>_<type_name>_<stream_id>_<chunk_id>_<blk_size>
// e.g.    "Index_rawblk_3_42_4096"
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
    // Each name has the form: <dev_name>_<type_name>_<stream_id>_<chunk_id>_<blk_size>
    static const std::pair< std::string_view, StreamType > kPatterns[] = {
        {kRawBlkSuffix, StreamType::RawBlk},
        {kAppendBlkSuffix, StreamType::AppendBlk},
        {kAppendByteSuffix, StreamType::AppendByte},
    };

    for (auto& [suffix, type] : kPatterns) {
        std::string prefix{dev_name};
        prefix += suffix;

        if (name.substr(0, prefix.size()) != prefix) {
            continue;
        }

        // Remainder is "<stream_id>_<chunk_id>_<blk_size>"
        std::string_view rest = name.substr(prefix.size());

        auto sep1 = rest.find('_');
        if (sep1 == std::string_view::npos) {
            continue;
        }

        uint64_t stream_id{};
        auto res1 = std::from_chars(rest.data(), rest.data() + sep1, stream_id);
        if (res1.ec != std::errc{}) {
            continue;
        }

        std::string_view after_sid = rest.substr(sep1 + 1);
        auto sep2 = after_sid.find('_');
        if (sep2 == std::string_view::npos) {
            continue;
        }

        uint32_t chunk_id{};
        auto res2 = std::from_chars(after_sid.data(), after_sid.data() + sep2, chunk_id);
        if (res2.ec != std::errc{}) {
            continue;
        }

        std::string_view bs_part = after_sid.substr(sep2 + 1);
        uint32_t blk_size{};
        auto res3 = std::from_chars(bs_part.data(), bs_part.data() + bs_part.size(), blk_size);
        if (res3.ec != std::errc{}) {
            continue;
        }

        return ParsedChunkMblk{type, stream_id, chunk_id, blk_size};
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// CP lifecycle
// ─────────────────────────────────────────────────────────────────────────────

Async< void > BlobDev::cp_flush(CP* cp) {
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

Async< shared< RawBlkStream > > BlobDev::create_raw_blk_stream(uint64_t chunk_size, uint32_t blk_size) {
    auto sid = next_stream_id();
    auto stream = co_await RawBlkStream::create(sid, meta_client_, dev_name_, vdev_, chunk_size, blk_size);
    {
        std::unique_lock lk{streams_mutex_};
        raw_blk_streams_.emplace(sid, stream);
    }
    co_return stream;
}

Async< shared< AppendBlkStream > > BlobDev::create_append_blk_stream(uint64_t chunk_size, uint32_t blk_size) {
    auto sid = next_stream_id();
    auto stream = co_await AppendBlkStream::create(sid, meta_client_, dev_name_, vdev_, chunk_size, blk_size);
    {
        std::unique_lock lk{streams_mutex_};
        append_blk_streams_.emplace(sid, stream);
    }
    co_return stream;
}

Async< shared< AppendByteStream > > BlobDev::create_append_byte_stream(uint64_t chunk_size, bool concurrent_safe) {
    auto sid = next_stream_id();
    auto stream = co_await AppendByteStream::create(sid, meta_client_, dev_name_, vdev_, chunk_size, concurrent_safe);
    {
        std::unique_lock lk{streams_mutex_};
        append_byte_streams_.emplace(sid, stream);
    }
    co_return stream;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recovery load
// ─────────────────────────────────────────────────────────────────────────────

Async< void > BlobDev::load(StreamMblkMap&& raw_blk, StreamMblkMap&& append_blk, AppendByteSbMap&& append_byte) {
    uint64_t max_sid = 0;

    for (auto& [sid, info] : raw_blk) {
        raw_blk_streams_[sid] = co_await RawBlkStream::load(sid, meta_client_, dev_name_, vdev_, info.blk_size,
                                                            std::move(info.chunk_mblks));
        max_sid = std::max(max_sid, sid);
    }
    for (auto& [sid, info] : append_blk) {
        append_blk_streams_[sid] = co_await AppendBlkStream::load(sid, meta_client_, dev_name_, vdev_, info.blk_size,
                                                                  std::move(info.chunk_mblks));
        max_sid = std::max(max_sid, sid);
    }
    for (auto& [sid, info] : append_byte) {
        append_byte_streams_[sid] = co_await AppendByteStream::load(sid, meta_client_, dev_name_, vdev_,
                                                                    std::move(info.sb), std::move(info.payload));
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

Async< void > BlobDev::reconcile_chunks() {
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
