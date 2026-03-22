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

#include "device/device_manager.h"
#include "device/physical_dev.h"
#include "device/virtual_dev.h"

namespace homestore {

// ── Constructor ───────────────────────────────────────────────────────────────

static int io_flag_to_posix(io_flag f) {
    switch (f) {
    case io_flag::BUFFERED_IO:
        return O_RDWR | O_CREAT;
    case io_flag::READ_ONLY:
        return O_RDONLY;
    case io_flag::DIRECT_IO:
#ifdef O_DIRECT
        return O_RDWR | O_CREAT | O_DIRECT;
#else
        return O_RDWR | O_CREAT;
#endif
    }
    return O_RDWR | O_CREAT;
}

DeviceManager::DeviceManager(std::vector< dev_info > devs, io_flag data_open_flags, io_flag fast_open_flags) :
        dev_infos_{std::move(devs)},
        data_open_flags_{io_flag_to_posix(data_open_flags)},
        fast_open_flags_{io_flag_to_posix(fast_open_flags)} {
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
        hdr.num_pdevs = static_cast< uint32_t >(dev_infos_.size());
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

        auto pdev = co_await PhysicalDev::create(dinfo, oflags, pdev_id);
        const uint32_t id = pdev->pdev_id();

        {
            std::lock_guard lg{state_mutex_};
            state_.pdevs_by_type[to_u8(dinfo.dev_type)].push_back(pdev);
            state_.all_pdevs.emplace(id, std::move(pdev));
        }
    }

    co_await write_vdev_slot_bitmap();
}

