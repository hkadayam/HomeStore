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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/coro/Task.h>
#include "sisl/fds/rcu.h"

#include "homestore/blk.h" // BlkId, BlkIds, BlkAllocStatus, blk_alloc_hints, blk_count_t

#include "iomanager/drive_interface.hpp" // IOBuffer
#include "device/hs_super_blk.h"         // VDevInfo, ChunkInfo, HSSuperBlk
#include "device/chunk.h"                // Chunk, ChunkPool
#include "device/chunk_selector.h"       // IChunkSelector, ChunkSelectorType, concrete selectors

namespace homestore {

#define VDEV_LOG(level, vdev, msg, ...) HS_SUBMOD_LOG(level, device, , "vdev", vdev, msg, ##__VA_ARGS__)

class PhysicalDev;
class BlkAllocator;

VENUM(MultiPDevOpts, uint8_t, AllPDevStriped = 0, AllPDevMirrored = 1, SingleFirstPDev = 2, SingleRandomPDev = 3);
VENUM(BlkAllocatorType, uint8_t, None = 0, SlabCompact = 1, SlabExtend = 2, Append = 3);

// Which chunk to remove during shrink.
ENUM(ChunkToShrink, uint8_t,
     Last,     // Remove the chunk with the highest vdev_order.
     Specific, // Remove a specific chunk by ID; pass specific_chunk_id to shrink().
);

// ── VDevParameters ────────────────────────────────────────────────────────────
// Creation parameters for a new VirtualDev.
struct VDevParameters {
    std::string vdev_name;
    uint64_t vdev_size{0};
    uint32_t num_chunks{0};
    uint64_t chunk_size{0};
    uint64_t incremental_chunk_size{0};
    uint32_t blk_size{4096};
    HSDevType dev_type{HSDevType::Data};
    MultiPDevOpts multi_pdev_opts{MultiPDevOpts::SingleFirstPDev};
    uint32_t num_mirrors{0};
    BlkAllocatorType alloc_type{BlkAllocatorType::SlabCompact};
    ChunkSelectorType chunk_sel_type{ChunkSelectorType::RoundRobin};
    bool use_slab_allocator{false};
    std::optional< size_t > chunk_pool_limit; // nullopt = no pooling; Some(n) = pool ≤ n per size
};

// ── VDevMutableState ──────────────────────────────────────────────────────────
// All mutable VDev state, bundled for clone-on-write (RCU). chunk_selector is rebuilt atomically together with the
// chunk set so reads always see a consistent (chunks, selector) pair with no extra locking.
struct VDevMutableState {
    VDevInfo vdev_info;
    std::unordered_set< uint32_t > pdevs;                       // pdev_ids in use
    std::unordered_map< uint32_t, shared< Chunk > > all_chunks; // chunk_id → Chunk (hot path)
    std::vector< shared< Chunk > > chunks_by_vdev_order;        // sorted (cold path)
    uint64_t total_chunk_num{0};
    uint64_t total_vdev_size{0};
    uint64_t next_vdev_order{0};             // monotonically increasing
    shared< IChunkSelector > chunk_selector; // rebuilt on every chunk-set change
};

// ── VirtualDev ────────────────────────────────────────────────────────────────
//
// Key design:
//  • RCU mutable state: sisl::Rcu::data<VDevMutableState> — reads are truly lock-free (atomic load + folly::rcu_reader
//    guard, ~2-5 ns),
//  • expand()/shrink() for adding/removing chunks at runtime.
//  • ChunkPool integration for efficient chunk reuse.
//  • Public constructor takes VDevInfo + pdevs; create()/load() are static factories.
//  • All I/O uses folly coroutines (Task<>)
class VirtualDev {
public:
    VirtualDev() = delete;
    VirtualDev(const VirtualDev&) = delete;
    VirtualDev& operator=(const VirtualDev&) = delete;
    ~VirtualDev() = default;

    // ──────────────────────────────────────────────────────────────────────────────
    // Constructors and Factory Methods (create/load)
    // ──────────────────────────────────────────────────────────────────────────────

    /// Single constructor: initialises immutable fields from VDevInfo and stores pdevs.
    /// Use the create() / load() static factories rather than calling this directly.
    VirtualDev(VDevInfo info, std::vector< shared< PhysicalDev > > pdevs);

