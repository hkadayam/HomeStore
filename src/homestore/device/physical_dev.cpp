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
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include <folly/coro/BlockingWait.h>

#include "device/physical_dev.h"

namespace homestore {

// ── Global device cache ───────────────────────────────────────────────────────
// Mirrors Rust's CACHED_OPENED_DEVS (once_cell::Lazy<AsyncMutex<HashMap<...>>>).
// We use a plain std::mutex here because open/close are rare, cold-path operations
// that don't need to yield.
namespace {
std::mutex s_dev_cache_mtx;
std::unordered_map< std::string, std::shared_ptr< IoDevice > > s_dev_cache;
} // namespace

folly::coro::Task< std::shared_ptr< IoDevice > > open_and_cache_dev(std::string devname, int oflags) {
    {
        std::lock_guard lg{s_dev_cache_mtx};
        auto it = s_dev_cache.find(devname);
        if (it != s_dev_cache.end()) { co_return it->second; }
    }

    auto iodev = co_await DriveInterface::open_dev(devname, oflags);
    if (!iodev) { throw std::system_error(errno, std::system_category(), "Failed to open device: " + devname); }

    std::lock_guard lg{s_dev_cache_mtx};
    // Re-check after the await in case another coroutine raced us.
    auto [it, inserted] = s_dev_cache.emplace(devname, std::move(iodev));
    co_return it->second;
}

folly::coro::Task< void > close_and_uncache_dev(std::string devname) {
    std::lock_guard lg{s_dev_cache_mtx};
    s_dev_cache.erase(devname);
    // IoDevice destructor closes the fd when the last shared_ptr drops.
    co_return;
}

// ── Static helpers ────────────────────────────────────────────────────────────

PDevInfoHeader PhysicalDev::create_pdev_info(const dev_info& dinfo, uint32_t pdev_id) {
    // TODO: compute data_offset properly from superblock layout constants
    // (HSSuperBlk::total_size) once HSSuperBlk is decoupled from iomgr.
    const uint64_t data_offset = 8192;

    DiskAttr attr{};
    attr.phys_page_size = 512;
    attr.align_size = 512;
    attr.atomic_phys_page_size = 512;
    attr.num_streams = 0;

    PDevInfoHeader hdr;
    hdr.pdev_id = pdev_id;
    hdr.data_offset = data_offset;
    hdr.size = dinfo.dev_size;
    hdr.max_pdev_chunks = 0; // populated by DeviceManager
    hdr.dev_attr = attr;
    hdr.mirror_super_block = 0x00;
    std::memset(&hdr.system_uuid, 0, sizeof(hdr.system_uuid));
    return hdr;
}

folly::coro::Task< first_block > PhysicalDev::read_first_block(const std::string& devname, int oflags) {
    auto iodev = co_await open_and_cache_dev(devname, oflags);

    DriveInterface di{};
    IOBuffer buf{first_block::s_io_fb_size};
    auto [ec, rbuf] = co_await di.read(*iodev, std::move(buf), HSSuperBlk::first_block_offset());
    if (ec) { throw std::system_error(ec, "read_first_block failed on " + devname); }

    first_block fb;
    std::memcpy(&fb, rbuf.data(), sizeof(first_block));
    co_return fb;
}

folly::coro::Task< uint64_t > PhysicalDev::get_dev_size(const std::string& devname) {
    auto iodev = co_await open_and_cache_dev(devname, O_RDWR | O_CREAT);
    co_return co_await DriveInterface::get_size(*iodev);
}

// ── Factory: construct (private) ─────────────────────────────────────────────
folly::coro::Task< std::shared_ptr< PhysicalDev > > PhysicalDev::construct(dev_info dinfo, int oflags,
                                                                           PDevInfoHeader pinfo) {
    // Allocate via make_shared so enable_shared_from_this works immediately.
    auto pdev = std::make_shared< PhysicalDev >();

    pdev->drive_iface_ = std::make_shared< DriveInterface >();
    pdev->iodev_ = co_await open_and_cache_dev(dinfo.dev_name, oflags);

    const uint64_t dev_size = co_await DriveInterface::get_size(*pdev->iodev_);
    if (dev_size == 0) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "Device " + dinfo.dev_name + " size=0 is too small");
    }

    const uint64_t actual = (dinfo.dev_size == 0) ? dev_size : std::min(dev_size, dinfo.dev_size);

    // Round down to physical page size (mirrors Rust's round_down).
    const uint64_t page = pinfo.dev_attr.phys_page_size;
    const uint64_t rounded = (page > 0) ? (actual / page) * page : actual;
    if (rounded != actual) {
        std::cout << "device size=" << actual << " is not a multiple of physical page; adjusted to " << rounded << "\n";
    }

    std::cout << "Device " << dinfo.dev_name << " opened, size=" << rounded << "\n";

    auto di = dinfo;
    di.dev_size = actual;

    pdev->devname_ = dinfo.dev_name;
    pdev->dev_type_ = dinfo.dev_type;
    pdev->dev_info_ = std::move(di);
    pdev->pdev_info_ = pinfo;
    pdev->devsize_ = rounded;
    pdev->super_blk_in_footer_ = (pinfo.mirror_super_block != 0);

    co_return pdev;
}

