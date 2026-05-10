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

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

#include <boost/uuid/random_generator.hpp>
#include <fcntl.h>
#include "sisl/fds/buffer.h"
#include "sisl/logging/logging.h"

#include "homestore/blkalloc/sweep_service.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/physical_dev.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/managers.h"

namespace homestore {

using namespace iomanager;
using sisl::IOBuffer;

// ── Constructor ───────────────────────────────────────────────────────────────

static int io_flag_to_posix(IOFlag f) {
    switch (f) {
    case IOFlag::BUFFERED_IO:
        return O_RDWR | O_CREAT;
    case IOFlag::READ_ONLY:
        return O_RDONLY;
    case IOFlag::DIRECT_IO:
#ifdef O_DIRECT
        return O_RDWR | O_CREAT | O_DIRECT;
#else
        return O_RDWR | O_CREAT;
#endif
    }
    return O_RDWR | O_CREAT;
}

DeviceManager::DeviceManager(std::vector< DevInfo >&& devs, IOFlag data_open_flags, IOFlag fast_open_flags) :
        dev_infos_{std::move(devs)},
        data_open_flags_{io_flag_to_posix(data_open_flags)},
        fast_open_flags_{io_flag_to_posix(fast_open_flags)} {
}

shared< DeviceManager > DeviceManager::create(std::vector< DevInfo >&& devs, IOFlag data_open_flags,
                                              IOFlag fast_open_flags) {
    // Bring up the module-scoped sweep service before any chunk/allocator can be constructed.
    // Idempotent — safe to call across multiple DeviceManager lifetimes within the same process.
    blkalloc::init_sweep_service();
    auto mgr = shared< DeviceManager >(new DeviceManager(std::move(devs), data_open_flags, fast_open_flags));
    Managers::init_device_mgr(mgr);
    return mgr;
}

folly::coro::Task< shared< DeviceManager > >
DeviceManager::create_and_format(std::vector< DevInfo >&& devs, IOFlag data_open_flags, IOFlag fast_open_flags) {
    auto mgr = create(std::move(devs), data_open_flags, fast_open_flags);
    co_await mgr->format_devices();
    co_await mgr->commit_formatting();
    co_return mgr;
}

// ── Boot-time queries ─────────────────────────────────────────────────────────

bool DeviceManager::is_first_time_boot() const {
    std::lock_guard lg{state_mutex_};
    return state_.first_time_boot;
}

bool DeviceManager::is_boot_in_degraded_mode() const {
    std::lock_guard lg{state_mutex_};
    return state_.boot_in_degraded_mode;
}

// ── Device lifecycle ──────────────────────────────────────────────────────────

folly::coro::Task< void > DeviceManager::format_devices() {
    {
        std::lock_guard lg{state_mutex_};
        auto& hdr = state_.first_blk_hdr;
        hdr.gen_number += 1;
        hdr.version = FirstBlockHeader::CURRENT_SUPERBLOCK_VERSION;
        std::strncpy(hdr.product_name, FirstBlockHeader::PRODUCT_NAME, FirstBlockHeader::s_product_name_size);
        hdr.num_pdevs = to_u32(dev_infos_.size());
        hdr.max_vdevs = HSSuperBlk::MAX_VDEVS_IN_SYSTEM;
        hdr.max_system_chunks = HSSuperBlk::MAX_CHUNKS_IN_SYSTEM;
        hdr.system_uuid = boost::uuids::random_generator{}();

        state_.vdev_slot_bm = std::make_unique< sisl::Bitset >(HSSuperBlk::MAX_VDEVS_IN_SYSTEM);
        state_.first_time_boot = true;
    }

    for (const auto& dinfo : dev_infos_) {
        const int oflags = device_open_flags(dinfo.dev_type);

        uint32_t pdev_id;
        {
            std::lock_guard lg{state_mutex_};
            pdev_id = state_.cur_pdev_id++;
        }

        auto pdev = co_await PhysicalDev::create(dinfo, oflags, pdev_id, state_.first_blk_hdr);
        const uint32_t id = pdev->pdev_id();

        {
            std::lock_guard lg{state_mutex_};
            state_.pdevs_by_type[to_u8(dinfo.dev_type)].push_back(pdev);
            state_.all_pdevs.emplace(id, std::move(pdev));
        }
    }

    co_await write_vdev_slot_bitmap();
}

folly::coro::Task< void > DeviceManager::commit_formatting() {
    std::vector< shared< PhysicalDev > > pdevs;
    {
        std::lock_guard lg{state_mutex_};
        for (auto& [id, p] : state_.all_pdevs) {
            pdevs.push_back(p);
        }
    }
    for (auto& pdev : pdevs) {
        co_await pdev->commit_formatting();
    }
    LOGINFO("HomeStore formatting committed on all {} physical devices", pdevs.size());
}

folly::coro::Task< void > DeviceManager::load_devices() {
    // Read the first block from the first device to recover the system header (uuid, num_pdevs, etc.).
    {
        const auto& first_dev = dev_infos_.front();
        const int oflags = device_open_flags(first_dev.dev_type);
        auto fb = co_await PhysicalDev::read_first_block(first_dev.dev_name, oflags);
        if (!fb.is_valid()) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "Invalid first block on lead device " + first_dev.dev_name);
        }

