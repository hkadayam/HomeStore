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
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <folly/coro/Task.h>

#include <homestore/blk.h>              // BlkId, BlkAllocStatus, blk_alloc_hints
#include <homestore/crc.h>              // crc32_ieee
#include "common/defs.h"                // shared<>, unique<>, to_u32
#include "base/homestore_assert.hpp"    // HS_SUBMOD_LOG

#include "iomanager/drive_interface.hpp" // IOBuffer
#include <sisl/fds/buffer.h>            // ByteArray, make_byte_array

namespace homestore {

#define META_LOG(level, msg, ...) HS_SUBMOD_LOG(level, metablk, , "metablk", "meta", msg, ##__VA_ARGS__)

// ──────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ──────────────────────────────────────────────────────────────────────────────
class MetaClient;
class VirtualDev;

// ──────────────────────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t META_BLK_HEADER_MAGIC = 0xABCD5678u;
static constexpr size_t META_BLK_HEADER_SIZE = 64;

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkHeader
//
// On-disk header stored at byte 0 of every meta block. Exactly 64 bytes so
// that user data always begins at a clean, known offset.
// ──────────────────────────────────────────────────────────────────────────────
#pragma pack(1)
struct MetaBlkHeader {
    uint32_t magic{0};     // META_BLK_HEADER_MAGIC
    uint32_t data_size{0}; // Size of the payload (inline or in overflow blocks)
    uint32_t data_crc{0};  // CRC32 of the payload bytes
    BlkId next_bid{};      // Next block in the client's chain (invalid = last)
    BlkId overflow_bid{};  // Overflow block for large payloads (invalid = inlined)
    char name[32]{};       // Name of this meta block (null-terminated)
    uint8_t pad[4]{};      // Padding — 4+4+4+8+8+32+4 = 64 bytes total

    static constexpr size_t SIZE = META_BLK_HEADER_SIZE;

    static MetaBlkHeader make(std::string_view name_sv) {
        MetaBlkHeader h;
        h.magic = META_BLK_HEADER_MAGIC;
        h.data_size = 0;
        h.data_crc = 0;
        h.next_bid = BlkId{};
        h.overflow_bid = BlkId{};
        size_t copy_len = std::min(name_sv.size(), sizeof(h.name) - 1);
        std::memcpy(h.name, name_sv.data(), copy_len);
        h.name[copy_len] = '\0';
        return h;
    }

    bool is_valid() const { return magic == META_BLK_HEADER_MAGIC; }

    std::string get_name() const {
        size_t end = 0;
        while (end < sizeof(name) && name[end] != '\0')
            ++end;
        return std::string(name, end);
    }
};
#pragma pack()

static_assert(sizeof(MetaBlkHeader) == META_BLK_HEADER_SIZE,
              "MetaBlkHeader must be exactly META_BLK_HEADER_SIZE bytes");

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk
//
// One metadata block owned by a MetaClient. Caches exactly one disk block
// (header + inline data) in a ByteArray (shared<IoBlobSafe>). Overflow data
// lives in separate blocks referenced by overflow_bid — never cached here.
//
// prev_bid is kept in memory only (not persisted in the header) to support
// O(1) removal from the doubly-linked chain.
//
// Copyable via shared_ptr refcount bump — no buffer memcpy on copy.
// ──────────────────────────────────────────────────────────────────────────────
class MetaBlk {
public:
    BlkId blkid{};              // Block ID on the vdev
    BlkId prev_bid{};           // Previous block in chain (in-memory only)
    sisl::ByteArray buffer;     // Exactly one block: header (64 B) + inline data (shared ownership)
    bool is_fresh{true};        // true until first write into the client's chain

    // ── Factory ──────────────────────────────────────────────────────────────
    static MetaBlk create(BlkId blkid, uint32_t blk_sz, std::string_view name) {
        MetaBlk blk;
        blk.blkid = blkid;
        blk.buffer = sisl::make_byte_array(blk_sz);
        blk.is_fresh = true;
        MetaBlkHeader hdr = MetaBlkHeader::make(name);
        std::memcpy(blk.buffer->bytes(), &hdr, MetaBlkHeader::SIZE);
        return blk;
    }

