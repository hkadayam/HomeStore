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
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/icl/split_interval_set.hpp>

#include "homestore/crc.h"
#include "homestore/homestore_decl.hpp" // hs_init_crc_16
#include "common/defs.h"

namespace homestore {

// ── Forward declarations ──────────────────────────────────────────────────────
class PhysicalDev;
class BlkAllocator; // TODO: forward-declared until blkalloc is ported to new_device/

// ── ChunkInfo ─────────────────────────────────────────────────────────────────
// On-disk structure for chunk metadata.  Binary layout mirrors Rust's ChunkInfo
// (#[repr(C, packed)]) exactly, including the stream_id field added in the Rust
// version.
//
// Field offsets (all packed, no implicit padding):
//   0   chunk_start_offset  u64  (8)
//   8   chunk_size          u64  (8)
//  16   vdev_id             u32  (4)
//  20   chunk_id            u32  (4)
//  24   chunk_creation_order u32 (4)
//  28   stream_id           u64  (8)
//  36   chunk_allocated     u8   (1)
//  37   checksum            u16  (2)
//  39   padding             u8[281]
// 320   chunk_selector_private u8[64]
// 384   user_private        u8[128]
// 512   = ChunkInfo::SIZE   ✓
#pragma pack(1)
struct ChunkInfo {
    static constexpr size_t SIZE = 512;
    static constexpr size_t USER_PRIVATE_SIZE = 128;
    static constexpr size_t SELECTOR_PRIVATE_SIZE = 64;

    uint64_t chunk_start_offset{0};                          //   0: start offset within pdev
    uint64_t chunk_size{0};                                  //   8: size of this chunk
    uint32_t vdev_id{0};                                     //  16: owning vdev (UINT32_MAX = free)
    uint32_t chunk_id{0};                                    //  20: system-wide unique chunk id
    uint32_t chunk_creation_order{0};                        //  24: sequential creation order in vdev
    uint64_t stream_id{0};                                   //  28: stream id (0 = unassigned/default)
    uint8_t chunk_allocated{0x00};                           //  36: 0x01 = allocated, 0x00 = free
    uint16_t checksum{0};                                    //  37: CRC16 of entire ChunkInfo
    uint8_t padding[281]{};                                  //  39
    uint8_t chunk_selector_private[SELECTOR_PRIVATE_SIZE]{}; // 320
    uint8_t user_private[USER_PRIVATE_SIZE]{};               // 384

    // ── Accessors ─────────────────────────────────────────────────────────────
    bool is_allocated() const { return chunk_allocated != 0x00; }
    void set_allocated() { chunk_allocated = 0x01; }
    void set_free() { chunk_allocated = 0x00; }

    bool has_stream() const { return stream_id != 0; }
    uint64_t get_stream_id() const { return stream_id; }
    void set_stream_id(uint64_t sid) { stream_id = sid; }

    void set_selector_private(const uint8_t* data, size_t len) {
        if (data && len > 0) { std::memcpy(chunk_selector_private, data, std::min(len, SELECTOR_PRIVATE_SIZE)); }
    }

    void set_user_private(const uint8_t* data, size_t len) {
        if (data && len > 0) { std::memcpy(user_private, data, std::min(len, USER_PRIVATE_SIZE)); }
    }

    void compute_checksum() {
        checksum = 0;
        checksum = crc16_t10dif(hs_init_crc_16, reinterpret_cast< const unsigned char* >(this), sizeof(ChunkInfo));
    }

    // Raw-byte view of this struct (mirrors Rust's to_bytes()).
    const uint8_t* to_bytes() const { return reinterpret_cast< const uint8_t* >(this); }
};
#pragma pack()

static_assert(sizeof(ChunkInfo) == ChunkInfo::SIZE, "ChunkInfo size mismatch");

// ── Interval types ────────────────────────────────────────────────────────────
using ChunkIntervalSet = boost::icl::split_interval_set< uint64_t >;
using ChunkInterval = ChunkIntervalSet::interval_type;

class Chunk {
public:
    static constexpr uint32_t MAX_CHUNK_SIZE = std::numeric_limits< uint32_t >::max();

    // Constructor mirrors Rust's Chunk::new(chunk_info, chunk_slot, pdev).
    Chunk(ChunkInfo info, uint32_t chunk_slot, shared< PhysicalDev > pdev);

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = delete;
    Chunk& operator=(Chunk&&) = delete;
    ~Chunk() = default;

