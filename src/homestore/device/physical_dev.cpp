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

#include <cassert>
#include "common/async.h"
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include "homestore/device/physical_dev.h"
#include "homestore/device/device_manager.h" // DeviceManager: global chunk_id authority
#include "homestore/base/crash_simulator.h"  // is_crash_simulated() write gate
#include "homestore/base/homestore_assert.h"

namespace homestore {

// Per-pdev logging on the device module (mirrors VDEV_LOG), keyed by device name so a pdev's lines group under
// `--log_mods device:trace`.
#define PDEV_LOG(level, ...) HS_SUBMOD_LOG(level, device, , "pdev", get_devname(), ##__VA_ARGS__)

using namespace iomanager;
using sisl::IoBuf;

// ── Global device cache ───────────────────────────────────────────────────────
// We use a plain std::mutex here because open/close are rare, cold-path operations that don't need to yield.
namespace {
std::mutex s_dev_cache_mtx;
std::unordered_map< std::string, shared< IoDevice > > s_dev_cache;
} // namespace

Async< shared< IoDevice > > open_and_cache_dev(const std::string& devname, int oflags) {
    {
        std::lock_guard lg{s_dev_cache_mtx};
        auto it = s_dev_cache.find(devname);
        if (it != s_dev_cache.end()) {
            co_return it->second;
        }
    }

    auto iodev = co_await DriveInterface::open_dev(devname, oflags);
    if (!iodev) {
        throw std::system_error(errno, std::system_category(), "Failed to open device: " + devname);
    }

    std::lock_guard lg{s_dev_cache_mtx};
    // Re-check after the await in case another coroutine raced us.
    auto [it, inserted] = s_dev_cache.emplace(devname, std::move(iodev));
    co_return it->second;
}

Async< void > close_and_uncache_dev(const std::string& devname) {
    std::lock_guard lg{s_dev_cache_mtx};
    s_dev_cache.erase(devname);
    // IoDevice destructor closes the fd when the last shared_ptr drops.
    co_return;
}

// ── Static helpers ────────────────────────────────────────────────────────────

PDevInfoHeader PhysicalDev::create_pdev_info(const DevInfo& dinfo, uint32_t pdev_id) {
    DiskAttr attr{};
    attr.phys_page_size = DiskAttr::DEFAULT_ALIGN_SIZE;
    attr.align_size = DiskAttr::DEFAULT_ALIGN_SIZE;
    attr.atomic_phys_page_size = DiskAttr::DEFAULT_ALIGN_SIZE;
    attr.num_streams = 0;

    PDevInfoHeader hdr;
    hdr.pdev_id = pdev_id;
    hdr.data_offset = HSSuperBlk::total_size(dinfo);
    hdr.size = dinfo.dev_size;
    hdr.max_pdev_chunks = 0; // populated by DeviceManager
    hdr.dev_attr = attr;
    hdr.mirror_super_block = 0x00;
    hdr.system_uuid = boost::uuids::uuid{};
    return hdr;
}

Async< FirstBlock > PhysicalDev::read_first_block(const std::string& devname, int oflags) {
    auto iodev = co_await open_and_cache_dev(devname, oflags);

    DriveInterface di{};
    sisl::IoBufOwn buf{FirstBlock::s_io_fb_size};
    auto ec = co_await di.read(*iodev, buf, HSSuperBlk::first_block_offset());
    if (ec) {
        throw std::system_error(ec, "read_first_block failed on " + devname);
    }

    FirstBlock fb;
    std::memcpy(&fb, buf.bytes(), sizeof(FirstBlock));
    // A valid-magic block with a bad checksum is corruption, never freshness — refuse it rather than let a
    // torn/rotted first block masquerade as either a healthy header or a first-time boot.
    if ((fb.get_magic() == FirstBlock::HOMESTORE_MAGIC) && !fb.verify_checksum()) {
        throw std::runtime_error{"FirstBlock checksum mismatch on " + devname + " — corrupt first block"};
    }
    co_return fb;
}

Async< void > PhysicalDev::write_first_block(const FirstBlockHeader& fbhdr) {
    FirstBlock fb{};
    fb.magic = FirstBlock::HOMESTORE_MAGIC;
    fb.formatting_done = 0x0; // Not yet complete — commit_formatting() sets this to 1
    fb.hdr = fbhdr;
    fb.this_pdev_hdr = pdev_info_;

    // Compute checksum over the atomic portion of the first block (excluding the checksum field itself).
    fb.checksum = 0;
    fb.checksum =
        crc32_ieee(hs_init_crc_32, reinterpret_cast< const unsigned char* >(&fb), FirstBlock::s_atomic_fb_size);

    sisl::IoBufOwn buf{FirstBlock::s_io_fb_size};
    std::memset(buf.bytes(), 0, FirstBlock::s_io_fb_size);
    std::memcpy(buf.bytes(), &fb, sizeof(FirstBlock));

    co_await write_super_block(buf, HSSuperBlk::first_block_offset());
}

Async< void > PhysicalDev::commit_formatting() {
    sisl::IoBufOwn buf{FirstBlock::s_io_fb_size};
    auto ec = co_await read_super_block(buf, HSSuperBlk::first_block_offset());
    if (ec) {
        throw std::system_error(ec, "commit_formatting: failed to read first block on " + devname_);
    }

    auto* fb = reinterpret_cast< FirstBlock* >(buf.bytes());
    fb->formatting_done = 0x1;
    fb->checksum = 0;
    fb->checksum =
        crc32_ieee(hs_init_crc_32, reinterpret_cast< const unsigned char* >(fb), FirstBlock::s_atomic_fb_size);

    co_await write_super_block(buf, HSSuperBlk::first_block_offset());
}

Async< uint64_t > PhysicalDev::get_dev_size(const std::string& devname) {
    auto iodev = co_await open_and_cache_dev(devname, O_RDWR | O_CREAT);
    co_return co_await DriveInterface::get_size(*iodev);
}

// ── Factory: construct (private) ─────────────────────────────────────────────
Async< shared< PhysicalDev > > PhysicalDev::construct(const DevInfo& dinfo, int oflags, const PDevInfoHeader& pinfo) {
    auto pdev = std::make_shared< PhysicalDev >();

    pdev->drive_iface_ = std::make_shared< DriveInterface >();
    pdev->iodev_ = co_await open_and_cache_dev(dinfo.dev_name, oflags);

    const uint64_t dev_size = co_await DriveInterface::get_size(*pdev->iodev_);
    if (dev_size == 0) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "Device " + dinfo.dev_name + " size=0 is too small");
    }

    const uint64_t actual = (dinfo.dev_size == 0) ? dev_size : std::min(dev_size, dinfo.dev_size);

    // Round down to physical page size.
    const uint64_t page = pinfo.dev_attr.phys_page_size;
    const uint64_t rounded = (page > 0) ? (actual / page) * page : actual;
    if (rounded != actual) {
        HS_LOG(INFO, device, "pdev {}: device size={} not a multiple of physical page; adjusted to {}", dinfo.dev_name,
               actual, rounded);
    }

    HS_LOG(INFO, device, "pdev {} opened: size={}", dinfo.dev_name, rounded);

    pdev->devname_ = dinfo.dev_name;
    pdev->dev_type_ = dinfo.dev_type;
    pdev->dev_info_ = dinfo;
    pdev->dev_info_.dev_size = actual;
    pdev->pdev_info_ = pinfo;
    pdev->devsize_ = rounded;
    pdev->super_blk_in_footer_ = (pinfo.mirror_super_block != 0);

    co_return pdev;
}