// ── Factory: create ───────────────────────────────────────────────────────────

folly::coro::Task< std::shared_ptr< PhysicalDev > > PhysicalDev::create(dev_info dinfo, int oflags, uint32_t pdev_id) {
    auto pinfo = create_pdev_info(dinfo, pdev_id);
    auto pdev = co_await construct(std::move(dinfo), oflags, std::move(pinfo));
    co_await pdev->format_chunks();
    co_return pdev;
}

// ── Factory: load ─────────────────────────────────────────────────────────────

folly::coro::Task< std::shared_ptr< PhysicalDev > > PhysicalDev::load(dev_info dinfo, int oflags) {
    const auto fb = co_await read_first_block(dinfo.dev_name, oflags);
    if (!fb.is_valid()) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                "Invalid first block for device " + dinfo.dev_name);
    }

    auto pdev = co_await construct(std::move(dinfo), oflags, fb.this_pdev_hdr);
    co_await pdev->load_chunks();
    co_return pdev;
}

// ── Super block ───────────────────────────────────────────────────────────────

folly::coro::Task< void > PhysicalDev::write_super_block(const IOBuffer& buf, uint64_t offset) {
    auto ec = co_await drive_iface_->write(*iodev_, buf, offset);
    if (ec) { throw std::system_error(ec, "write_super_block failed on " + devname_); }
    if (super_blk_in_footer_) {
        const uint64_t t_offset = data_end_offset() + offset;
        ec = co_await drive_iface_->write(*iodev_, buf, t_offset);
        if (ec) { throw std::system_error(ec, "write_super_block (footer) failed on " + devname_); }
    }
}

folly::coro::Task< std::error_code > PhysicalDev::read_super_block(IOBuffer& buf, uint64_t offset) {
    co_return co_await drive_iface_->read(*iodev_, buf, offset);
}

folly::coro::Task< void > PhysicalDev::close_device() { co_await close_and_uncache_dev(devname_); }

// ── Data IO ───────────────────────────────────────────────────────────────────

folly::coro::Task< void > PhysicalDev::write(const IOBuffer& buf, uint64_t offset) {
    auto ec = co_await drive_iface_->write(*iodev_, buf, offset);
    if (ec) { throw std::system_error(ec, "write failed on " + devname_); }
}

folly::coro::Task< void > PhysicalDev::writev(std::vector< IOBuffer > bufs, uint64_t offset) {
    auto ec = co_await drive_iface_->writev(*iodev_, std::move(bufs), offset);
    if (ec) { throw std::system_error(ec, "writev failed on " + devname_); }
}

folly::coro::Task< std::error_code > PhysicalDev::read(IOBuffer& buf, uint64_t offset) {
    co_return co_await drive_iface_->read(*iodev_, buf, offset);
}

folly::coro::Task< std::error_code > PhysicalDev::readv(std::vector< IOBuffer >& bufs, uint64_t offset) {
    co_return co_await drive_iface_->readv(*iodev_, bufs, offset);
}

