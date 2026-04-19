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

#include "common/defs.h"
#include "device/hs_super_blk.h" // ChunkInfo

namespace homestore {

// ── Forward declarations ──────────────────────────────────────────────────────
class PhysicalDev;
namespace blkalloc { class BlkAllocator; }

// ── Interval types ────────────────────────────────────────────────────────────
using ChunkIntervalSet = boost::icl::split_interval_set< uint64_t >;
using ChunkInterval = ChunkIntervalSet::interval_type;

class Chunk {
public:
    static constexpr uint32_t MAX_CHUNK_SIZE = std::numeric_limits< uint32_t >::max();

    Chunk(ChunkInfo info, uint32_t chunk_slot, shared< PhysicalDev > pdev);
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = delete;
    Chunk& operator=(Chunk&&) = delete;
    ~Chunk() = default;

    // ── Physical device ───────────────────────────────────────────────────────
    cshared< PhysicalDev >& physical_dev() const { return pdev_; }

    // ── ChunkInfo access ──────────────────────────────────────────────────────
    const ChunkInfo& info() const { return chunk_info_; }

    /// In-place update used during deactivate/reactivate from pool.
    /// Caller must hold the appropriate higher-level lock (VirtualDev's mutex).
    void update_info(const ChunkInfo& new_info) { chunk_info_ = new_info; }

    // ── Getters ────────────────────────────────
    uint64_t start_offset() const { return chunk_info_.chunk_start_offset; }
    uint64_t size() const { return chunk_info_.chunk_size; }
    uint32_t vdev_id() const { return chunk_info_.vdev_id; }
    uint32_t chunk_id() const { return chunk_info_.chunk_id; }
    uint64_t vdev_order() const { return chunk_info_.chunk_vdev_order; }
    uint64_t stream_id() const { return chunk_info_.stream_id; }
    uint32_t slot_number() const { return chunk_slot_; }
    bool is_busy() const { return chunk_info_.is_allocated(); }

    // ── Alignment check  ──────────────────────────
    bool is_aligned(uint32_t align) const {
        return (chunk_info_.chunk_start_offset % align == 0) && (chunk_info_.chunk_size % align == 0);
    }

    const uint8_t* user_private() const { return chunk_info_.user_private; }

    // ── Block allocator ───────────────────────────────────────────────────────
    void set_block_allocator(shared< blkalloc::BlkAllocator > alloc) { blk_allocator_ = std::move(alloc); }
    bool has_blk_allocator() const { return blk_allocator_ != nullptr; }
    const blkalloc::BlkAllocator* blk_allocator() const { return blk_allocator_.get(); }
    blkalloc::BlkAllocator* blk_allocator_mutable() { return blk_allocator_.get(); }

    // ── String / debug ────────────────────────────────────────────────────────
    std::string to_string() const;

private:
    ChunkInfo chunk_info_;
    const uint32_t chunk_slot_;
    shared< PhysicalDev > pdev_;
    shared< blkalloc::BlkAllocator > blk_allocator_;
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
    bool has_room(uint64_t chunk_size) const {
        std::lock_guard lg{mutex_};
        auto it = pools_.find(chunk_size);
        return (it == pools_.end()) ? true : (it->second.size() < pool_limit_);
    }

    // Return a deactivated chunk to the pool.
    // Caller must have already called PhysicalDev::deactivate_chunk() first.
    void return_chunk(shared< Chunk > chunk) {
        const uint64_t sz = chunk->size();
        std::lock_guard lg{mutex_};
        pools_[sz].push_back(std::move(chunk));
    }

    // Try to pop a chunk of the requested size from the pool.
    // Returns nullptr if none available.
    shared< Chunk > try_get_chunk(uint64_t chunk_size) {
        std::lock_guard lg{mutex_};
        auto it = pools_.find(chunk_size);
        if (it == pools_.end() || it->second.empty()) {
            return nullptr;
        }
        auto chunk = std::move(it->second.back());
        it->second.pop_back();
        return chunk;
    }

    // Number of pooled chunks available for chunk_size.
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