// ── Factory: create ───────────────────────────────────────────────────────────

Async< shared< PhysicalDev > > PhysicalDev::create(const DevInfo& dinfo, int oflags, uint32_t pdev_id,
                                                   const FirstBlockHeader& fbhdr) {
    auto pinfo = create_pdev_info(dinfo, pdev_id);
    pinfo.system_uuid = fbhdr.system_uuid;
    auto pdev = co_await construct(dinfo, oflags, pinfo);
    co_await pdev->write_first_block(fbhdr);
    co_await pdev->format_chunks();
    co_return pdev;
}

// ── Factory: load ─────────────────────────────────────────────────────────────

Async< shared< PhysicalDev > > PhysicalDev::load(const DevInfo& dinfo, int oflags, const FirstBlockHeader& fbhdr) {
    const auto fb = co_await read_first_block(dinfo.dev_name, oflags);
    if (!fb.is_valid()) {
        LOGCRITICAL("load() is_valid failed: magic={:#x} formatting_done={} product='{}' expected='{}'", fb.magic,
                    fb.formatting_done, fb.hdr.product_name, FirstBlockHeader::PRODUCT_NAME);
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "Invalid first block for device " + dinfo.dev_name);
    }

    // Validate that this device belongs to the same system instance.
    if (fb.this_pdev_hdr.system_uuid != fbhdr.system_uuid) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                fmt::format("Device {} has system_uuid={} but expected={} — possible device swap",
                                            dinfo.dev_name, fb.this_pdev_hdr.get_system_uuid_str(),
                                            fbhdr.get_system_uuid_str()));
    }

    auto pdev = co_await construct(dinfo, oflags, fb.this_pdev_hdr);
    co_await pdev->load_chunks();
    co_return pdev;
}