folly::coro::Task< void > PhysicalDev::write_zero(uint64_t size, uint64_t offset) {
    auto ec = co_await drive_iface_->write_zero(*iodev_, size, offset);
    if (ec) { throw std::system_error(ec, "write_zero failed on " + devname_); }
}

folly::coro::Task< void > PhysicalDev::fsync() {
    auto ec = co_await drive_iface_->fsync(*iodev_);
    if (ec) { throw std::system_error(ec, "fsync failed on " + devname_); }
}

// ── Chunk management ─────────────────────────────────────────────────────────

folly::coro::Task< void > PhysicalDev::format_chunks() {
    const uint32_t max_chunks = max_chunks_in_pdev();
    sisl::Bitset bitset{std::max(1u, max_chunks), /* align */ 0};

    const auto bitmap_data = bitset.serialize(pdev_info_.dev_attr.align_size);
    assert(bitmap_data->size() <= chunk_info_bitmap_size());

    IOBuffer sb_buf{bitmap_data->size()};
    std::memcpy(sb_buf.data(), bitmap_data->cbytes(), bitmap_data->size());
    co_await write_super_block(sb_buf, chunk_sb_offset());

    auto lock = co_await chunk_mutex_.co_scoped_lock();
    chunk_provisioner_.chunk_info_slots = std::make_unique< sisl::Bitset >(std::move(bitset));
}