    /// First-time creation: allocates chunks across pdevs and writes superblock metadata.
    static folly::coro::Task< unique< VirtualDev > > create(VDevParameters&& params, uint32_t vdev_id,
                                                            const std::vector< shared< PhysicalDev > >& pdevs);

    /// Recovery: constructs VDev from persisted VDevInfo. Caller should then call on_chunk_found() for each chunk
    /// (which atomically rebuilds the selector), then load_blk_allocator().
    static unique< VirtualDev > load(VDevInfo vinfo, std::vector< shared< PhysicalDev > > pdevs);

    /// Destroy the entire vdev and remove all its chunks and remove the vdev info. Upon completion next load
    /// will not have any trace of this vdev.
    folly::coro::Task< void > destroy();

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: Device Resizing section with chunks
    // ──────────────────────────────────────────────────────────────────────────────
    /// Expand: allocate one new chunk. Returns the new chunk.
    folly::coro::Task< shared< Chunk > > expand(uint64_t chunk_size);

    /// Shrink: remove a chunk from a vdev.
    /// Pooling enabled → deactivate + park in pool; disabled → permanently remove.
    /// Returns the removed chunk_id.
    folly::coro::Task< uint32_t > shrink(ChunkToShrink which, uint32_t specific_chunk_id = 0);

    /// Register one chunk with this vdev (recovery or post-create). Forwards to on_chunks_added().
    void on_chunk_added(cshared< Chunk >& chunk, bool newly_created);

    /// Register a batch of chunks. Active chunks enter all_chunks and rebuild the selector atomically;
    /// inactive chunks go to chunk_pool_. newly_created=true also constructs a fresh blk allocator per chunk.
    void on_chunks_added(const std::vector< shared< Chunk > >& chunks, bool newly_created);

    /// Enable chunk pooling for chunk reuse.
    void enable_chunk_pooling(size_t pool_limit);

    /// Get the nth chunk (0-indexed by vdev_order), creating it if needed.
    /// Returns (chunk, is_newly_created).
    folly::coro::Task< std::pair< shared< Chunk >, bool > > get_or_create_nth_chunk(size_t n);

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: I/Os
    // ──────────────────────────────────────────────────────────────────────────────
    folly::coro::Task< void > write(const IOBuffer& buf, const BlkId& bid);
    folly::coro::Task< void > writev(std::vector< IOBuffer >&& bufs, const BlkId& bid);
    folly::coro::Task< std::error_code > read(IOBuffer& buf, const BlkId& bid);
    folly::coro::Task< std::error_code > readv(std::vector< IOBuffer >& bufs, const BlkId& bid);
    folly::coro::Task< void > format();
    folly::coro::Task< void > fsync();

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: Block Allocations
    // ──────────────────────────────────────────────────────────────────────────────
    BlkAllocStatus alloc_contiguous_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkId& out_blkid);
    BlkAllocStatus alloc_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkIds& out_blkids);
    void free_blk(const BlkId& bid);
    BlkAllocStatus commit_blk(const BlkId& bid);
    void recovery_completed();

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: Getters
    // ──────────────────────────────────────────────────────────────────────────────
    shared< Chunk > get_nth_chunk(size_t n) const;
    shared< Chunk > get_chunk(uint32_t chunk_id) const; // O(1) lookup by chunk_id; nullptr if not found
    std::vector< shared< Chunk > > get_chunks() const;
    std::vector< shared< Chunk > > get_chunks_by_vdev_order() const;

    uint32_t vdev_id() const { return vdev_id_; }
    const std::string& name() const { return name_; }
    uint32_t block_size() const { return blk_size_; }
    HSDevType hs_dev_type() const { return hs_dev_type_; }
    BlkAllocatorType allocator_type() const { return allocator_type_; }
    ChunkSelectorType chunk_selector_type() const { return chunk_selector_type_; }
    uint64_t incremental_chunk_size() const { return incremental_chunk_size_; }
    uint64_t size() const;
    uint64_t num_chunks() const;

    // ── VDevInfo ──────────────────────────────────────────────────────────────

    /// Recompute vdev_size and num_primary_chunks from actual loaded chunks.
    void adjust_vdev_info();

    /// Write VDevInfo to ALL physical devices (mirrored for redundancy).
    folly::coro::Task< void > write_vdev_info();

    // ── Block allocators ──────────────────────────────────────────────────────

    /// (Re-)construct block allocator for one chunk (or all if chunk == nullptr).
    void init_blk_allocator(cshared< Chunk >& chunk = {});

    /// Recovery: load block allocators from on-disk buffers (chunk_id → ByteArray).
    void load_blk_allocator(const std::unordered_map< uint32_t, sisl::ByteArray >& chunk_buffers = {});

    /// Recovery: load block allocator for a single chunk from its on-disk bitmap buffer.
    void load_blk_allocator(uint32_t chunk_id, const sisl::ByteArray& buffer);

    uint64_t chunk_size_bytes() const;
    VDevInfo get_vdev_info() const;

    size_t num_chunks_actual() const;