// ── Super block ───────────────────────────────────────────────────────────────

Async< void > PhysicalDev::write_super_block(const IoBuf& buf, uint64_t offset) {
    if (is_crash_simulated()) {
        co_return; // fake success — the disk stays frozen at the crash instant
    }
    auto ec = co_await drive_iface_->write(*iodev_, buf, offset);
    if (ec) {
        throw std::system_error(ec, "write_super_block failed on " + devname_);
    }
    if (super_blk_in_footer_) {
        const uint64_t t_offset = data_end_offset() + offset;
        ec = co_await drive_iface_->write(*iodev_, buf, t_offset);
        if (ec) {
            throw std::system_error(ec, "write_super_block (footer) failed on " + devname_);
        }
    }
}

Async< std::error_code > PhysicalDev::read_super_block(IoBuf& buf, uint64_t offset) {
    co_return co_await drive_iface_->read(*iodev_, buf, offset);
}

Async< void > PhysicalDev::close_device() {
    // Release this pdev's owning refs to its chunks.  Chunks hold shared<PhysicalDev> back at us, so without this
    // the mutual ownership keeps both sides alive forever (PhysicalDev → chunk_provisioner_.chunks → Chunk → pdev_
    // → PhysicalDev).  After this clear, only VirtualDev's shared<Chunk> entries remain; those are released when
    // DeviceManager drops state_.all_vdevs during its destructor, at which point Chunks die, their pdev_ refs
    // drop, and PhysicalDev itself is freeable.
    {
        auto lock = co_await chunk_mutex_.co_scoped_lock();
        chunk_provisioner_.chunks.clear();
    }
    co_await close_and_uncache_dev(devname_);
}

// ── Data IO ───────────────────────────────────────────────────────────────────

Async< void > PhysicalDev::write(const IoBuf& buf, uint64_t offset) {
    if (is_crash_simulated()) {
        co_return; // fake success — the disk stays frozen at the crash instant
    }
    auto ec = co_await drive_iface_->write(*iodev_, buf, offset);
    if (ec) {
        throw std::system_error(ec, "write failed on " + devname_);
    }
}

Async< std::error_code > PhysicalDev::read(IoBuf& buf, uint64_t offset) {
    co_return co_await drive_iface_->read(*iodev_, buf, offset);
}

Async< void > PhysicalDev::writev(sisl::SgList const& sg, uint64_t offset) {
    if (is_crash_simulated()) {
        co_return; // fake success — the disk stays frozen at the crash instant
    }
    auto ec = co_await drive_iface_->writev(*iodev_, sg, offset);
    if (ec) {
        throw std::system_error(ec, "writev failed on " + devname_);
    }
}

Async< std::error_code > PhysicalDev::readv(sisl::SgList const& sg, uint64_t offset) {
    co_return co_await drive_iface_->readv(*iodev_, sg, offset);
}

Async< void > PhysicalDev::write_zero(uint64_t size, uint64_t offset) {
    if (is_crash_simulated()) {
        co_return; // fake success — the disk stays frozen at the crash instant
    }
    auto ec = co_await drive_iface_->write_zero(*iodev_, size, offset);
    if (ec) {
        throw std::system_error(ec, "write_zero failed on " + devname_);
    }
}

Async< void > PhysicalDev::fsync() {
    if (is_crash_simulated()) {
        co_return; // fake success — the disk stays frozen at the crash instant
    }
    auto ec = co_await drive_iface_->fsync(*iodev_);
    if (ec) {
        throw std::system_error(ec, "fsync failed on " + devname_);
    }
}

// ── Chunk management ─────────────────────────────────────────────────────────