        std::lock_guard lg{state_mutex_};
        state_.first_blk_hdr = fb.hdr;
        state_.first_time_boot = false;

        const uint32_t expected = state_.first_blk_hdr.num_pdevs;
        const uint32_t actual = to_u32(dev_infos_.size());
        if (expected != actual) {
            LOGWARN("Homestore formatted with {} devices but restarted with {} devices — degraded mode", expected,
                    actual);
            state_.boot_in_degraded_mode = true;
        }
    }

    for (const auto& dinfo : dev_infos_) {
        const int oflags = device_open_flags(dinfo.dev_type);

        auto pdev = co_await PhysicalDev::load(dinfo, oflags, state_.first_blk_hdr);
        const uint32_t id = pdev->pdev_id();

        {
            std::lock_guard lg{state_mutex_};
            state_.pdevs_by_type[to_u8(dinfo.dev_type)].push_back(pdev);
            state_.all_pdevs.emplace(id, std::move(pdev));
        }
    }

    co_await load_vdevs();
}

folly::coro::Task< void > DeviceManager::close_devices() {
    // Stop the sweep service before tearing down devices: the ticker thread holds weak_ptrs into per-allocator
    // SegmentManagers, and dereferencing them after PhysicalDev/Chunk teardown is a use-after-free.
    blkalloc::shutdown_sweep_service();

    std::vector< shared< PhysicalDev > > pdevs;
    {
        std::lock_guard lg{state_mutex_};
        for (auto& [id, p] : state_.all_pdevs) {
            pdevs.push_back(p);
        }
    }
    for (auto& pdev : pdevs) {
        co_await pdev->close_device();
    }
}

// ── VirtualDev management ─────────────────────────────────────────────────────

folly::coro::Task< shared< VirtualDev > > DeviceManager::create_vdev(VDevParameters&& params) {
    auto pdevs = get_pdevs_by_dev_type(params.dev_type);
    if (pdevs.empty()) {
        throw std::runtime_error(fmt::format("No physical devices of type {} available", enum_name(params.dev_type)));
    }

    const auto vdev_id_opt = allocate_vdev_id();
    if (!vdev_id_opt) {
        throw std::runtime_error(fmt::format("No VDev slots available (max: {})", HSSuperBlk::MAX_VDEVS_IN_SYSTEM));
    }
    const uint32_t vdev_id = *vdev_id_opt;

    auto vdev_unique = co_await VirtualDev::create(std::move(params), vdev_id, pdevs);
    auto vdev = shared< VirtualDev >{std::move(vdev_unique)};

    {
        std::lock_guard lg{state_mutex_};
        state_.all_vdevs.emplace(vdev_id, vdev);
    }

    co_await write_vdev_slot_bitmap();

    LOGINFO("Created VirtualDev '{}' id={}", vdev->name(), vdev_id);
    co_return vdev;
}

folly::coro::Task< void > DeviceManager::destroy_vdev(cshared< VirtualDev >& vdev) {
    co_await vdev->destroy();
    const uint32_t vdev_id = vdev->vdev_id();
    {
        std::lock_guard lg{state_mutex_};
        state_.all_vdevs.erase(vdev_id);
    }
    free_vdev_id(vdev_id);
    co_await write_vdev_slot_bitmap();
    LOGINFO("VDev '{}' id={} removed from registry; bitmap committed", vdev->name(), vdev_id);
}

// ── PhysicalDev accessors ─────────────────────────────────────────────────────

shared< PhysicalDev > DeviceManager::get_pdev(uint32_t pdev_id) const {
    std::lock_guard lg{state_mutex_};
    auto it = state_.all_pdevs.find(pdev_id);
    return (it != state_.all_pdevs.end()) ? it->second : nullptr;
}

