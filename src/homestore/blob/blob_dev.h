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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <folly/SharedMutex.h>
#include "common/async.h"

#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include "homestore/checkpoint/cp.h"       // CP

#include "homestore/meta/meta_blk.h" // MetaBlk

namespace homestore {

class Chunk;
class MetaClient;
class VirtualDev;
class StreamBase;
class RawBlkStream;
class AppendBlkStream;
class AppendByteStream;
class CPManager;

// ─────────────────────────────────────────────────────────────────────────────
// StreamType
//
// Identifies the kind of stream.
// ─────────────────────────────────────────────────────────────────────────────
VENUM(StreamType, uint64_t, RawBlk = 1, AppendBlk = 2, AppendByte = 3);

// ─────────────────────────────────────────────────────────────────────────────
// BlobDev
//
// 1:1 with VirtualDev.  Holds zero or more streams of each type, keyed by a
// BlobDev-assigned stream_id.  All streams share the single MetaClient owned
// by BlobDevManager — accessed via reference.
//
// Stream naming convention for per-chunk MetaBlks:
//   "<dev>_<type>_<stream_id>_<chunk_id>"
//   e.g. "Index_rawblk_3_42"
// ─────────────────────────────────────────────────────────────────────────────
class BlobDev {
public:
    // ── Types ────────────────────────────────────────────────────────────────

    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, sisl::IoBufView > >;

    struct StreamRecoveryInfo {
        uint32_t blk_size{0}; // 0 = use vdev default
        ChunkMblkMap chunk_mblks;
    };

    /// Per-stream recovery data grouped by stream_id (for per-chunk-mblk stream types: RawBlk, AppendBlk).
    using StreamMblkMap = std::map< uint64_t, StreamRecoveryInfo >;

    /// Per-stream recovery for AppendByteStream: a single sb MetaBlk + its payload per stream.  No per-chunk MetaBlks
    /// — chunk list lives in the sb payload.
    struct AppendByteSbInfo {
        MetaBlk sb;
        sisl::IoBufView payload;
    };
    using AppendByteSbMap = std::map< uint64_t, AppendByteSbInfo >;

    // ── Creation ─────────────────────────────────────────────────────────────

    /// Construct a BlobDev backed by a VirtualDev, referencing the manager's MetaClient.
    BlobDev(std::string dev_name, shared< VirtualDev > vdev, MetaClient& meta_client);

    BlobDev(const BlobDev&) = delete;
    BlobDev& operator=(const BlobDev&) = delete;
    BlobDev(BlobDev&&) = delete;
    BlobDev& operator=(BlobDev&&) = delete;
    ~BlobDev();

    // ── Per-stream create (auto-assigns stream_id) ───────────────────────────

    /// Create a fresh stream. Returns the new stream (use stream_id() to get the assigned id).
    Async< shared< RawBlkStream > > create_raw_blk_stream(uint64_t chunk_size, uint32_t blk_size = 0);
    Async< shared< AppendBlkStream > > create_append_blk_stream(uint64_t chunk_size, uint32_t blk_size = 0);
    Async< shared< AppendByteStream > > create_append_byte_stream(uint64_t chunk_size, bool concurrent_safe = true);

    // ── Recovery load ─────────────────────────────────────────────────────────

    /// Load previously-persisted streams from recovered data.
    /// RawBlk and AppendBlk use per-chunk MetaBlks grouped by stream_id (StreamMblkMap).
    /// AppendByte uses a single per-stream sb MetaBlk (AppendByteSbMap).
    Async< void > load(StreamMblkMap&& raw_blk, StreamMblkMap&& append_blk, AppendByteSbMap&& append_byte);

    /// After all streams are loaded, remove any VDev chunks not owned by any stream (orphans left by a crash before
    /// MetaBlk was written).
    Async< void > reconcile_chunks();

    // ── Stream accessors (returns a snapshot under shared lock) ────────────

    std::vector< shared< RawBlkStream > > raw_blk_streams() const;
    std::vector< shared< AppendBlkStream > > append_blk_streams() const;
    std::vector< shared< AppendByteStream > > append_byte_streams() const;

    shared< RawBlkStream > get_raw_blk_stream(uint64_t stream_id) const;
    shared< AppendBlkStream > get_append_blk_stream(uint64_t stream_id) const;
    shared< AppendByteStream > get_append_byte_stream(uint64_t stream_id) const;

    /// Destroy one stream of the given type: run its own destroy() (free chunks + remove its sb) and deregister it
    /// from this BlobDev so cp_flush no longer iterates it (which would re-persist an inconsistent, chunks-freed sb).
    /// No-op if the id isn't registered (e.g. the stream was never created).  The caller must also drop its own
    /// handle to the stream for the object itself to be freed.
    Async< void > destroy_raw_blk_stream(uint64_t stream_id);
    Async< void > destroy_append_blk_stream(uint64_t stream_id);
    Async< void > destroy_append_byte_stream(uint64_t stream_id);

    // ── CP lifecycle ──────────────────────────────────────────────────────────

    /// Flush dirty block-allocating streams (RawBlk, AppendBlk) for the given CP.
    Async< void > cp_flush(CP* cp);

    // ── Device accessors ──────────────────────────────────────────────────────
    VirtualDev& vdev() const;
    const std::string& name() const { return dev_name_; }

    // ── MetaBlk name helpers (public for use by BlobDevManager) ───────────

    /// Build the per-chunk MetaBlk name: "<dev>_<type>_<stream_id>_<chunk_id>".
    std::string chunk_mblk_name(StreamType type, uint64_t stream_id, uint32_t chunk_id) const;

    /// Parse "<dev>_<type>_<stream_id>_<chunk_id>" and return (stream_type, stream_id, chunk_id).
    /// Returns nullopt if the name does not match this device.
    struct ParsedChunkMblk {
        StreamType type;
        uint64_t stream_id;
        uint32_t chunk_id;
        uint32_t blk_size;
    };
    static std::optional< ParsedChunkMblk > parse_chunk_mblk_name(const std::string_view& dev_name,
                                                                  const std::string_view& name);

private:
    uint64_t next_stream_id() { return next_stream_id_.fetch_add(1, std::memory_order_relaxed); }

    std::string dev_name_;
    shared< VirtualDev > vdev_;
    MetaClient& meta_client_;
    std::atomic< uint64_t > next_stream_id_{0};

    mutable folly::SharedMutex streams_mutex_;
    std::map< uint64_t, shared< RawBlkStream > > raw_blk_streams_;
    std::map< uint64_t, shared< AppendBlkStream > > append_blk_streams_;
    std::map< uint64_t, shared< AppendByteStream > > append_byte_streams_;
};

} // namespace homestore