Async< void > PhysicalDev::format_chunks() {
    const uint32_t max_chunks = max_chunks_in_pdev();
    sisl::Bitset bitset{std::max(1u, max_chunks), /* align */ 0};

    const auto bitmap_data = bitset.serialize(pdev_info_.dev_attr.align_size);
    assert(bitmap_data->size() <= chunk_info_bitmap_size());
    co_await write_super_block(*bitmap_data, chunk_sb_offset());

    auto lock = co_await chunk_mutex_.co_scoped_lock();
    chunk_provisioner_.chunk_info_slots = std::make_unique< sisl::Bitset >(std::move(bitset));
}

uint32_t PhysicalDev::alloc_chunk_id_locked(uint64_t cslot) {
    // Standalone single-pdev use (no DeviceManager): the per-pdev slot is itself globally unique, so use it directly.
    if (dev_mgr_ == nullptr) {
        return to_u32(cslot);
    }
    // Multi-pdev: draw a globally-unique id so pdev N's chunks never alias pdev 0's (the old pdev_id*64K+slot formula
    // overflowed the uint16_t chunk_num for pdev_id >= 1).
    auto id = dev_mgr_->allocate_chunk_id();
    if (!id) {
        throw std::out_of_range("No free global chunk_id; system limit of " +
                                std::to_string(MAX_CHUNKS_IN_SYSTEM) + " chunks reached");
    }
    return *id;
}

