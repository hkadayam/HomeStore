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

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <folly/coro/Task.h>

#include <homestore/homestore_decl.hpp> // shared<>, unique<>
#include <homestore/checkpoint/cp.h>    // CP

#include "meta/meta_blk.h"             // MetaBlk

namespace homestore {

class Chunk;
class MetaClient;
class VirtualDev;
class RawBlkStream;
class AppendBlkStream;
class AppendByteStream;
class CPManager;

// ─────────────────────────────────────────────────────────────────────────────
// StreamType
//
// Fixed stream identifier stored in ChunkInfo::stream_id. Each BlobDev has at most one instance per type.
// ─────────────────────────────────────────────────────────────────────────────
VENUM(StreamType, uint64_t, RawBlk = 1, AppendBlk = 2, AppendByte = 3);

// ─────────────────────────────────────────────────────────────────────────────
// BlobDev
//
// 1:1 with VirtualDev.  Holds at most one stream of each type as shared<T> (nullptr if the stream has not been
// created).  All streams share the single MetaClient owned by BlobDevManager — accessed via reference.
//
// Stream naming convention for per-chunk MetaBlks:
//   RawBlkStream    → "<dev>_rawblk_<chunk_id>"
//   AppendBlkStream → "<dev>_appendblk_<chunk_id>"
//   AppendByteStream→ "<dev>_appendbyte_<chunk_id>"
// ─────────────────────────────────────────────────────────────────────────────
class BlobDev {
public:
    // ── Creation ─────────────────────────────────────────────────────────────

    /// Construct a BlobDev backed by a VirtualDev, referencing the manager's MetaClient.
    BlobDev(std::string dev_name, shared< VirtualDev > vdev, MetaClient& meta_client);

    BlobDev(const BlobDev&) = delete;
    BlobDev& operator=(const BlobDev&) = delete;
    BlobDev(BlobDev&&) = delete;
    BlobDev& operator=(BlobDev&&) = delete;
    ~BlobDev();

    // ── Per-stream create (called once per type) ──────────────────────────────

    /// Create a fresh stream.
    folly::coro::Task< shared< RawBlkStream > > create_raw_blk_stream(uint64_t chunk_size);
    folly::coro::Task< shared< AppendBlkStream > > create_append_blk_stream(uint64_t chunk_size);
    folly::coro::Task< shared< AppendByteStream > > create_append_byte_stream(uint64_t chunk_size);

    // ── Recovery load ─────────────────────────────────────────────────────────

    /// Load previously-persisted streams from recovered data (chunk_id → MetaBlk + payload).
    /// Empty map = stream not present.
    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, IOBuffer > >;
    folly::coro::Task< void > load(ChunkMblkMap&& raw_blk, ChunkMblkMap&& append_blk, ChunkMblkMap&& append_byte);

    /// After all streams are loaded, remove any VDev chunks not owned by any stream (orphans left by a crash before
    /// MetaBlk was written).
    folly::coro::Task< void > reconcile_chunks();

    // ── Stream accessors (nullptr if not created) ─────────────────────────────
    shared< RawBlkStream > raw_blk_stream() const { return raw_blk_; }
    shared< AppendBlkStream > append_blk_stream() const { return append_blk_; }
    shared< AppendByteStream > append_byte_stream() const { return append_byte_; }

    // ── CP lifecycle ──────────────────────────────────────────────────────────

    /// Called by BlobDevManager::on_switchover_cp.  Propagates the switchover to all present streams.
    void on_cp_switchover(CP* cur_cp, CP* new_cp);

    // ── Device accessors ──────────────────────────────────────────────────────
    VirtualDev& vdev() const;
    const std::string& name() const { return dev_name_; }

    // ── MetaBlk name helpers (public for use by BlobDevManager) ───────────

    /// Build the per-chunk MetaBlk name for the given stream type and chunk id.
    std::string chunk_mblk_name(StreamType type, uint32_t chunk_id) const;

    /// Parse "<dev>_<type_name>_<chunk_id>" and return (stream_type, chunk_id). Returns nullopt if the name does not
    /// match this device.
    static std::optional< std::pair< StreamType, uint32_t > > parse_chunk_mblk_name(std::string_view dev_name,
                                                                                    std::string_view name);

private:
    std::string dev_name_;
    shared< VirtualDev > vdev_;
    MetaClient& meta_client_;
    shared< RawBlkStream > raw_blk_;
    shared< AppendBlkStream > append_blk_;
    shared< AppendByteStream > append_byte_;
};

} // namespace homestore