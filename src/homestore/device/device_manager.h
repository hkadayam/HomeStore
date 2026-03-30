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
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <folly/coro/Task.h>
#include "sisl/fds/bitset.h"

#include "device/device_decl.h"  // HSDevType, IOFlag, DevInfo
#include "device/hs_super_blk.h" // FirstBlockHeader, HSSuperBlk
#include "device/physical_dev.h" // PhysicalDev
#include "device/virtual_dev.h"  // VirtualDev, VDevParameters, VDevInfo

namespace homestore {

// ── DeviceManagerState ────────────────────────────────────────────────────────
// All mutable DeviceManager state. Every field is accessed only while holding
// DeviceManager::state_mutex_.
struct DeviceManagerState {
    std::unordered_map< uint32_t, shared< VirtualDev > > all_vdevs;
    std::unordered_map< uint32_t, shared< PhysicalDev > > all_pdevs;

    // Keyed by static_cast<uint8_t>(HSDevType) to avoid a std::hash specialisation.
    std::unordered_map< uint8_t, std::vector< shared< PhysicalDev > > > pdevs_by_type;

    std::unique_ptr< sisl::Bitset > vdev_slot_bm; // one bit per vdev_id slot
    uint32_t cur_pdev_id{0};
    FirstBlockHeader first_blk_hdr{};
    bool first_time_boot{true};
    bool boot_in_degraded_mode{false};
};

// ── DeviceManager ─────────────────────────────────────────────────────────────
// Owns all PhysicalDevs and VirtualDevs.
//
// Lifecycle:
//   1. Construct with the list of device paths and per-type open flags.
//   2. Call format_devices() on a first-time boot, or load_devices() on restart.
//   3. Use create_vdev() / get_vdev() / destroy_vdev() to manage VirtualDevs.
//   4. Call close_devices() before destruction.
//
// Thread-safety: all public methods are safe to call from any thread.
// Async methods that perform I/O are folly coroutines (Task<>).
class DeviceManager : public std::enable_shared_from_this< DeviceManager > {
public:
    static shared< DeviceManager > create(std::vector< DevInfo >&& devs, IOFlag data_open_flags, IOFlag fast_open_flags);

    ~DeviceManager() = default;
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // ── Boot-time queries (valid after format_devices / load_devices) ─────────
    bool is_first_time_boot() const;
    bool is_boot_in_degraded_mode() const;

    // ── Device lifecycle ──────────────────────────────────────────────────────
    folly::coro::Task< void > format_devices();
    folly::coro::Task< void > commit_formatting();
    folly::coro::Task< void > load_devices();
    folly::coro::Task< void > close_devices();

    // ── VirtualDev management ─────────────────────────────────────────────────
    folly::coro::Task< shared< VirtualDev > > create_vdev(VDevParameters&& params);

    /// Destroys the vdev on disk, removes it from the registry, frees its slot, and persists the bitmap.
    folly::coro::Task< void > destroy_vdev(cshared< VirtualDev >& vdev);

    // ── PhysicalDev accessors (thread-safe, lock-protected) ───────────────────
    shared< PhysicalDev > get_pdev(uint32_t pdev_id) const;
    std::vector< shared< PhysicalDev > > get_pdevs_by_dev_type(HSDevType dtype) const;
    std::vector< shared< PhysicalDev > > get_all_pdevs() const;

    // ── VirtualDev accessors ──────────────────────────────────────────────────
    shared< VirtualDev > get_vdev(uint32_t vdev_id) const;
    shared< VirtualDev > get_vdev(std::string_view name) const;

    // ── Capacity / alignment queries (from PhysicalDevs of the given type) ────
    uint64_t total_capacity() const;
    uint64_t total_capacity_by_type(HSDevType dtype) const;
    uint32_t atomic_page_size(HSDevType dtype) const;
    uint32_t optimal_page_size(HSDevType dtype) const;
    uint32_t align_size(HSDevType dtype) const;

    // ── VDev slot bitmap management ───────────────────────────────────────────
    std::optional< uint32_t > allocate_vdev_id();
    void free_vdev_id(uint32_t vdev_id);

private:
    DeviceManager(std::vector< DevInfo >&& devs, IOFlag data_open_flags, IOFlag fast_open_flags);

    // ── Private async helpers ─────────────────────────────────────────────────
    folly::coro::Task< void > load_vdevs();
    folly::coro::Task< void > cleanup_stale_slot_vdevs(const std::vector< uint32_t >& stale_slot_ids);
    folly::coro::Task< void > write_vdev_slot_bitmap();

    static folly::coro::Task< VDevInfo > read_vdev_info(cshared< PhysicalDev >& pdev, uint32_t vdev_id);

    /// Returns (start_slot, end_slot) pairs of consecutive set-bit runs in bm.
    static std::vector< std::pair< uint32_t, uint32_t > > find_consecutive_ranges(const sisl::Bitset& bm);

    int device_open_flags(HSDevType dtype) const;

private:
    // ── Immutable after construction ──────────────────────────────────────────
    std::vector< DevInfo > dev_infos_;
    int data_open_flags_;
    int fast_open_flags_;

    // ── Mutable state (lock-protected) ───────────────────────────────────────
    mutable std::mutex state_mutex_;
    DeviceManagerState state_;
};

} // namespace homestore
