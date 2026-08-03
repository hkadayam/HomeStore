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

#include "common/async.h"

#include "homestore/base/blk.h"              // BlkId, BlkAllocStatus, blk_alloc_hints
#include "homestore/base/crc.h"              // crc32_ieee
#include "common/defs.h"                     // shared<>, unique<>, to_u32
#include "homestore/base/homestore_assert.h" // HS_SUBMOD_LOG

#include "sisl/fds/buffer.h" // IoBufShared, make_io_buf_shared

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
        // Silent truncation here is a debugging nightmare — multiple metablks end up sharing the same on-disk
        // name, parse_mblk_name fails on every one of them, and recovery surfaces zero metablks with no obvious
        // cause.  Trip in debug builds so callers find this immediately.
        HS_DBG_ASSERT_LT(name_sv.size(), sizeof(MetaBlkHeader::name),
                         "MetaBlk name '{}' is too long ({} bytes); max {}", name_sv, name_sv.size(),
                         sizeof(MetaBlkHeader::name) - 1);
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
// MetaBlkHolder
//
// The single in-memory instance of one meta block's state. Both the copy in
// MetaClient's map and the copy held by the consumer point at one holder, so the
// chain linkage (prev_bid) and the cached bytes (buffer) are shared — never
// duplicated, and therefore never able to diverge.
//
// prev_bid is kept in memory only (not persisted in the header) to support O(1)
// removal; the on-disk chain is singly-linked (only next_bid, in the header).
// ──────────────────────────────────────────────────────────────────────────────
struct MetaBlkHolder {
    BlkId blkid{};            // Block ID on the vdev
    BlkId prev_bid{};         // Previous block in chain (in-memory only)
    sisl::IoBufShared buffer; // Exactly one block: header (64 B) + inline data (shared ownership)
    bool linked{false};       // false until the first write appends this block into the client's chain
};

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk
//
// A lightweight handle over a shared MetaBlkHolder. Copying a MetaBlk shares the
// holder (a refcount bump, no buffer memcpy), so every reference to one block
// observes a single authoritative state. The static_assert below forbids adding
// any per-copy field that would reintroduce divergence.
// ──────────────────────────────────────────────────────────────────────────────
class MetaBlk {
public:
    // ── Factory ──────────────────────────────────────────────────────────────
    static MetaBlk create(BlkId blkid, uint32_t blk_sz, std::string_view name) {
        auto holder = std::make_shared< MetaBlkHolder >();
        holder->blkid = blkid;
        holder->buffer = sisl::make_io_buf_shared(blk_sz);
        MetaBlkHeader hdr = MetaBlkHeader::make(name);
        std::memcpy(holder->buffer->bytes(), &hdr, MetaBlkHeader::SIZE);
        return MetaBlk{std::move(holder)};
    }

    static MetaBlk load(BlkId blkid, BlkId prev_bid, sisl::IoBufShared buf) {
        auto holder = std::make_shared< MetaBlkHolder >();
        holder->blkid = blkid;
        holder->prev_bid = prev_bid;
        holder->buffer = std::move(buf);
        holder->linked = true; // a recovered block is already part of the chain
        return MetaBlk{std::move(holder)};
    }

    // ── Handle / topology state (all forwarded to the shared holder) ──────────
    bool valid() const { return holder_ != nullptr; }

    BlkId blkid() const { return holder_->blkid; }
    BlkId prev_bid() const { return holder_->prev_bid; }
    void set_prev_bid(BlkId prev) { holder_->prev_bid = prev; }
    bool linked() const { return holder_->linked; }
    void set_linked(bool v) { holder_->linked = v; }

    // ── Accessors ────────────────────────────────────────────────────────────
    MetaBlkHeader& header() { return *r_cast< MetaBlkHeader* >(holder_->buffer->bytes()); }
    const MetaBlkHeader& header() const { return *r_cast< const MetaBlkHeader* >(holder_->buffer->cbytes()); }