    static MetaBlk load(BlkId blkid, BlkId prev_bid, sisl::ByteArray buf) {
        MetaBlk blk;
        blk.blkid = blkid;
        blk.prev_bid = prev_bid;
        blk.buffer = std::move(buf);
        blk.is_fresh = false;
        return blk;
    }

    // ── Accessors ────────────────────────────────────────────────────────────
    MetaBlkHeader& header() { return *reinterpret_cast< MetaBlkHeader* >(buffer->bytes()); }
    const MetaBlkHeader& header() const { return *reinterpret_cast< const MetaBlkHeader* >(buffer->cbytes()); }

    std::string name() const { return header().get_name(); }

    /// Inline data region: everything after the header in the single cached block.
    uint8_t* inline_data() { return buffer->bytes() + MetaBlkHeader::SIZE; }
    const uint8_t* inline_data() const { return buffer->cbytes() + MetaBlkHeader::SIZE; }

    size_t max_inline_data_size() const { return buffer->size() - MetaBlkHeader::SIZE; }

    static uint32_t data_size_to_nblks(size_t data_size, size_t block_size) {
        return to_u32((data_size + MetaBlkHeader::SIZE + block_size - 1) / block_size);
    }

    // ── Public async I/O ─────────────────────────────────────────────────────

    /// Write payload to disk. Stores inline if it fits in one block, allocates overflow blocks otherwise. Updates
    /// data_size/data_crc in the header, writes the block, then frees any previous overflow block.
    folly::coro::Task< void > write_data(const sisl::ByteArray& data, VirtualDev& vdev);

    /// Read the payload. Returns a ByteView into the cached buffer for inline data (zero copy, zero I/O) or reads
    /// overflow blocks from disk into a new ByteArray and wraps it in a ByteView.
    folly::coro::Task< sisl::ByteView > read_data(VirtualDev& vdev) const;

    /// Free this block (and any overflow blocks) on the vdev.
    folly::coro::Task< void > free(VirtualDev& vdev);

    // ── Copyable (shared_ptr refcount bump), movable ─────────────────────────
    MetaBlk() = default;
    MetaBlk(const MetaBlk&) = default;
    MetaBlk& operator=(const MetaBlk&) = default;
    MetaBlk(MetaBlk&&) = default;
    MetaBlk& operator=(MetaBlk&&) = default;

private:
    friend class MetaClient;

    /// Update next_bid in the on-disk header and write the cached block back to disk.
    folly::coro::Task< void > update_next_bid(BlkId next, VirtualDev& vdev);
};

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkWrapper
//
// Convenience pair of (MetaBlk, MetaClient) so callers don't need to thread
// both objects through every I/O call.
// ──────────────────────────────────────────────────────────────────────────────
class MetaBlkWrapper {
public:
    /// Allocate a new MetaBlk through the given client.
    static folly::coro::Task< MetaBlkWrapper > create(shared< MetaClient > client, std::string_view name,
                                                      std::optional< size_t > estimated_data_size);

    /// Wrap an already-loaded MetaBlk.
    static MetaBlkWrapper load(shared< MetaClient > client, MetaBlk blk) {
        MetaBlkWrapper w;
        w.meta_blk_ = std::move(blk);
        w.client_ = std::move(client);
        return w;
    }

    folly::coro::Task< void > write(const uint8_t* data, size_t len);
    folly::coro::Task< sisl::ByteView > read();

    const MetaBlk& meta_blk() const { return meta_blk_; }
    shared< MetaClient > meta_client() const { return client_; }

private:
    MetaBlk meta_blk_;
    shared< MetaClient > client_;

    MetaBlkWrapper() = default;
};

} // namespace homestore