std::vector< shared< PhysicalDev > > DeviceManager::get_pdevs_by_dev_type(HSDevType dtype) const {
    std::lock_guard lg{state_mutex_};
    auto it = state_.pdevs_by_type.find(to_u8(dtype));
    if (it != state_.pdevs_by_type.end()) {
        return it->second;
    }
    // Fall back to Data pdevs when the requested type has no dedicated devices.
    auto it2 = state_.pdevs_by_type.find(to_u8(HSDevType::Data));
    return (it2 != state_.pdevs_by_type.end()) ? it2->second : std::vector< shared< PhysicalDev > >{};
}

std::vector< shared< PhysicalDev > > DeviceManager::get_all_pdevs() const {
    std::lock_guard lg{state_mutex_};
    std::vector< shared< PhysicalDev > > out;
    out.reserve(state_.all_pdevs.size());
    for (auto& [id, p] : state_.all_pdevs) {
        out.push_back(p);
    }
    return out;
}

// ── VirtualDev accessor ───────────────────────────────────────────────────────

shared< VirtualDev > DeviceManager::get_vdev(uint32_t vdev_id) const {
    std::lock_guard lg{state_mutex_};
    auto it = state_.all_vdevs.find(vdev_id);
    return (it != state_.all_vdevs.end()) ? it->second : nullptr;
}

shared< VirtualDev > DeviceManager::get_vdev(std::string_view name) const {
    std::lock_guard lg{state_mutex_};
    for (auto& [id, vdev] : state_.all_vdevs) {
        if (vdev->name() == name) {
            return vdev;
        }
    }
    return nullptr;
}

// ── Capacity / alignment queries ──────────────────────────────────────────────

uint64_t DeviceManager::total_capacity() const {
    std::lock_guard lg{state_mutex_};
    uint64_t total = 0;
    for (auto& [id, p] : state_.all_pdevs) {
        total += p->data_size();
    }
    return total;
}

uint64_t DeviceManager::total_capacity_by_type(HSDevType dtype) const {
    std::lock_guard lg{state_mutex_};
    auto it = state_.pdevs_by_type.find(to_u8(dtype));
    if (it == state_.pdevs_by_type.end()) {
        return 0;
    }
    uint64_t total = 0;
    for (auto& p : it->second) {
        total += p->data_size();
    }
    return total;
}

uint32_t DeviceManager::atomic_page_size(HSDevType dtype) const {
    const auto pdevs = get_pdevs_by_dev_type(dtype);
    return pdevs.empty() ? 512u : pdevs.front()->atomic_page_size();
}

uint32_t DeviceManager::optimal_page_size(HSDevType dtype) const {
    const auto pdevs = get_pdevs_by_dev_type(dtype);
    return pdevs.empty() ? 4096u : pdevs.front()->optimal_page_size();
}

uint32_t DeviceManager::align_size(HSDevType dtype) const {
    const auto pdevs = get_pdevs_by_dev_type(dtype);
    return pdevs.empty() ? 512u : pdevs.front()->align_size();
}

// ── VDev slot bitmap management ───────────────────────────────────────────────

std::optional< uint32_t > DeviceManager::allocate_vdev_id() {
    std::lock_guard lg{state_mutex_};
    assert(state_.vdev_slot_bm);
    const uint64_t pos = state_.vdev_slot_bm->get_next_reset_bit(0);
    if (pos == sisl::Bitset::npos) {
        return std::nullopt;
    }
    state_.vdev_slot_bm->set_bit(pos);
    return to_u32(pos);
}

void DeviceManager::free_vdev_id(uint32_t vdev_id) {
    std::lock_guard lg{state_mutex_};
    assert(state_.vdev_slot_bm);
    state_.vdev_slot_bm->reset_bit(to_u64(vdev_id));
}

// ── Private async helpers ─────────────────────────────────────────────────────