    std::string name() const { return header().get_name(); }

    /// Inline data region: everything after the header in the single cached block.
    uint8_t* inline_data() { return holder_->buffer->bytes() + MetaBlkHeader::SIZE; }
    const uint8_t* inline_data() const { return holder_->buffer->cbytes() + MetaBlkHeader::SIZE; }

    size_t max_inline_data_size() const { return holder_->buffer->size() - MetaBlkHeader::SIZE; }

    static uint32_t data_size_to_nblks(size_t data_size, size_t block_size) {
        return to_u32((data_size + MetaBlkHeader::SIZE + block_size - 1) / block_size);
    }

    // ── Public async I/O ─────────────────────────────────────────────────────

    /// Write payload to disk. Stores inline if it fits in one block, allocates overflow blocks otherwise. Updates
    /// data_size/data_crc in the header, writes the block, then frees any previous overflow block.
    Async< void > write_data(const sisl::IoBufShared& data, VirtualDev& vdev);

    /// Read the payload. Returns a IoBufView into the cached buffer for inline data (zero copy, zero I/O) or reads
    /// overflow blocks from disk into a new IoBufShared and wraps it in a IoBufView.
    Async< sisl::IoBufView > read_data(VirtualDev& vdev) const;

    /// Free this block (and any overflow blocks) on the vdev.
    Async< void > free(VirtualDev& vdev);

    // ── Handle: default = empty (null); copies/moves share the holder ─────────
    MetaBlk() = default;
    MetaBlk(const MetaBlk&) = default;
    MetaBlk& operator=(const MetaBlk&) = default;
    MetaBlk(MetaBlk&&) = default;
    MetaBlk& operator=(MetaBlk&&) = default;

private:
    friend class MetaClient;
    explicit MetaBlk(shared< MetaBlkHolder > holder) : holder_{std::move(holder)} {}

    /// Update next_bid in the on-disk header and write the cached block back to disk.
    Async< void > update_next_bid(BlkId next, VirtualDev& vdev);

    shared< MetaBlkHolder > holder_;
};

static_assert(sizeof(MetaBlk) == sizeof(shared< MetaBlkHolder >),
              "MetaBlk must hold nothing but its shared holder; any extra field reintroduces per-copy state that "
              "can diverge from the map's copy");

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkWrapper
//
// Convenience pair of (MetaBlk, MetaClient) so callers don't need to thread
// both objects through every I/O call.
// ──────────────────────────────────────────────────────────────────────────────
class MetaBlkWrapper {
public:
    /// Allocate a new MetaBlk through the given client.
    static Async< MetaBlkWrapper > create(shared< MetaClient > client, std::string_view name,
                                          std::optional< size_t > estimated_data_size);

    /// Wrap an already-loaded MetaBlk.
    static MetaBlkWrapper load(shared< MetaClient > client, MetaBlk blk) {
        MetaBlkWrapper w;
        w.meta_blk_ = std::move(blk);
        w.client_ = std::move(client);
        return w;
    }

    Async< void > write(const uint8_t* data, size_t len);
    Async< sisl::IoBufView > read();

    /// Unlink this MetaBlk from the client's chain and free its blocks on the vdev.  After destroy() the
    /// wrapper's meta_blk_ is stale — the caller must not perform further I/O through this wrapper.
    Async< void > destroy();

    const MetaBlk& meta_blk() const { return meta_blk_; }
    MetaBlk& meta_blk() { return meta_blk_; }
    shared< MetaClient > meta_client() const { return client_; }

private:
    MetaBlk meta_blk_;
    shared< MetaClient > client_;

public:
    // Empty "unattached" wrapper.  Normal instances come from create()/load(); this exists so an owner can hold
    // a MetaBlkWrapper member that is populated later by move-assignment (e.g. ReplicaSet::raft_cfg_mblk_).
    MetaBlkWrapper() = default;
};

} // namespace homestore