private:
    // ── RCU helpers ───────────────────────────────────────────────────────────
    // Reads are lock-free (~2-5 ns): atomic load + folly::rcu_reader guard.
    // Writes clone + make_and_exchange under chunk_mgmt_mutex_ + grace period.
    //
    // IMPORTANT: do NOT hold a load_state() result across any call to store_state()
    // (that would deadlock synchronize_rcu()). Always scope load_state() before
    // calling store_state(), or use clone_state() which releases the guard immediately.

    sisl::Rcu::access_ptr< VDevMutableState > load_state() const { return mutable_state_.get(); }

    // Copy current state; RCU guard is acquired and released inside this call.
    VDevMutableState clone_state() const {
        auto acc = mutable_state_.get();
        return *acc.get(); // copy, then acc (rcu_reader) drops
    }

    // Install new state: atomically swap + wait for RCU grace period.
    // Must NOT be called while any load_state() result is still in scope on this thread.
    void store_state(VDevMutableState new_state) { mutable_state_.make_and_exchange(std::move(new_state)); }

    // ── Internal helpers ──────────────────────────────────────────────────────

    std::pair< uint64_t, shared< Chunk > > to_dev_offset(const BlkId& bid) const;

    void construct_blk_allocator(cshared< Chunk >& chunk, std::optional< sisl::ByteArray > buffer = std::nullopt);

    shared< Chunk > select_chunk_for_alloc(blk_count_t nblks, const blk_alloc_hints& hints,
                                           std::optional< uint32_t > last_failed_id) const;

    static std::vector< shared< PhysicalDev > > pick_pdevs(const std::vector< shared< PhysicalDev > >& pdevs,
                                                           MultiPDevOpts opts);

    static void adjust_vdev_params(VDevParameters& params);

    /// Remove one chunk from mutable state (all_chunks, counters, selector). Called by shrink().
    void on_chunk_removed(cshared< Chunk >& chunk);

    static shared< IChunkSelector > build_chunk_selector(ChunkSelectorType type,
                                                         const std::vector< shared< Chunk > >& chunks);

private:
    // ── Immutable fields (set once at construction, cached for hot-path access) ──
    std::string name_;
    uint32_t vdev_id_;
    HSDevType hs_dev_type_;
    uint32_t blk_size_;
    MultiPDevOpts multi_pdev_choice_;

    BlkAllocatorType allocator_type_;
    ChunkSelectorType chunk_selector_type_;
    bool use_slab_allocator_;
    uint64_t incremental_chunk_size_;
    std::vector< shared< PhysicalDev > > pdevs_; // physical devices backing this vdev

    // ── Mutable state (RCU) ───────────────────────────────────────────────────
    // sisl::Rcu::data<T>: reads are truly lock-free (folly::rcu_reader, ~2-5 ns).
    // Writes call make_and_exchange() which waits for a grace period — cheap for
    // infrequent writes (expand/shrink/recovery), not on the I/O hot path.
    sisl::Rcu::data< VDevMutableState > mutable_state_;

    // ── Chunk management mutex ────────────────────────────────────────────────
    // Serializes all writes to mutable_state_ (expand, shrink, on_chunks_added, on_chunk_removed, destroy).
    // Does NOT protect reads — RCU handles that.
    mutable std::mutex chunk_mgmt_mutex_;

    // ── Optional chunk pool ────────────────────────────────────────────────────
    // Has its own internal mutex (std::mutex inside ChunkPool).
    std::optional< ChunkPool > chunk_pool_;
};

} // namespace homestore