folly::coro::Task< void > DeviceManager::load_vdevs() {
    // Collect all pdevs and load chunks from each, merging into a vdev_id → chunks map.
    std::vector< shared< PhysicalDev > > all_pdevs;
    {
        std::lock_guard lg{state_mutex_};
        for (auto& [id, p] : state_.all_pdevs) {
            all_pdevs.push_back(p);
        }
    }

    if (all_pdevs.empty()) {
        throw std::runtime_error("No physical devices loaded; cannot load vdevs");
    }

    // Building vdev - pdev - chunk mapping
    std::unordered_map< uint32_t, std::vector< shared< Chunk > > > all_vdev_chunks;
    for (auto& pdev : all_pdevs) {
        auto pdev_chunks = co_await pdev->load_chunks();
        for (auto& [vdev_id, chunks] : pdev_chunks) {
            auto& vec = all_vdev_chunks[vdev_id];
            vec.insert(vec.end(), chunks.begin(), chunks.end());
        }
    }

    // Read the vdev slot bitmap from the first pdev.
    auto& first_pdev = all_pdevs[0];
    const uint64_t bitmap_offset = HSSuperBlk::vdev_sb_offset();
    const uint32_t bitmap_size = HSSuperBlk::vdev_slot_bitmap_size();

    auto ba = sisl::make_byte_array(bitmap_size, first_pdev->align_size());
    if (auto ec = co_await first_pdev->read_super_block(*ba, bitmap_offset); ec) {
        throw std::system_error(ec, "Failed to read vdev slot bitmap");
    }

    auto vdev_slot_bm = std::make_unique< sisl::Bitset >(std::move(ba));
    const auto active_ranges = find_consecutive_ranges(*vdev_slot_bm);
    {
        std::lock_guard lg{state_mutex_};
        state_.vdev_slot_bm = std::move(vdev_slot_bm);
    }

    if (active_ranges.empty()) {
        LOGINFO("No active VDev slots in bitmap");
        co_return;
    }

    std::vector< uint32_t > stale_slot_vdev_ids;
    std::unordered_set< uint32_t > loaded_vdev_ids;

    // Build vdev_id → pdev mapping from chunks.  VDevInfo is written only to the vdev's backing pdevs (not mirrored to
    // every pdev like the slot bitmap), so a vdev with zero chunks on disk can't be located from chunks alone.  For
    // those, scan all pdevs at VDevInfo::vdev_info_offset(vdev_id) and use the one whose vinfo is allocated.
    std::unordered_map< uint32_t, shared< PhysicalDev > > vdev_to_pdev;
    for (auto& [vdev_id, chunks] : all_vdev_chunks) {
        if (!chunks.empty()) {
            vdev_to_pdev.emplace(vdev_id, chunks.front()->physical_dev());
        }
    }

    auto read_vinfo_from = [](const shared< PhysicalDev >& pdev,
                              uint32_t vdev_id) -> folly::coro::Task< std::optional< VDevInfo > > {
        const uint64_t off = VDevInfo::vdev_info_offset(vdev_id);
        IOBuffer buf{to_u32(VDevInfo::SIZE)};
        if (auto ec = co_await pdev->read_super_block(buf, off); ec) {
            co_return std::nullopt;
        }
        VDevInfo v{};
        std::memcpy(&v, buf.bytes(), VDevInfo::SIZE);
        co_return v;
    };

    // Read VDevInfo for each active slot, scanning all pdevs as a fallback for vdevs with no chunks.
    for (auto [range_start, range_end] : active_ranges) {
        for (uint32_t vdev_id = range_start; vdev_id <= range_end; ++vdev_id) {
            VDevInfo vinfo{};
            bool found = false;

            if (auto it = vdev_to_pdev.find(vdev_id); it != vdev_to_pdev.end()) {
                // Fast path: read from a pdev we know backs this vdev via its chunks.
                auto v = co_await read_vinfo_from(it->second, vdev_id);
                if (v && v->is_allocated()) {
                    vinfo = *v;
                    found = true;
                }
            }

            if (!found) {
                // Slow path: vdev has no chunks (or the fast-path pdev's vinfo isn't valid).  Scan all pdevs for a
                // valid vinfo at this vdev_id's slot.
                for (auto& p : all_pdevs) {
                    auto v = co_await read_vinfo_from(p, vdev_id);
                    if (v && v->is_allocated()) {
                        vinfo = *v;
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                LOGWARN("Found stale-slot VDev id={} (bitmap set but no pdev has a valid vdev_info)", vdev_id);
                stale_slot_vdev_ids.push_back(vdev_id);
                continue;
            }

            LOGINFO("Loading VirtualDev id={} name={}", vdev_id, vinfo.get_name());

            auto backing_pdevs = get_pdevs_by_dev_type(static_cast< HSDevType >(vinfo.hs_dev_type));
            auto vdev = VirtualDev::load(vinfo, std::move(backing_pdevs));

            if (auto it = all_vdev_chunks.find(vdev_id); it != all_vdev_chunks.end()) {
                vdev->on_chunks_added(std::move(it->second), /*newly_created=*/false);
            }

            vdev->adjust_vdev_info();

            {
                std::lock_guard lg{state_mutex_};
                state_.all_vdevs.emplace(vdev_id, shared< VirtualDev >{std::move(vdev)});
            }

            loaded_vdev_ids.insert(vdev_id);
        }
    }

    if (!stale_slot_vdev_ids.empty()) {
        LOGINFO("Cleaning up {} stale-slot VDev(s)", stale_slot_vdev_ids.size());
        co_await cleanup_stale_slot_vdevs(stale_slot_vdev_ids);
    }

    // Chunks that belong to a vdev_id not in loaded_vdev_ids crashed during creation.
    std::vector< uint32_t > dangling_chunk_vdev_ids;
    for (auto& [vdev_id, _] : all_vdev_chunks) {
        if (!loaded_vdev_ids.count(vdev_id)) {
            dangling_chunk_vdev_ids.push_back(vdev_id);
        }
    }
    if (!dangling_chunk_vdev_ids.empty()) {
        LOGWARN("{} VDev(s) have dangling-chunk chunks (crashed during creation); removing",
                dangling_chunk_vdev_ids.size());
        for (uint32_t vdev_id : dangling_chunk_vdev_ids) {
            for (auto& pdev : all_pdevs) {
                co_await pdev->remove_chunks_for_vdev(vdev_id);
            }
        }
    }

    LOGINFO("Loaded {} virtual device(s)", state_.all_vdevs.size());
}

folly::coro::Task< void > DeviceManager::cleanup_stale_slot_vdevs(const std::vector< uint32_t >& stale_slot_ids) {
    std::vector< shared< PhysicalDev > > all_pdevs;
    {
        std::lock_guard lg{state_mutex_};
        for (auto& [id, p] : state_.all_pdevs) {
            all_pdevs.push_back(p);
        }
    }

    for (uint32_t vdev_id : stale_slot_ids) {
        LOGINFO("Cleaning up stale-slot VDev id={}", vdev_id);
        for (auto& pdev : all_pdevs) {
            co_await pdev->remove_chunks_for_vdev(vdev_id);
        }
        free_vdev_id(vdev_id);
    }

    if (!stale_slot_ids.empty()) {
        co_await write_vdev_slot_bitmap();
    }
}

folly::coro::Task< void > DeviceManager::write_vdev_slot_bitmap() {
    const uint64_t offset = HSSuperBlk::vdev_sb_offset();

    sisl::ByteArray ba;
    std::vector< shared< PhysicalDev > > pdevs;
    {
        std::lock_guard lg{state_mutex_};
        assert(state_.vdev_slot_bm);
        ba = state_.vdev_slot_bm->serialize();
        for (auto& [id, p] : state_.all_pdevs) {
            pdevs.push_back(p);
        }
    }
    if (pdevs.empty()) {
        throw std::runtime_error("No physical devices available to persist vdev bitmap");
    }

    for (auto& pdev : pdevs) {
        co_await pdev->write_super_block(*ba, offset);
    }
}

// static
folly::coro::Task< VDevInfo > DeviceManager::read_vdev_info(cshared< PhysicalDev >& pdev, uint32_t vdev_id) {
    const uint64_t offset = VDevInfo::vdev_info_offset(vdev_id);
    IOBuffer buf{VDevInfo::SIZE};
    if (auto ec = co_await pdev->read_super_block(buf, offset); ec) {
        throw std::system_error(ec, "Failed to read VDevInfo");
    }

    VDevInfo vinfo{};
    std::memcpy(&vinfo, buf.bytes(), VDevInfo::SIZE);
    co_return vinfo;
}

// static
std::vector< std::pair< uint32_t, uint32_t > > DeviceManager::find_consecutive_ranges(const sisl::Bitset& bm) {
    std::vector< std::pair< uint32_t, uint32_t > > ranges;
    const uint64_t max_slots = std::min(bm.size(), to_u64(HSSuperBlk::MAX_VDEVS_IN_SYSTEM));
    uint64_t cur = 0;

    while (true) {
        cur = bm.get_next_set_bit(cur);
        if (cur == sisl::Bitset::npos || cur >= max_slots) {
            break;
        }

        const uint32_t range_start = to_u32(cur);

        uint64_t next_reset = bm.get_next_reset_bit(cur + 1);
        const uint32_t range_end = (next_reset == sisl::Bitset::npos || next_reset > max_slots)
            ? to_u32(max_slots - 1)
            : to_u32(next_reset - 1);

        ranges.emplace_back(range_start, range_end);

        cur = to_u64(range_end) + 1;
        if (cur >= max_slots) {
            break;
        }
    }

    return ranges;
}

int DeviceManager::device_open_flags(HSDevType dtype) const {
    return (dtype == HSDevType::Fast) ? fast_open_flags_ : data_open_flags_;
}

} // namespace homestore
