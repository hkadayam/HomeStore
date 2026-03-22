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
#include <sisl/fds/urcu_helper.h>

#include <homestore/blk.h>              // BlkId, MultiBlkId, BlkAllocStatus, blk_alloc_hints, blk_count_t
#include <homestore/crc.h>              // crc16_t10dif, hs_init_crc_16
#include <homestore/homestore_decl.hpp> // HSDevType, blk_allocator_type_t

#include "iomanager/drive_interface.hpp" // IOBuffer
#include "device/chunk.h"            // Chunk, ChunkPool, ChunkInfo
#include "device/chunk_selector.h"   // IChunkSelector, ChunkSelectorType, concrete selectors

namespace homestore {

class PhysicalDev;
class BlkAllocator;

VENUM(MultiPDevOpts, uint8_t, AllPDevStriped = 0, AllPDevMirrored = 1, SingleFirstPDev = 2, SingleRandomPDev = 3);
VENUM(VDevSizeType, uint8_t, Static = 0, Dynamic = 1);
VENUM(BlkAllocatorType, uint8_t, None = 0, SlabCompact = 1, SlabExtend = 2, Append = 3);

// Which chunk to remove during shrink (for Dynamic VDevs only).
ENUM(ChunkToShrink, uint8_t,
     Last,     // Remove the chunk with the highest creation_order.
     Specific, // Remove a specific chunk by ID; pass specific_chunk_id to shrink().
);

// ── VDevInfo ──────────────────────────────────────────────────────────────────
// On-disk metadata for one virtual device.
// Binary layout is #pragma pack(1), identical to Rust's VDevInfo (#[repr(C, packed)]).
//
// Field offsets:
//    0  vdev_size           u64  (8)
//    8  vdev_id             u32  (4)
//   12  num_mirrors         u32  (4)
//   16  blk_size            u32  (4)
//   20  num_primary_chunks  u32  (4)
//   24  chunk_size          u32  (4)
//   28  size_type           u8   (1)
//   29  slot_allocated      u8   (1)
//   30  failed              u8   (1)
//   31  hs_dev_type         u8   (1)
//   32  multi_pdev_choice   u8   (1)
//   33  name                u8[64]
//   97  checksum            u16  (2)
//   99  alloc_type          u8   (1)
//  100  chunk_sel_type      u8   (1)
//  101  use_slab_allocator  u8   (1)
//  102  padding             u8[154]
//  256  user_private        u8[256]
//  512  = VDevInfo::SIZE
#pragma pack(1)
struct VDevInfo {
    static constexpr size_t SIZE = 512;
    static constexpr size_t USER_PRIVATE_SIZE = 256;
    static constexpr size_t MAX_NAME_LEN = 64;

    uint64_t vdev_size{0};                     //   0
    uint32_t vdev_id{0};                       //   8
    uint32_t num_mirrors{0};                   //  12
    uint32_t blk_size{0};                      //  16
    uint32_t num_primary_chunks{0};            //  20
    uint32_t chunk_size{0};                    //  24
    uint8_t size_type{0};                      //  28  (VDevSizeType as u8)
    uint8_t slot_allocated{0};                 //  29
    uint8_t failed{0};                         //  30
    uint8_t hs_dev_type{0};                    //  31  (HSDevType as u8)
    uint8_t multi_pdev_choice{0};              //  32  (MultiPDevOpts as u8)
    char name[MAX_NAME_LEN]{};                 //  33
    uint16_t checksum{0};                      //  97
    uint8_t alloc_type{0};                     //  99  (BlkAllocatorType as u8)
    uint8_t chunk_sel_type{0};                 // 100  (ChunkSelectorType as u8)
    uint8_t use_slab_allocator{0};             // 101
    uint8_t padding[154]{};                    // 102
    uint8_t user_private[USER_PRIVATE_SIZE]{}; // 256

    // ── Accessors ──────────────────────────────────────────────────────────
    bool is_allocated() const { return slot_allocated == 0x01; }
    void set_allocated() { slot_allocated = 0x01; }
    void set_free() { slot_allocated = 0x00; }

    bool is_failed() const { return failed == 0x01; }

    void set_name(const std::string& n) {
        std::strncpy(name, n.c_str(), MAX_NAME_LEN - 1);
        name[MAX_NAME_LEN - 1] = '\0';
    }
    std::string get_name() const { return std::string{name}; }

    void compute_checksum() {
        checksum = 0;
        checksum = crc16_t10dif(hs_init_crc_16, reinterpret_cast< const unsigned char* >(this), sizeof(VDevInfo));
    }

    const uint8_t* to_bytes() const { return reinterpret_cast< const uint8_t* >(this); }

