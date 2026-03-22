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
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <homestore/homestore_decl.hpp>    // dev_info, HSDevType
#include <sisl/fds/bitset.h>

// TODO: HSSuperBlk.h still has iomgr deps; will be cleaned up when
// device_metadata is fully separated from iomgr.
#include "device/HSSuperBlk.h"           // PDevInfoHeader, first_block, HSSuperBlk

#include "iomanager/drive_interface.hpp"   // DriveInterface, IoDevice, IOBuffer
#include "device/chunk.h"              // ChunkInfo, ChunkInterval, ChunkIntervalSet, Chunk

namespace homestore {

// ── Global device cache ───────────────────────────────────────────────────────
// Mirrors Rust's CACHED_OPENED_DEVS / open_and_cache_dev / close_and_uncache_dev.
// The cache avoids reopening the same device when both a format pass and a load
// pass reference the same underlying file/block device.
folly::coro::Task< std::shared_ptr< IoDevice > > open_and_cache_dev(std::string devname, int oflags);
folly::coro::Task< void >                        close_and_uncache_dev(std::string devname);

// ── ChunkProvisioner ──────────────────────────────────────────────────────────
// Mirrors Rust's inner ChunkProvisioner struct.
// All mutable chunk-related state is grouped here and protected by
// PhysicalDev::chunk_mutex_ (a folly::coro::Mutex so it can be held across
// co_await points — identical to Rust's AsyncMutex<ChunkProvisioner>).
struct ChunkProvisioner {
    ChunkIntervalSet                                     chunk_data_area;   // occupied ranges
    std::unique_ptr< sisl::Bitset >                     chunk_info_slots;  // slot bitmap
    std::unordered_set< uint64_t >                      chunk_start;       // start-offset dedup set
    std::unordered_map< uint32_t, std::shared_ptr< Chunk > > chunks;       // keyed by chunk_id
};

// ── PhysicalDev ───────────────────────────────────────────────────────────────
// C++ port of Rust's PhysicalDev (physical_dev.rs).
//
// Design notes vs the old device/physical_dev.hpp:
//  • Factory methods create() / load() replace the single constructor.
//    create() is for first-time format; load() is for recovery.
//  • All IO and chunk operations are folly coroutines (Task<>), matching Rust's
//    async/await.
//  • Stream concept removed — Rust dropped it; chunks are keyed by chunk_id.
//  • ChunkProvisioner bundles all chunk state behind a single coroutine mutex
//    so the lock can be held across disk writes, just as Rust's AsyncMutex does.
//  • Metrics removed for now; can be re-added via a separate observer.
//  • ChunkInfo / ChunkInterval / ChunkIntervalSet all live in chunk.h (chunk.rs).
class PhysicalDev : public std::enable_shared_from_this< PhysicalDev > {
public:
    PhysicalDev()                              = default;
    PhysicalDev(const PhysicalDev&)            = delete;
    PhysicalDev& operator=(const PhysicalDev&) = delete;
    ~PhysicalDev()                             = default;

    // ── Factory methods ───────────────────────────────────────────────────────

    /// First-time format: creates pdev_info from dinfo, opens the device, and
    /// initialises the on-disk chunk bitmap. Mirrors Rust's PhysicalDev::create().
    static folly::coro::Task< std::shared_ptr< PhysicalDev > >
    create(dev_info dinfo, int oflags, uint32_t pdev_id);

    /// Recovery: reads the existing pdev_info from the first block, opens the
    /// device, and replays all chunk metadata from disk.
    /// Mirrors Rust's PhysicalDev::load().
    static folly::coro::Task< std::shared_ptr< PhysicalDev > >
    load(dev_info dinfo, int oflags);

    /// Build a PDevInfoHeader for a device (used by DeviceManager too).
    /// Mirrors Rust's PhysicalDev::create_pdev_info().
    static PDevInfoHeader create_pdev_info(const dev_info& dinfo, uint32_t pdev_id);

    /// Read the first block from a device without constructing a PhysicalDev.
    static folly::coro::Task< first_block >
    read_first_block(const std::string& devname, int oflags);

    /// Return the total device/file size in bytes.
    static folly::coro::Task< uint64_t > get_dev_size(const std::string& devname);

    // ── Super block ───────────────────────────────────────────────────────────

    /// Write buf to offset (and optionally mirrored to the footer).
    folly::coro::Task< void > write_super_block(const IOBuffer& buf, uint64_t offset);

    /// Read into buf. Caller retains ownership; returns error_code.
    folly::coro::Task< std::error_code >
    read_super_block(IOBuffer& buf, uint64_t offset);

    folly::coro::Task< void > close_device();

    // ── Data IO ───────────────────────────────────────────────────────────────
    // All async; mirrors Rust's write / writev / read / readv / write_zero / fsync.

    folly::coro::Task< void > write(const IOBuffer& buf, uint64_t offset);
    folly::coro::Task< void > writev(std::vector< IOBuffer > bufs, uint64_t offset);

    folly::coro::Task< std::error_code >
    read(IOBuffer& buf, uint64_t offset);

    folly::coro::Task< std::error_code >
    readv(std::vector< IOBuffer >& bufs, uint64_t offset);

    folly::coro::Task< void > write_zero(uint64_t size, uint64_t offset);
    folly::coro::Task< void > fsync();

    // ── Chunk management ─────────────────────────────────────────────────────

    /// Initialise the on-disk chunk slot bitmap (first-time format).
    folly::coro::Task< void > format_chunks();