    // ── Physical device ───────────────────────────────────────────────────────
    const shared< PhysicalDev >& physical_dev() const { return pdev_; }

    // ── ChunkInfo access ──────────────────────────────────────────────────────
    const ChunkInfo& info() const { return chunk_info_; }

    /// In-place update used during deactivate/reactivate from pool.
    /// Caller must hold the appropriate higher-level lock (VirtualDev's mutex).
    void update_info(const ChunkInfo& new_info) { chunk_info_ = new_info; }

    // ── Getters (mirrors Rust's Chunk methods) ────────────────────────────────
    uint64_t start_offset() const { return chunk_info_.chunk_start_offset; }
    uint64_t size() const { return chunk_info_.chunk_size; }
    uint32_t vdev_id() const { return chunk_info_.vdev_id; }
    uint32_t chunk_id() const { return chunk_info_.chunk_id; }
    uint32_t creation_order() const { return chunk_info_.chunk_creation_order; }
    uint64_t stream_id() const { return chunk_info_.stream_id; }
    uint32_t slot_number() const { return chunk_slot_; }
    bool is_busy() const { return chunk_info_.is_allocated(); }

    // ── Alignment check (C++ extension, not in Rust) ──────────────────────────
    bool is_aligned(uint32_t align) const {
        return (chunk_info_.chunk_start_offset % align == 0) && (chunk_info_.chunk_size % align == 0);
    }

    const uint8_t* user_private() const { return chunk_info_.user_private; }

    // ── Block allocator ───────────────────────────────────────────────────────
    void set_block_allocator(shared< BlkAllocator > alloc) { blk_allocator_ = std::move(alloc); }
    bool has_blk_allocator() const { return blk_allocator_ != nullptr; }
    const BlkAllocator* blk_allocator() const { return blk_allocator_.get(); }
    BlkAllocator* blk_allocator_mutable() { return blk_allocator_.get(); }

    // ── String / debug ────────────────────────────────────────────────────────
    std::string to_string() const;

private:
    ChunkInfo chunk_info_;
    const uint32_t chunk_slot_;
    shared< PhysicalDev > pdev_;
    shared< BlkAllocator > blk_allocator_;
};

// ── ChunkPool ─────────────────────────────────────────────────────────────────
//
// Holds a free-list of deactivated Chunks keyed by chunk_size so they can be
// reused without going through the full create/remove disk cycle.
//
//
// Uses std::mutex (not folly::coro::Mutex) because all operations are short
// critical sections with no I/O inside
class ChunkPool {
public:
    explicit ChunkPool(size_t pool_limit) : pool_limit_{pool_limit} {}

    // Returns true if the pool for chunk_size has room for at least one more chunk.
    // Mirrors Rust's ChunkPool::has_room().
    bool has_room(uint64_t chunk_size) const {
        std::lock_guard lg{mutex_};
        auto it = pools_.find(chunk_size);
        return (it == pools_.end()) ? true : (it->second.size() < pool_limit_);
    }

    // Return a deactivated chunk to the pool.
    // Caller must have already called PhysicalDev::deactivate_chunk() first.
    // Mirrors Rust's ChunkPool::return_chunk().
    void return_chunk(shared< Chunk > chunk) {
        const uint64_t sz = chunk->size();
        std::lock_guard lg{mutex_};
        pools_[sz].push_back(std::move(chunk));
    }

    // Try to pop a chunk of the requested size from the pool.
    // Returns nullptr if none available.
    // Mirrors Rust's ChunkPool::try_get_chunk().
    shared< Chunk > try_get_chunk(uint64_t chunk_size) {
        std::lock_guard lg{mutex_};
        auto it = pools_.find(chunk_size);
        if (it == pools_.end() || it->second.empty()) { return nullptr; }
        auto chunk = std::move(it->second.back());
        it->second.pop_back();
        return chunk;
    }

    // Number of pooled chunks available for chunk_size.
    // Mirrors Rust's ChunkPool::available_count().
    size_t available_count(uint64_t chunk_size) const {
        std::lock_guard lg{mutex_};
        auto it = pools_.find(chunk_size);
        return (it == pools_.end()) ? 0u : it->second.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map< uint64_t, std::vector< shared< Chunk > > > pools_;
    size_t pool_limit_;
};

} // namespace homestore