Async< shared< Chunk > > PhysicalDev::create_chunk(uint32_t vdev_id, uint64_t size, uint64_t vdev_order,
                                                   const uint8_t* user_private_data, size_t up_size) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    if (!prov.chunk_info_slots) {
        throw std::runtime_error("chunk_info_slots not initialised on " + devname_);
    }

    const uint64_t cslot = prov.chunk_info_slots->get_next_reset_bit(0u);
    if (cslot == sisl::Bitset::npos) {
        throw std::out_of_range("No room for additional chunk on " + devname_);
    }
    prov.chunk_info_slots->set_bit(cslot);

    const uint32_t chunk_id = alloc_chunk_id_locked(cslot);

    ChunkInfo cinfo{};
    populate_chunk_info_locked(prov, cinfo, vdev_id, size, chunk_id, vdev_order, user_private_data, up_size);

    // Write this chunk's metadata to the superblock.
    sisl::IoBufOwn cinfo_buf{ChunkInfo::SIZE};
    std::memcpy(cinfo_buf.bytes(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write_super_block(cinfo_buf, chunk_info_offset_nth(to_u32(cslot)));

    auto chunk = std::make_shared< Chunk >(cinfo, to_u32(cslot), shared_from_this());

    prov.chunks.emplace(chunk_id, chunk);

    // Crash point: ChunkInfo durable, slot bit not set — the slot reads as free on recovery and the chunk
    // must vanish harmlessly (any data the caller placed in it is discarded by upper-layer CP gates).
    if (crash_if_flip_fired("crash_after_chunk_info_write")) {
        co_return chunk;
    }

    // Persist the updated bitmap.
    const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
    co_await write_super_block(*bm, chunk_sb_offset());

    PDEV_LOG(INFO, "created chunk: chunk_id={} pdev_id={} cslot={} vdev_id={} vdev_order={}", chunk_id, pdev_id(),
             cslot, vdev_id, vdev_order);
    co_return chunk;
}

Async< std::vector< shared< Chunk > > > PhysicalDev::create_chunks(uint32_t vdev_id, uint32_t num_chunks, uint64_t size,
                                                                   uint64_t start_vdev_order) {
    std::vector< shared< Chunk > > ret_chunks;
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    if (!prov.chunk_info_slots) {
        throw std::runtime_error("chunk_info_slots not initialised on " + devname_);
    }

    uint32_t chunks_remaining = num_chunks;
    uint64_t cur_vdev_order = start_vdev_order;

    while (chunks_remaining > 0) {
        // Find a contiguous run of free slots.
        auto b = prov.chunk_info_slots->get_next_contiguous_n_reset_bits(0u, std::nullopt, 1u, chunks_remaining);
        if (b.nbits == 0) {
            throw std::out_of_range("No room for additional chunks on " + devname_);
        }

        // Build all chunk_infos for this contiguous block.
        sisl::IoBufOwn buf{to_u32(ChunkInfo::SIZE * b.nbits)};
        uint8_t* ptr = buf.bytes();

        std::vector< shared< Chunk > > batch_chunks;
        for (uint32_t i = 0; i < b.nbits; ++i, ptr += ChunkInfo::SIZE) {
            const uint64_t cslot = b.start_bit + i;
            const uint32_t chunk_id = alloc_chunk_id_locked(cslot);
            const uint64_t vdev_order = cur_vdev_order++;

            ChunkInfo cinfo{};
            populate_chunk_info_locked(prov, cinfo, vdev_id, size, chunk_id, vdev_order, nullptr, 0);
            std::memcpy(ptr, cinfo.to_bytes(), ChunkInfo::SIZE);

            auto chunk = std::make_shared< Chunk >(cinfo, to_u32(cslot), shared_from_this());

            prov.chunks.emplace(chunk_id, chunk);
            batch_chunks.push_back(chunk);
            PDEV_LOG(INFO, "created chunk: chunk_id={} pdev_id={} cslot={} vdev_id={} vdev_order={}", chunk_id,
                     pdev_id(), cslot, vdev_id, vdev_order);
        }

        prov.chunk_info_slots->set_bits(b.start_bit, b.nbits);

        // Write the entire batch to disk in one call.
        co_await write_super_block(buf, chunk_info_offset_nth(to_u32(b.start_bit)));

        for (auto& c : batch_chunks) {
            ret_chunks.push_back(c);
        }
        chunks_remaining -= b.nbits;
    }

    // Crash point: the batch's ChunkInfos are durable, no slot bit is set — all of them must vanish
    // harmlessly on recovery.
    if (crash_if_flip_fired("crash_after_chunk_info_write")) {
        co_return ret_chunks;
    }

    // Persist the updated bitmap once for the entire batch.
    const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
    co_await write_super_block(*bm, chunk_sb_offset());

    co_return ret_chunks;
}

Async< std::unordered_map< uint32_t, std::vector< shared< Chunk > > > > PhysicalDev::load_chunks() {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    // Read the chunk slot bitmap from disk.
    const uint32_t bm_size = to_u32(chunk_info_bitmap_size());
    sisl::IoBufOwn bm_buf{bm_size};
    auto ec = co_await drive_iface_->read(*iodev_, bm_buf, chunk_sb_offset());
    if (ec) {
        throw std::system_error(ec, "load_chunks: bitmap read failed");
    }

    // Deserialise bitmap — wrap the IoBuf as a IoBufShared and construct directly (zero-copy).
    sisl::Bitset bitset{sisl::make_io_buf_shared(std::move(bm_buf))};

    std::unordered_map< uint32_t, std::vector< shared< Chunk > > > chunks_by_vdev;

    uint64_t prev_bit = 0;
    for (;;) {
        const uint64_t b = bitset.get_next_set_bit(prev_bit);
        if (b == sisl::Bitset::npos) {
            break;
        }

        // Read the chunk_info for this slot.
        sisl::IoBufOwn ci_buf{ChunkInfo::SIZE};
        auto ec2 = co_await drive_iface_->read(*iodev_, ci_buf, chunk_info_offset_nth(to_u32(b)));
        if (ec2) {
            throw std::system_error(ec2, "load_chunks: chunk_info read failed");
        }

        ChunkInfo cinfo;
        std::memcpy(&cinfo, ci_buf.bytes(), sizeof(ChunkInfo));

        // Verify checksum.
        const uint32_t stored_crc = cinfo.checksum;
        cinfo.checksum = 0;
        const uint32_t computed_crc =
            crc32_ieee(hs_init_crc_32, reinterpret_cast< const unsigned char* >(&cinfo), sizeof(ChunkInfo));
        if (computed_crc != stored_crc) {
            throw std::runtime_error("Checksum mismatch for chunk_info in slot " + std::to_string(b));
        }
        cinfo.checksum = stored_crc;

        prov.chunk_data_area.insert(
            ChunkInterval::right_open(cinfo.chunk_start_offset, cinfo.chunk_start_offset + cinfo.chunk_size));

        auto chunk = std::make_shared< Chunk >(cinfo, to_u32(b), shared_from_this());

        const uint32_t chunk_id = cinfo.chunk_id;
        const uint32_t vdev_id = cinfo.vdev_id;
        prov.chunks.emplace(chunk_id, chunk);
        chunks_by_vdev[vdev_id].push_back(chunk);

        // Rebuild the system-wide chunk-id pool: every loaded chunk (active or pooled) reserves its id.
        if (dev_mgr_ != nullptr) {
            dev_mgr_->mark_chunk_id_used(chunk_id);
        }

        prev_bit = b + 1;
    }
    prov.chunk_info_slots = std::make_unique< sisl::Bitset >(std::move(bitset));

    co_return chunks_by_vdev;
}

Async< void > PhysicalDev::remove_chunk(cshared< Chunk >& chunk) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    ChunkInfo cinfo = chunk->info();
    const uint32_t slot = chunk->slot_number();
    const uint32_t chunk_id = cinfo.chunk_id;

    prov.chunks.erase(chunk_id);
    free_chunk_info_locked(prov, cinfo);

    sisl::IoBufOwn freed_buf{ChunkInfo::SIZE};
    std::memcpy(freed_buf.bytes(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write_super_block(freed_buf, chunk_info_offset_nth(slot));

    // Crash point: the freed ChunkInfo is durable but its slot bit is still set — recovery loads a
    // free-marked record and must not resurrect it as a live (or wrongly pooled) chunk.
    if (crash_if_flip_fired("crash_after_chunk_info_free")) {
        co_return;
    }

    prov.chunk_info_slots->reset_bit(slot);
    const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
    co_await write_super_block(*bm, chunk_sb_offset());

    // Release the global chunk-id now that the chunk is gone from disk.
    if (dev_mgr_ != nullptr) {
        dev_mgr_->free_chunk_id(chunk_id);
    }

    PDEV_LOG(INFO, "removed chunk: chunk_id={}", chunk_id);
    co_return;
}

Async< void > PhysicalDev::remove_chunks(const std::vector< shared< Chunk > >& chunks) {
    if (chunks.empty()) {
        co_return;
    }

    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    for (const auto& chunk : chunks) {
        ChunkInfo cinfo = chunk->info();
        prov.chunks.erase(cinfo.chunk_id);
        free_chunk_info_locked(prov, cinfo);
        sisl::IoBufOwn freed_buf{ChunkInfo::SIZE};
        std::memcpy(freed_buf.bytes(), cinfo.to_bytes(), ChunkInfo::SIZE);
        co_await write_super_block(freed_buf, chunk_info_offset_nth(chunk->slot_number()));
        prov.chunk_info_slots->reset_bit(chunk->slot_number());
        if (dev_mgr_ != nullptr) {
            dev_mgr_->free_chunk_id(cinfo.chunk_id);
        }
    }

    // Crash point: the batch's freed ChunkInfos are durable, their slot bits still set — none of them may
    // resurrect on recovery.
    if (crash_if_flip_fired("crash_after_chunk_info_free")) {
        co_return;
    }

    // Single bitmap write for the entire batch.
    if (prov.chunk_info_slots) {
        const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
        co_await write_super_block(*bm, chunk_sb_offset());
    }
    co_return;
}

Async< void > PhysicalDev::remove_chunks_for_vdev(uint32_t vdev_id) {
    // Collect chunks for this vdev without holding the lock.
    std::vector< shared< Chunk > > to_remove;
    {
        auto lock = co_await chunk_mutex_.co_scoped_lock();
        for (const auto& [id, c] : chunk_provisioner_.chunks) {
            if (c->vdev_id() == vdev_id) {
                to_remove.push_back(c);
            }
        }
    }
    if (!to_remove.empty()) {
        co_await remove_chunks(to_remove);
    }
}

Async< void > PhysicalDev::deactivate_chunk(cshared< Chunk >& chunk) {
    ChunkInfo cinfo = chunk->info();
    cinfo.set_free();
    cinfo.compute_checksum();

    sisl::IoBufOwn buf{ChunkInfo::SIZE};
    std::memcpy(buf.bytes(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write(buf, chunk_info_offset_nth(chunk->slot_number()));

    chunk->update_info(cinfo);
    PDEV_LOG(DEBUG, "deactivated chunk for pooling: chunk_id={}", chunk->chunk_id());
    co_return;
}

Async< void > PhysicalDev::reactivate_chunk(cshared< Chunk >& chunk, uint64_t new_vdev_order) {
    ChunkInfo cinfo = chunk->info();
    cinfo.set_allocated();
    cinfo.chunk_vdev_order = new_vdev_order;
    cinfo.compute_checksum();

    sisl::IoBufOwn buf{ChunkInfo::SIZE};
    std::memcpy(buf.bytes(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write(buf, chunk_info_offset_nth(chunk->slot_number()));

    chunk->update_info(cinfo);
    PDEV_LOG(DEBUG, "reactivated chunk: chunk_id={} vdev_order={}", chunk->chunk_id(), new_vdev_order);
    co_return;
}

// ── Chunk accessors ───────────────────────────────────────────────────────────

Async< std::vector< shared< Chunk > > > PhysicalDev::get_all_chunks() {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    std::vector< shared< Chunk > > result;
    result.reserve(chunk_provisioner_.chunks.size());
    for (const auto& [_, c] : chunk_provisioner_.chunks) {
        result.push_back(c);
    }
    co_return result;
}

Async< shared< Chunk > > PhysicalDev::get_chunk(uint32_t chunk_id) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto it = chunk_provisioner_.chunks.find(chunk_id);
    co_return (it != chunk_provisioner_.chunks.end()) ? it->second : nullptr;
}

Async< std::vector< shared< Chunk > > > PhysicalDev::get_chunks_for_vdev(uint32_t vdev_id) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    std::vector< shared< Chunk > > result;
    for (const auto& [_, c] : chunk_provisioner_.chunks) {
        if (c->vdev_id() == vdev_id) {
            result.push_back(c);
        }
    }
    co_return result;
}

Async< size_t > PhysicalDev::get_chunk_count() {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    co_return chunk_provisioner_.chunks.size();
}

// ── Private locked helpers ────────────────────────────────────────────────────

void PhysicalDev::populate_chunk_info_locked(ChunkProvisioner& prov, ChunkInfo& cinfo, uint32_t vdev_id, uint64_t size,
                                             uint32_t chunk_id, uint64_t vdev_order, const uint8_t* private_data,
                                             size_t private_size) {
    const ChunkInterval ival = find_next_chunk_area_locked(prov.chunk_data_area, size);
    prov.chunk_data_area.insert(ival);

    cinfo.chunk_start_offset = ival.lower();
    cinfo.chunk_size = size;
    cinfo.vdev_id = vdev_id;
    cinfo.chunk_id = chunk_id;
    cinfo.chunk_vdev_order = vdev_order;
    cinfo.set_allocated();
    cinfo.set_user_private(private_data, private_size);
    cinfo.compute_checksum();

    auto [it, inserted] = prov.chunk_start.insert(cinfo.chunk_start_offset);
    if (!inserted) {
        throw std::runtime_error("Duplicate chunk start offset " + std::to_string(cinfo.chunk_start_offset) +
                                 " for chunk " + std::to_string(cinfo.chunk_id));
    }
}

void PhysicalDev::free_chunk_info_locked(ChunkProvisioner& prov, ChunkInfo& cinfo) {
    const ChunkInterval ival =
        ChunkInterval::right_open(cinfo.chunk_start_offset, cinfo.chunk_start_offset + cinfo.chunk_size);
    prov.chunk_data_area.erase(ival);
    prov.chunk_start.erase(cinfo.chunk_start_offset);

    cinfo.set_free();
    cinfo.checksum = 0;
    cinfo.compute_checksum();
}

ChunkInterval PhysicalDev::find_next_chunk_area_locked(const ChunkIntervalSet& data_area, uint64_t size) const {
    // Walk the occupied intervals to find the first gap of at least `size`.
    auto ins = ChunkInterval::right_open(data_start_offset(), data_start_offset() + size);
    for (const auto& existing : data_area) {
        if (ins.upper() <= existing.lower()) {
            break;
        }
        ins = ChunkInterval::right_open(existing.upper(), existing.upper() + size);
    }
    if (ins.upper() > data_end_offset()) {
        throw std::out_of_range("Physical dev has no room for additional chunk");
    }
    return ins;
}

// ── Superblock layout helpers ─────────────────────────────────────────────────

uint64_t PhysicalDev::data_end_offset() const {
    return super_blk_in_footer_ ? (devsize_ - pdev_info_.data_offset) : devsize_;
}

uint64_t PhysicalDev::chunk_info_offset_nth(uint32_t slot) const {
    return chunk_sb_offset() + to_u64(chunk_info_bitmap_size()) + to_u64(slot) * ChunkInfo::SIZE;
}

uint64_t PhysicalDev::chunk_sb_offset() const {
    return HSSuperBlk::chunk_sb_offset();
}

size_t PhysicalDev::chunk_info_bitmap_size() const {
    return to_size(HSSuperBlk::chunk_info_bitmap_size(dev_info_));
}

uint32_t PhysicalDev::max_chunks_in_pdev() const {
    return HSSuperBlk::max_chunks_in_pdev(dev_info_);
}

} // namespace homestore