folly::coro::Task< void > DeviceManager::load_devices() {
    {
        std::lock_guard lg{state_mutex_};
        const uint32_t expected = state_.first_blk_hdr.num_pdevs;
        const uint32_t actual = to_u32(dev_infos_.size());
        if (expected != actual) {
            LOGWARN("Homestore formatted with {} devices but restarted with {} devices — degraded mode", expected,
                    actual);
            state_.boot_in_degraded_mode = true;
        }
        state_.first_time_boot = false;
    }

    for (const auto& dinfo : dev_infos_) {
        const int oflags = device_open_flags(dinfo.dev_type);

        uint32_t pdev_id;
        {
            std::lock_guard lg{state_mutex_};
            pdev_id = state_.cur_pdev_id++;
        }

        auto pdev = co_await PhysicalDev::load(dinfo, oflags);
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

folly::coro::Task< shared< VirtualDev > > DeviceManager::create_vdev(VDevParameters params) {
    auto pdevs = get_pdevs_by_dev_type(params.dev_type);
    if (pdevs.empty()) {
        throw std::runtime_error(fmt::format("No physical devices of type {} available", enum_name(params.dev_type)));
    }

    const auto vdev_id_opt = allocate_vdev_id();
    if (!vdev_id_opt) {
        throw std::runtime_error(fmt::format("No VDev slots available (max: {})", HSSuperBlk::MAX_VDEVS_IN_SYSTEM));
    }
    const uint32_t vdev_id = *vdev_id_opt;

    auto vdev_unique = co_await VirtualDev::create(params, vdev_id, pdevs);
    auto vdev = shared< VirtualDev >{std::move(vdev_unique)};

    {
        std::lock_guard lg{state_mutex_};
        state_.all_vdevs.emplace(vdev_id, vdev);
    }

    co_await write_vdev_slot_bitmap();

    LOGINFO("Created VirtualDev '{}' id={}", vdev->name(), vdev_id);
    co_return vdev;
}

folly::coro::Task< void > DeviceManager::destroy_vdev(shared< VirtualDev > vdev) {
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
    auto it = state_.pdevs_by_type.find(static_cast< uint8_t >(dtype));
    if (it != state_.pdevs_by_type.end()) {
        return it->second;
    }
    // Fall back to Data pdevs when the requested type has no dedicated devices.
    auto it2 = state_.pdevs_by_type.find(static_cast< uint8_t >(HSDevType::Data));
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
        if (vdev->name() == name) { return vdev; }
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
    auto it = state_.pdevs_by_type.find(static_cast< uint8_t >(dtype));
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
    return static_cast< uint32_t >(pos);
}

void DeviceManager::free_vdev_id(uint32_t vdev_id) {
    std::lock_guard lg{state_mutex_};
    assert(state_.vdev_slot_bm);
    state_.vdev_slot_bm->reset_bit(static_cast< uint64_t >(vdev_id));
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
    const uint64_t bitmap_offset = vdev_slot_bitmap_offset();
    const uint32_t bitmap_size = vdev_slot_bitmap_size();

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

    // Batch-read VDevInfo records for each consecutive run of active slots.
    for (auto [range_start, range_end] : active_ranges) {
        const uint32_t num_slots = range_end - range_start + 1;
        const uint64_t read_off =
            vdev_slot_bitmap_offset() + vdev_slot_bitmap_size() + static_cast< uint64_t >(range_start) * VDevInfo::SIZE;
        const size_t read_size = static_cast< size_t >(num_slots) * VDevInfo::SIZE;

        IOBuffer batch{read_size};
        if (auto ec2 = co_await first_pdev->read_super_block(batch, read_off); ec2) {
            throw std::system_error(ec2, "Failed to read VDevInfo batch");
        }

        for (uint32_t i = 0; i < num_slots; ++i) {
            const uint32_t vdev_id = range_start + i;
            const size_t buf_offset = static_cast< size_t >(i) * VDevInfo::SIZE;

            VDevInfo vinfo{};
            std::memcpy(&vinfo, batch.bytes() + buf_offset, VDevInfo::SIZE);

            if (!vinfo.is_allocated()) {
                // Bitmap says slot is active but VDevInfo says it's free — this is a zombie.
                LOGWARN("Found stale-slot VDev id={} (bitmap set but slot_allocated=0)", vdev_id);
                stale_slot_vdev_ids.push_back(vdev_id);
                continue;
            }

            LOGINFO("Loading VirtualDev id={} name={}", vdev_id, vinfo.get_name());

            // Collect the pdevs that back this vdev (keyed by hs_dev_type in VDevInfo).
            auto backing_pdevs = get_pdevs_by_dev_type(static_cast< HSDevType >(vinfo.hs_dev_type));

            // Create an instance of VirtualDev from the loaded VDevInfo
            auto vdev = VirtualDev::load(vinfo, std::move(backing_pdevs));

            // Register all chunks for this vdev (this will build vdev with each of its chunk allocator)
            if (auto it = all_vdev_chunks.find(vdev_id); it != all_vdev_chunks.end()) {
                vdev->on_chunks_added(std::move(it->second), /*newly_created=*/false);
            }

            // Reconcile vdev_size / num_primary_chunks against loaded chunks.
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
    const uint64_t offset = vdev_slot_bitmap_offset();

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
folly::coro::Task< VDevInfo > DeviceManager::read_vdev_info(const shared< PhysicalDev >& pdev, uint32_t vdev_id) {
    const uint64_t offset =
        vdev_slot_bitmap_offset() + vdev_slot_bitmap_size() + static_cast< uint64_t >(vdev_id) * VDevInfo::SIZE;
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
    const uint64_t max_slots = std::min(bm.total_bits(), static_cast< uint64_t >(HSSuperBlk::MAX_VDEVS_IN_SYSTEM));
    uint64_t cur = 0;

    while (true) {
        cur = bm.get_next_set_bit(cur);
        if (cur == sisl::Bitset::npos || cur >= max_slots) {
            break;
        }

        const uint32_t range_start = static_cast< uint32_t >(cur);

        uint64_t next_reset = bm.get_next_reset_bit(cur + 1);
        const uint32_t range_end = (next_reset == sisl::Bitset::npos || next_reset > max_slots)
            ? static_cast< uint32_t >(max_slots - 1)
            : static_cast< uint32_t >(next_reset - 1);

        ranges.emplace_back(range_start, range_end);

        cur = static_cast< uint64_t >(range_end) + 1;
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