folly::coro::Task< std::shared_ptr< Chunk > > PhysicalDev::create_chunk(uint32_t vdev_id, uint64_t size,
                                                                        uint32_t ordinal,
                                                                        const uint8_t* user_private_data,
                                                                        size_t up_size) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    if (!prov.chunk_info_slots) { throw std::runtime_error("chunk_info_slots not initialised on " + devname_); }

    const uint64_t cslot = prov.chunk_info_slots->get_next_reset_bit(0u);
    if (cslot == sisl::Bitset::npos) { throw std::out_of_range("No room for additional chunk on " + devname_); }
    prov.chunk_info_slots->set_bit(cslot);

    // chunk_id = (pdev_id + 1) * slot  — mirrors Rust's formula
    const uint32_t chunk_id = static_cast< uint32_t >((pdev_id() + 1ULL) * cslot);

    ChunkInfo cinfo{};
    populate_chunk_info_locked(prov, cinfo, vdev_id, size, chunk_id, ordinal, user_private_data, up_size);

    // Write this chunk's metadata to the superblock.
    IOBuffer cinfo_buf{ChunkInfo::SIZE};
    std::memcpy(cinfo_buf.data(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write_super_block(cinfo_buf, chunk_info_offset_nth(static_cast< uint32_t >(cslot)));

    // Mirrors Rust: Arc::new(Chunk::new(cinfo, cslot as u32, Arc::clone(self)))
    auto chunk = std::make_shared< Chunk >(cinfo, static_cast< uint32_t >(cslot), shared_from_this());

    prov.chunks.emplace(chunk_id, chunk);

    // Persist the updated bitmap.
    const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
    IOBuffer bm_buf{bm->size()};
    std::memcpy(bm_buf.data(), bm->cbytes(), bm->size());
    co_await write_super_block(bm_buf, chunk_sb_offset());

    std::cout << "Created chunk " << chunk_id << " (slot " << cslot << ", vdev " << vdev_id << ")\n";
    co_return chunk;
}

folly::coro::Task< std::vector< std::shared_ptr< Chunk > > >
PhysicalDev::create_chunks(uint32_t vdev_id, uint32_t num_chunks, uint64_t size, uint32_t start_ordinal) {
    std::vector< std::shared_ptr< Chunk > > ret_chunks;
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    if (!prov.chunk_info_slots) { throw std::runtime_error("chunk_info_slots not initialised on " + devname_); }

    uint32_t chunks_remaining = num_chunks;
    uint32_t cur_ordinal = start_ordinal;

    while (chunks_remaining > 0) {
        // Find a contiguous run of free slots.
        auto b = prov.chunk_info_slots->get_next_contiguous_n_reset_bits(0u, std::nullopt, 1u, chunks_remaining);
        if (b.nbits == 0) { throw std::out_of_range("No room for additional chunks on " + devname_); }

        // Build all chunk_infos for this contiguous block.
        IOBuffer buf{ChunkInfo::SIZE * b.nbits};
        uint8_t* ptr = buf.data();

        std::vector< std::shared_ptr< Chunk > > batch_chunks;
        for (uint32_t i = 0; i < b.nbits; ++i, ptr += ChunkInfo::SIZE) {
            const uint64_t cslot = b.start_bit + i;
            const uint32_t chunk_id = static_cast< uint32_t >((pdev_id() + 1ULL) * cslot);
            const uint32_t ordinal = cur_ordinal++;

            ChunkInfo cinfo{};
            populate_chunk_info_locked(prov, cinfo, vdev_id, size, chunk_id, ordinal, nullptr, 0);
            std::memcpy(ptr, cinfo.to_bytes(), ChunkInfo::SIZE);

            auto chunk = std::make_shared< Chunk >(cinfo, static_cast< uint32_t >(cslot), shared_from_this());

            prov.chunks.emplace(chunk_id, chunk);
            batch_chunks.push_back(chunk);
            std::cout << "Creating chunk " << chunk_id << " (slot " << cslot << ")\n";
        }

        prov.chunk_info_slots->set_bits(b.start_bit, b.nbits);

        // Write the entire batch to disk in one call.
        co_await write_super_block(buf, chunk_info_offset_nth(static_cast< uint32_t >(b.start_bit)));

        for (auto& c : batch_chunks) {
            ret_chunks.push_back(c);
        }
        chunks_remaining -= b.nbits;
    }

    // Persist the updated bitmap once for the entire batch.
    const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
    IOBuffer bm_buf{bm->size()};
    std::memcpy(bm_buf.data(), bm->cbytes(), bm->size());
    co_await write_super_block(bm_buf, chunk_sb_offset());

    co_return ret_chunks;
}

folly::coro::Task< std::unordered_map< uint32_t, std::vector< std::shared_ptr< Chunk > > > >
PhysicalDev::load_chunks() {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    // Read the chunk slot bitmap from disk.
    const size_t bm_size = chunk_info_bitmap_size();
    IOBuffer bm_buf{bm_size};
    auto [ec, bm_rbuf] = co_await drive_iface_->read(*iodev_, std::move(bm_buf), chunk_sb_offset());
    if (ec) { throw std::system_error(ec, "load_chunks: bitmap read failed"); }

    // Deserialise bitmap (zero-copy from IOBuffer).
    auto [bitset, _set_count] = sisl::Bitset::load(bm_rbuf.data(), bm_rbuf.size());

    std::unordered_map< uint32_t, std::vector< std::shared_ptr< Chunk > > > chunks_by_vdev;

    uint64_t prev_bit = 0;
    for (;;) {
        const uint64_t b = bitset.get_next_set_bit(prev_bit);
        if (b == sisl::Bitset::npos) { break; }

        // Read the chunk_info for this slot.
        IOBuffer ci_buf{ChunkInfo::SIZE};
        auto [ec2, ci_rbuf] =
            co_await drive_iface_->read(*iodev_, std::move(ci_buf), chunk_info_offset_nth(static_cast< uint32_t >(b)));
        if (ec2) { throw std::system_error(ec2, "load_chunks: chunk_info read failed"); }

        ChunkInfo cinfo;
        std::memcpy(&cinfo, ci_rbuf.data(), sizeof(ChunkInfo));

        // Verify checksum (mirrors Rust's CRC check).
        const uint16_t stored_crc = cinfo.checksum;
        cinfo.checksum = 0;
        const uint16_t computed_crc =
            crc16_t10dif(hs_init_crc_16, reinterpret_cast< const unsigned char* >(&cinfo), sizeof(ChunkInfo));
        if (computed_crc != stored_crc) {
            throw std::runtime_error("Checksum mismatch for chunk_info in slot " + std::to_string(b));
        }
        cinfo.checksum = stored_crc;

        prov.chunk_data_area.insert(
            ChunkInterval::right_open(cinfo.chunk_start_offset, cinfo.chunk_start_offset + cinfo.chunk_size));

        auto chunk = std::make_shared< Chunk >(cinfo, static_cast< uint32_t >(b), shared_from_this());

        const uint32_t chunk_id = cinfo.chunk_id;
        const uint32_t vdev_id = cinfo.vdev_id;
        prov.chunks.emplace(chunk_id, chunk);
        chunks_by_vdev[vdev_id].push_back(chunk);

        prev_bit = b + 1;
    }
    prov.chunk_info_slots = std::make_unique< sisl::Bitset >(std::move(bitset));

    co_return chunks_by_vdev;
}

folly::coro::Task< void > PhysicalDev::remove_chunk(const std::shared_ptr< Chunk >& chunk) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    ChunkInfo cinfo = chunk->info();
    const uint32_t slot = chunk->slot_number();
    const uint32_t chunk_id = cinfo.chunk_id;

    prov.chunks.erase(chunk_id);
    free_chunk_info_locked(prov, cinfo);

    IOBuffer freed_buf{ChunkInfo::SIZE};
    std::memcpy(freed_buf.data(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write_super_block(freed_buf, chunk_info_offset_nth(slot));

    prov.chunk_info_slots->reset_bit(slot);
    const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
    IOBuffer bm_buf{bm->size()};
    std::memcpy(bm_buf.data(), bm->cbytes(), bm->size());
    co_await write_super_block(bm_buf, chunk_sb_offset());

    std::cout << "Removed chunk " << chunk_id << "\n";
    co_return;
}

folly::coro::Task< void > PhysicalDev::remove_chunks(const std::vector< std::shared_ptr< Chunk > >& chunks) {
    if (chunks.empty()) { co_return; }

    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto& prov = chunk_provisioner_;

    for (const auto& chunk : chunks) {
        ChunkInfo cinfo = chunk->info();
        prov.chunks.erase(cinfo.chunk_id);
        free_chunk_info_locked(prov, cinfo);
        IOBuffer freed_buf{ChunkInfo::SIZE};
        std::memcpy(freed_buf.data(), cinfo.to_bytes(), ChunkInfo::SIZE);
        co_await write_super_block(freed_buf, chunk_info_offset_nth(chunk->slot_number()));
        prov.chunk_info_slots->reset_bit(chunk->slot_number());
    }

    // Single bitmap write for the entire batch — mirrors Rust's batched approach.
    if (prov.chunk_info_slots) {
        const auto bm = prov.chunk_info_slots->serialize(pdev_info_.dev_attr.align_size);
        IOBuffer bm_buf{bm->size()};
        std::memcpy(bm_buf.data(), bm->cbytes(), bm->size());
        co_await write_super_block(bm_buf, chunk_sb_offset());
    }
    co_return;
}

folly::coro::Task< void > PhysicalDev::remove_chunks_for_vdev(uint32_t vdev_id) {
    // Collect chunks for this vdev without holding the lock.
    std::vector< std::shared_ptr< Chunk > > to_remove;
    {
        auto lock = co_await chunk_mutex_.co_scoped_lock();
        for (const auto& [id, c] : chunk_provisioner_.chunks) {
            if (c->vdev_id() == vdev_id) { to_remove.push_back(c); }
        }
    }
    if (!to_remove.empty()) { co_await remove_chunks(to_remove); }
}

folly::coro::Task< void > PhysicalDev::deactivate_chunk(const std::shared_ptr< Chunk >& chunk) {
    ChunkInfo cinfo = chunk->info();
    cinfo.set_free();
    cinfo.compute_checksum();

    IOBuffer buf{ChunkInfo::SIZE};
    std::memcpy(buf.data(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write(buf, chunk_info_offset_nth(chunk->slot_number()));

    chunk->update_info(cinfo);
    std::cout << "Deactivated chunk " << chunk->chunk_id() << " for pooling\n";
    co_return;
}

folly::coro::Task< void > PhysicalDev::reactivate_chunk(const std::shared_ptr< Chunk >& chunk,
                                                        uint32_t new_creation_order) {
    ChunkInfo cinfo = chunk->info();
    cinfo.set_allocated();
    cinfo.chunk_creation_order = new_creation_order;
    cinfo.compute_checksum();

    IOBuffer buf{ChunkInfo::SIZE};
    std::memcpy(buf.data(), cinfo.to_bytes(), ChunkInfo::SIZE);
    co_await write(buf, chunk_info_offset_nth(chunk->slot_number()));

    chunk->update_info(cinfo);
    std::cout << "Reactivated chunk " << chunk->chunk_id() << " with creation_order=" << new_creation_order << "\n";
    co_return;
}

// ── Chunk accessors ───────────────────────────────────────────────────────────

folly::coro::Task< std::vector< std::shared_ptr< Chunk > > > PhysicalDev::get_all_chunks() {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    std::vector< std::shared_ptr< Chunk > > result;
    result.reserve(chunk_provisioner_.chunks.size());
    for (const auto& [_, c] : chunk_provisioner_.chunks) {
        result.push_back(c);
    }
    co_return result;
}

folly::coro::Task< std::shared_ptr< Chunk > > PhysicalDev::get_chunk(uint32_t chunk_id) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    auto it = chunk_provisioner_.chunks.find(chunk_id);
    co_return (it != chunk_provisioner_.chunks.end()) ? it->second : nullptr;
}

folly::coro::Task< std::vector< std::shared_ptr< Chunk > > > PhysicalDev::get_chunks_for_vdev(uint32_t vdev_id) {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    std::vector< std::shared_ptr< Chunk > > result;
    for (const auto& [_, c] : chunk_provisioner_.chunks) {
        if (c->vdev_id() == vdev_id) { result.push_back(c); }
    }
    co_return result;
}

folly::coro::Task< size_t > PhysicalDev::get_chunk_count() {
    auto lock = co_await chunk_mutex_.co_scoped_lock();
    co_return chunk_provisioner_.chunks.size();
}

// ── Private locked helpers ────────────────────────────────────────────────────

void PhysicalDev::populate_chunk_info_locked(ChunkProvisioner& prov, ChunkInfo& cinfo, uint32_t vdev_id, uint64_t size,
                                             uint32_t chunk_id, uint32_t ordinal, const uint8_t* private_data,
                                             size_t private_size) {
    const ChunkInterval ival = find_next_chunk_area_locked(prov.chunk_data_area, size);
    prov.chunk_data_area.insert(ival);

    cinfo.chunk_start_offset = ival.lower();
    cinfo.chunk_size = size;
    cinfo.vdev_id = vdev_id;
    cinfo.chunk_id = chunk_id;
    cinfo.chunk_creation_order = ordinal;
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
    // Mirrors Rust's find_next_chunk_area_locked().
    auto ins = ChunkInterval::right_open(data_start_offset(), data_start_offset() + size);
    for (const auto& existing : data_area) {
        if (ins.upper() <= existing.lower()) { break; }
        ins = ChunkInterval::right_open(existing.upper(), existing.upper() + size);
    }
    if (ins.upper() > data_end_offset()) { throw std::out_of_range("Physical dev has no room for additional chunk"); }
    return ins;
}

// ── Superblock layout helpers ─────────────────────────────────────────────────

uint64_t PhysicalDev::data_end_offset() const {
    return super_blk_in_footer_ ? (devsize_ - pdev_info_.data_offset) : devsize_;
}

uint64_t PhysicalDev::chunk_info_offset_nth(uint32_t slot) const {
    return chunk_sb_offset() + static_cast< uint64_t >(chunk_info_bitmap_size()) +
        static_cast< uint64_t >(slot) * ChunkInfo::SIZE;
}

uint64_t PhysicalDev::chunk_sb_offset() const { return HSSuperBlk::chunk_sb_offset(); }

size_t PhysicalDev::chunk_info_bitmap_size() const {
    return static_cast< size_t >(HSSuperBlk::chunk_info_bitmap_size(dev_info_));
}

uint32_t PhysicalDev::max_chunks_in_pdev() const { return HSSuperBlk::max_chunks_in_pdev(dev_info_); }

} // namespace homestore