    /// Allocate one chunk slot; chunk_id = (pdev_id + 1) * slot_number.
    /// Mirrors Rust's create_chunk().
    folly::coro::Task< std::shared_ptr< Chunk > >
    create_chunk(uint32_t vdev_id, uint64_t size, uint32_t ordinal,
                 const uint8_t* user_private = nullptr, size_t user_private_size = 0);

    /// Allocate num_chunks slots in batch; ordinals start at start_ordinal.
    /// Mirrors Rust's create_chunks().
    folly::coro::Task< std::vector< std::shared_ptr< Chunk > > >
    create_chunks(uint32_t vdev_id, uint32_t num_chunks, uint64_t size,
                  uint32_t start_ordinal = 0);

    /// Load all chunks from disk. Returns vdev_id → [chunks] for recovery.
    /// Mirrors Rust's load_chunks() → HashMap<vdev_id, Vec<Arc<Chunk>>>.
    folly::coro::Task< std::unordered_map< uint32_t, std::vector< std::shared_ptr< Chunk > > > >
    load_chunks();

    /// Remove a single chunk (frees slot, persists bitmap).
    folly::coro::Task< void > remove_chunk(const std::shared_ptr< Chunk >& chunk);

    /// Remove a batch; batches the final bitmap write for efficiency.
    /// Mirrors Rust's remove_chunks() which avoids one bitmap write per chunk.
    folly::coro::Task< void > remove_chunks(const std::vector< std::shared_ptr< Chunk > >& chunks);

    /// Convenience: remove all chunks belonging to vdev_id.
    folly::coro::Task< void > remove_chunks_for_vdev(uint32_t vdev_id);

    /// Mark chunk as unallocated (chunk_allocated = 0) for pool reuse.
    /// Persists updated ChunkInfo; calls chunk->update_info().
    folly::coro::Task< void > deactivate_chunk(const std::shared_ptr< Chunk >& chunk);

    /// Mark chunk as allocated with a new creation_order (from pool reuse).
    /// Persists updated ChunkInfo; calls chunk->update_info().
    folly::coro::Task< void > reactivate_chunk(const std::shared_ptr< Chunk >& chunk,
                                               uint32_t new_creation_order);

    // ── Chunk accessors ───────────────────────────────────────────────────────

    folly::coro::Task< std::vector< std::shared_ptr< Chunk > > > get_all_chunks();
    folly::coro::Task< std::shared_ptr< Chunk > >                get_chunk(uint32_t chunk_id);
    folly::coro::Task< std::vector< std::shared_ptr< Chunk > > > get_chunks_for_vdev(uint32_t vdev_id);
    folly::coro::Task< size_t >                                  get_chunk_count();

    // ── Parameter getters (sync — immutable after construction) ──────────────
    uint32_t           pdev_id()           const { return pdev_info_.pdev_id; }
    const std::string& get_devname()       const { return devname_; }
    uint32_t           optimal_page_size() const { return pdev_info_.dev_attr.phys_page_size; }
    uint32_t           align_size()        const { return pdev_info_.dev_attr.align_size; }
    uint32_t           atomic_page_size()  const { return pdev_info_.dev_attr.atomic_phys_page_size; }
    uint64_t           data_start_offset() const { return pdev_info_.data_offset; }
    uint64_t           data_end_offset()   const;
    uint64_t           data_size()         const { return data_end_offset() - data_start_offset(); }

    /// Byte offset of slot n's ChunkInfo record within the superblock area.
    uint64_t chunk_info_offset_nth(uint32_t slot) const;

private:
    // ── Private factory helper ────────────────────────────────────────────────

    /// Common low-level init: opens device, measures size, populates fields.
    /// Returns a heap-allocated PhysicalDev wrapped in shared_ptr.
    static folly::coro::Task< std::shared_ptr< PhysicalDev > >
    construct(dev_info dinfo, int oflags, PDevInfoHeader pinfo);

    // ── Locked helpers (called with chunk_mutex_ held) ────────────────────────

    /// Find a free region and fill in all fields of cinfo.
    void populate_chunk_info_locked(ChunkProvisioner& prov, ChunkInfo& cinfo,
                                    uint32_t vdev_id, uint64_t size,
                                    uint32_t chunk_id, uint32_t ordinal,
                                    const uint8_t* private_data, size_t private_size);

    /// Clear chunk data-area bookkeeping and mark cinfo free.
    static void free_chunk_info_locked(ChunkProvisioner& prov, ChunkInfo& cinfo);

    /// Walk chunk_data_area to find the first gap of at least `size` bytes.
    ChunkInterval find_next_chunk_area_locked(const ChunkIntervalSet& data_area,
                                              uint64_t size) const;

    // ── Superblock layout helpers ─────────────────────────────────────────────
    uint64_t chunk_sb_offset()        const;
    size_t   chunk_info_bitmap_size() const;
    uint32_t max_chunks_in_pdev()     const;

private:
    // ── Fields ────────────────────────────────────────────────────────────────
    std::shared_ptr< IoDevice >       iodev_;
    std::shared_ptr< DriveInterface > drive_iface_;
    std::string                       devname_;
    HSDevType                         dev_type_{HSDevType::Data};
    dev_info                          dev_info_{"", HSDevType::Data};
    PDevInfoHeader                  pdev_info_;
    uint64_t                          devsize_{0};
    bool                              super_blk_in_footer_{false};

    // All mutable chunk state lives here, protected by chunk_mutex_.
    // Mirrors Rust's AsyncMutex<ChunkProvisioner>.
    folly::coro::Mutex chunk_mutex_;
    ChunkProvisioner   chunk_provisioner_;
};

} // namespace homestore