    /// Byte offset of this vdev's record within the superblock area on any pdev.
    /// Mirrors Rust's VDevInfo::vdev_info_offset(vdev_id).
    static uint64_t vdev_info_offset(uint32_t vdev_id);
};
#pragma pack()

static_assert(sizeof(VDevInfo) == VDevInfo::SIZE, "VDevInfo size mismatch");

// ── VDevParameters ────────────────────────────────────────────────────────────
// Creation parameters for a new VirtualDev. Mirrors Rust's VDevParameters.
struct VDevParameters {
    std::string vdev_name;
    uint64_t vdev_size{0};
    uint32_t num_chunks{0};
    uint64_t chunk_size{0};
    uint64_t incremental_chunk_size{0}; // Only for Dynamic VDevs
    VDevSizeType size_type{VDevSizeType::Static};
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
    std::vector< shared< Chunk > > chunks_by_creation_order;    // sorted (cold path)
    uint64_t total_chunk_num{0};
    uint32_t next_creation_order{0};         // monotonically increasing
    shared< IChunkSelector > chunk_selector; // rebuilt on every chunk-set change
};

// ── VirtualDev ────────────────────────────────────────────────────────────────
//
// Key design:
//  • RCU mutable state: sisl::urcu_data<VDevMutableState> — reads are truly lock-free (atomic load + folly::rcu_reader
//    guard, ~2-5 ns),
//  • Dynamic expand()/shrink() for VDevSizeType::Dynamic vdevs.
//  • ChunkPool integration for efficient chunk reuse in dynamic vdevs.
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
    /// Mirrors Rust's VirtualDev::create().
    static folly::coro::Task< unique< VirtualDev > > create(VDevParameters params, uint32_t vdev_id,
                                                            const std::vector< shared< PhysicalDev > >& pdevs);

    /// Recovery: constructs VDev from persisted VDevInfo. Caller should then call on_chunk_found() for each chunk
    /// (which atomically rebuilds the selector), then load_blk_allocator(). Mirrors Rust's VirtualDev::load().
    static unique< VirtualDev > load(VDevInfo vinfo, std::vector< shared< PhysicalDev > > pdevs);

    /// Destroy the entire vdev and remove all its chunks and remove the vdev info. Upon completion next load
    /// will not have any trace of this vdev.
    folly::coro::Task< void > destroy();

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: Device Resizing section with chunks
    // ──────────────────────────────────────────────────────────────────────────────
    /// Expand: allocate one new chunk for a Dynamic vdev. Returns the new chunk.
    folly::coro::Task< shared< Chunk > > expand(uint64_t chunk_size);

    /// Shrink: remove a chunk from a Dynamic vdev.
    /// Pooling enabled → deactivate + park in pool; disabled → permanently remove.
    /// Returns the removed chunk_id. Mirrors Rust's shrink().
    folly::coro::Task< uint32_t > shrink(ChunkToShrink which, uint32_t specific_chunk_id = 0);

    /// Register one chunk with this vdev (recovery or post-create). Forwards to on_chunks_added().
    void on_chunk_added(const shared< Chunk >& chunk, bool newly_created);

    /// Register a batch of chunks. Active chunks enter all_chunks and rebuild the selector atomically;
    /// inactive chunks go to chunk_pool_. newly_created=true also constructs a fresh blk allocator per chunk.
    void on_chunks_added(std::vector< shared< Chunk > > chunks, bool newly_created);

    /// Enable chunk pooling for dynamic vdevs.
    void enable_chunk_pooling(size_t pool_limit);

