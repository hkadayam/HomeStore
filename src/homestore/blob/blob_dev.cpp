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
// MetaBlk name helpers
// ─────────────────────────────────────────────────────────────────────────────

// Format: <dev>_<type_name>_<chunk_id>
// e.g.    "Index_rawblk_42"
std::string BlobDev::chunk_mblk_name(StreamType type, uint32_t chunk_id) const {
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
    return fmt::format("{}{}{}", dev_name_, type_name, chunk_id);
}

std::optional< std::pair< StreamType, uint32_t > > BlobDev::parse_chunk_mblk_name(std::string_view dev_name,
                                                                                  std::string_view name) {
    // Each name has the form: <dev_name>_<type_name>_<chunk_id>
    // Try each known suffix.
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

        // Remainder is "<chunk_id>"
        std::string_view rest = name.substr(prefix.size());

        uint32_t chunk_id{};
        auto res = std::from_chars(rest.data(), rest.data() + rest.size(), chunk_id);
        if (res.ec != std::errc{}) {
            continue;
        }

        return std::make_pair(type, chunk_id);
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// CP lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void BlobDev::on_cp_switchover(CP* cur_cp, CP* new_cp) {
    if (raw_blk_) {
        raw_blk_->on_cp_switchover(cur_cp, new_cp);
    }
    if (append_blk_) {
        append_blk_->on_cp_switchover(cur_cp, new_cp);
    }
    if (append_byte_) {
        append_byte_->on_cp_switchover(cur_cp, new_cp);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream creation
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< RawBlkStream > > BlobDev::create_raw_blk_stream(uint64_t chunk_size) {
    if (raw_blk_) {
        co_return raw_blk_;
    }
    raw_blk_ = co_await RawBlkStream::create(meta_client_, dev_name_, vdev_, chunk_size);
    co_return raw_blk_;
}

folly::coro::Task< shared< AppendBlkStream > > BlobDev::create_append_blk_stream(uint64_t chunk_size) {
    if (append_blk_) {
        co_return append_blk_;
    }
    append_blk_ = co_await AppendBlkStream::create(meta_client_, dev_name_, vdev_, chunk_size);
    co_return append_blk_;
}

folly::coro::Task< shared< AppendByteStream > > BlobDev::create_append_byte_stream(uint64_t chunk_size) {
    if (append_byte_) {
        co_return append_byte_;
    }
    append_byte_ = co_await AppendByteStream::create(meta_client_, dev_name_, vdev_, chunk_size);
    co_return append_byte_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recovery load
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDev::load(ChunkMblkMap&& raw_blk, ChunkMblkMap&& append_blk,
                                        ChunkMblkMap&& append_byte) {
    if (!raw_blk.empty()) {
        raw_blk_ = co_await RawBlkStream::load(meta_client_, dev_name_, vdev_, std::move(raw_blk));
    }
    if (!append_blk.empty()) {
        append_blk_ = co_await AppendBlkStream::load(meta_client_, dev_name_, vdev_, std::move(append_blk));
    }
    if (!append_byte.empty()) {
        append_byte_ = co_await AppendByteStream::load(meta_client_, dev_name_, vdev_, std::move(append_byte));
    }
    co_await reconcile_chunks();
}

// ─────────────────────────────────────────────────────────────────────────────
// reconcile_chunks
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDev::reconcile_chunks() {
    // Build the set of chunk_ids claimed by all loaded streams.
    std::unordered_set< uint32_t > claimed;
    auto collect = [&](const auto* stream) {
        if (!stream) {
            return;
        }
        auto acc = stream->chunks();
        for (auto& chunk : *acc) {
            claimed.insert(chunk->chunk_id());
        }
    };
    collect(raw_blk_.get());
    collect(append_blk_.get());
    collect(append_byte_.get());

    // Any chunk in the VDev that is not claimed is orphaned.
    auto all_chunks = vdev_->get_chunks();
    for (auto& chunk : all_chunks) {
        if (!claimed.count(chunk->chunk_id())) {
            co_await vdev_->shrink(ChunkToShrink::Specific, chunk->chunk_id());
        }
    }
}

} // namespace homestore