    /// Get the nth chunk (0-indexed by creation_order), creating it if needed.
    /// Returns (chunk, is_newly_created). Mirrors Rust's get_or_create_nth_chunk().
    folly::coro::Task< std::pair< shared< Chunk >, bool > > get_or_create_nth_chunk(size_t n);

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: I/Os
    // ──────────────────────────────────────────────────────────────────────────────
    folly::coro::Task< void > write(const IOBuffer& buf, const BlkId& bid);
    folly::coro::Task< void > writev(std::vector< IOBuffer > bufs, const BlkId& bid);
    folly::coro::Task< std::pair< std::error_code, IOBuffer > > read(IOBuffer buf, const BlkId& bid);
    folly::coro::Task< void > format();
    folly::coro::Task< void > fsync();

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: Block Allocations
    // ──────────────────────────────────────────────────────────────────────────────
    BlkAllocStatus alloc_contiguous_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkId& out_blkid);
    BlkAllocStatus alloc_blks(blk_count_t nblks, const blk_alloc_hints& hints, MultiBlkId& out_blkid);
    void free_blk(const BlkId& bid);
    BlkAllocStatus commit_blk(const BlkId& bid);

    // ──────────────────────────────────────────────────────────────────────────────
    // Public APIs: Getters
    // ──────────────────────────────────────────────────────────────────────────────
    shared< Chunk > get_nth_chunk(size_t n) const;
    std::vector< shared< Chunk > > get_chunks() const;
    std::vector< shared< Chunk > > get_chunks_by_creation_order() const;

    uint32_t vdev_id() const { return vdev_id_; }
    const std::string& name() const { return name_; }
    uint32_t block_size() const { return blk_size_; }
    HSDevType hs_dev_type() const { return hs_dev_type_; }
    VDevSizeType size_type() const { return size_type_; }
    BlkAllocatorType allocator_type() const { return allocator_type_; }
    ChunkSelectorType chunk_selector_type() const { return chunk_selector_type_; }
    uint64_t incremental_chunk_size() const { return incremental_chunk_size_; }
    uint64_t size() const;
    uint64_t num_chunks() const;

    // ── VDevInfo ──────────────────────────────────────────────────────────────

    /// Recompute vdev_size and num_primary_chunks from actual loaded chunks.
    void adjust_vdev_info();

    /// Write VDevInfo to ALL physical devices (mirrored for redundancy).
    /// Mirrors Rust's write_vdev_info().
    folly::coro::Task< void > write_vdev_info();

    // ── Block allocators ──────────────────────────────────────────────────────

    /// (Re-)construct block allocator for one chunk (or all if chunk == nullptr).
    void init_blk_allocator(shared< Chunk >& chunk = nullptr);

    /// Recovery: load block allocators from on-disk buffers (chunk_id → ByteArray).
    void load_blk_allocator(const std::unordered_map< uint32_t, sisl::ByteArray >& chunk_buffers = {});

    uint64_t chunk_size_bytes() const;
    VDevInfo get_vdev_info() const;

    size_t num_chunks_actual() const;

private:
    // ── RCU helpers ───────────────────────────────────────────────────────────
    // Reads are lock-free (~2-5 ns): atomic load + folly::rcu_reader guard.
    // Writes clone + make_and_exchange under chunk_mgmt_mutex_ + grace period.
    // Mirrors Rust's mutable_state.load_full() / mutable_state.store(Arc::new(s)).
    //
    // IMPORTANT: do NOT hold a load_state() result across any call to store_state()
    // (that would deadlock synchronize_rcu()). Always scope load_state() before
    // calling store_state(), or use clone_state() which releases the guard immediately.

    sisl::_urcu_access_ptr< VDevMutableState > load_state() const { return mutable_state_.get(); }

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

    void construct_blk_allocator(const shared< Chunk >& chunk,
                                 std::optional< sisl::ByteArray > buffer = std::nullopt);

    shared< Chunk > select_chunk_for_alloc(blk_count_t nblks, const blk_alloc_hints& hints,
                                           std::optional< uint32_t > last_failed_id) const;

    static std::vector< shared< PhysicalDev > > pick_pdevs(const std::vector< shared< PhysicalDev > >& pdevs,
                                                           MultiPDevOpts opts);

    static void adjust_vdev_params(VDevParameters& params);

    /// Remove one chunk from mutable state (all_chunks, counters, selector). Called by shrink().
    void on_chunk_removed(const shared< Chunk >& chunk);

    static shared< IChunkSelector > build_chunk_selector(ChunkSelectorType type,
                                                         const std::vector< shared< Chunk > >& chunks);

private:
    // ── Immutable fields (set once at construction, cached for hot-path access) ──
    std::string name_;
    uint32_t vdev_id_;
    HSDevType hs_dev_type_;
    uint32_t blk_size_;
    MultiPDevOpts multi_pdev_choice_;
    VDevSizeType size_type_;
    BlkAllocatorType allocator_type_;
    ChunkSelectorType chunk_selector_type_;
    bool use_slab_allocator_;
    uint64_t incremental_chunk_size_;
    std::vector< shared< PhysicalDev > > pdevs_; // physical devices backing this vdev

    // ── Mutable state (RCU) ───────────────────────────────────────────────────
    // sisl::urcu_data<T>: reads are truly lock-free (folly::rcu_reader, ~2-5 ns).
    // Writes call make_and_exchange() which waits for a grace period — cheap for
    // infrequent writes (expand/shrink/recovery), not on the I/O hot path.
    sisl::urcu_data< VDevMutableState > mutable_state_;

    // ── Chunk management mutex ────────────────────────────────────────────────
    // Serializes all writes to mutable_state_ (expand, shrink, on_chunks_added, on_chunk_removed, destroy).
    // Does NOT protect reads — RCU handles that.
    mutable std::mutex chunk_mgmt_mutex_;

    // ── Optional chunk pool (Dynamic vdevs only) ──────────────────────────────
    // Has its own internal mutex (std::mutex inside ChunkPool).
    std::optional< ChunkPool > chunk_pool_;
};

} // namespace homestore
