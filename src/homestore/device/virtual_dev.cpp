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
#include <random>
#include <stdexcept>
#include <system_error>

#include "sisl/logging/logging.h"
#include "blkalloc/slab_blk_allocator.h"
#include "device/hs_super_blk.h" // HSSuperBlk layout constants
#include "device/physical_dev.h" // PhysicalDev
#include "device/virtual_dev.h"

namespace homestore {
using namespace blkalloc;

// ──────────────────────────────────────────────────────────────────────────────
// Constructors and Factory Methods (create/load)
// ──────────────────────────────────────────────────────────────────────────────

// Constructor: Initialize immutable fields from persisted/created VDevInfo and store the pdevs that back this vdev.
VirtualDev::VirtualDev(VDevInfo info, std::vector< shared< PhysicalDev > > pdevs) :
        name_{info.get_name()},
        vdev_id_{info.vdev_id},
        hs_dev_type_{static_cast< HSDevType >(info.hs_dev_type)},
        blk_size_{info.blk_size},
        multi_pdev_choice_{static_cast< MultiPDevOpts >(info.multi_pdev_choice)},
        allocator_type_{static_cast< BlkAllocatorType >(info.alloc_type)},
        chunk_selector_type_{static_cast< ChunkSelectorType >(info.chunk_sel_type)},
        persist_blk_alloced_{info.persist_blk_alloced != 0},
        incremental_chunk_size_{to_u64(info.chunk_size)},
        pdevs_{std::move(pdevs)},
        mutable_state_() {
    VDevMutableState initial{};
    initial.vdev_info = info;
    store_state(std::move(initial));
}

folly::coro::Task< unique< VirtualDev > > VirtualDev::create(VDevParameters&& params, uint32_t vdev_id,
                                                             const std::vector< shared< PhysicalDev > >& pdevs) {
    if (pdevs.empty()) {
        throw std::invalid_argument("No pdevs available; cannot create vdev " + params.vdev_name);
    }

    const uint32_t align_size = pdevs[0]->align_size();
    if (params.blk_size % align_size != 0) {
        throw std::invalid_argument("blk_size " + std::to_string(params.blk_size) +
                                    " must be a multiple of pdev align_size " + std::to_string(align_size));
    }

    auto selected_pdevs = pick_pdevs(pdevs, params.multi_pdev_opts);
    adjust_vdev_params(params); // Normalise params (no-op when num_chunks == 0).

    if (params.num_chunks == 0) {
        LOGINFO("New VirtualDev={} id={} (no initial chunks)", params.vdev_name, vdev_id);
    } else {
        LOGINFO("New VirtualDev={} size={} id={} chunks={} chunk_size={}", params.vdev_name, params.vdev_size, vdev_id,
                params.num_chunks, params.chunk_size);
    }

    // Build VDevInfo from (possibly adjusted) params
    VDevInfo vinfo{};
    vinfo.vdev_id = vdev_id;
    vinfo.chunk_size = to_u32(params.chunk_size);
    vinfo.blk_size = params.blk_size;
    vinfo.num_mirrors = params.num_mirrors;
    vinfo.slot_allocated = 0x01;
    vinfo.hs_dev_type = to_u8(params.dev_type);
    vinfo.multi_pdev_choice = to_u8(params.multi_pdev_opts);
    vinfo.alloc_type = to_u8(params.alloc_type);
    vinfo.chunk_sel_type = to_u8(params.chunk_sel_type);
    vinfo.persist_blk_alloced = params.persist_blk_alloced ? 1 : 0;
    vinfo.set_name(params.vdev_name);
    vinfo.compute_checksum();

    auto vdev = unique< VirtualDev >{new VirtualDev{vinfo, std::move(selected_pdevs)}};
    if (params.chunk_pool_limit) {
        vdev->chunk_pool_.emplace(*params.chunk_pool_limit);
    }

    // Distribute chunks across pdevs proportionally by capacity.
    const uint64_t total_size = [vdev = vdev.get()]() {
        uint64_t s = 0;
        for (auto& p : vdev->pdevs_) {
            s += p->data_size();
        }
        return s;
    }();

    uint32_t total_created = 0;
    for (size_t pi = 0; pi < vdev->pdevs_.size(); ++pi) {
        if (total_created >= params.num_chunks) { break; }
        auto& pdev = vdev->pdevs_[pi];

        uint32_t n = to_u32(params.num_chunks * (to_double(pdev->data_size()) / to_double(total_size)));

        // First pdev picks up any chunks lost to rounding so that chunk IDs stay small.
        if (n == 0 && total_created == 0) { n = 1; }
        n = std::min(n, params.num_chunks - total_created);
        if (n == 0) { continue; }

        auto chunks = co_await pdev->create_chunks(vdev_id, n, params.chunk_size, /*start_ordinal=*/total_created);
        vdev->on_chunks_added(std::move(chunks), /*newly_created=*/true);
        total_created += n;
    }

    // Write the newly minted vdev info
    co_await vdev->write_vdev_info();

    LOGINFO("VirtualDev={} size={} created", params.vdev_name, params.vdev_size);
    co_return vdev;
}

unique< VirtualDev > VirtualDev::load(VDevInfo vinfo, std::vector< shared< PhysicalDev > > pdevs) {
    // Constructor populates immutable fields from vinfo and stores pdevs. Pdev load then calls on_chunk_added() for
    // each chunk, which creates blk_allocator selector. Then upper layer will load the chunk blk_allocator with
    // load_blk_allocator()
    return unique< VirtualDev >{new VirtualDev{vinfo, std::move(pdevs)}};
}

// ──────────────────────────────────────────────────────────────────────────────
// Public APIs: Device Resizing section with chunks
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< shared< Chunk > > VirtualDev::expand(uint64_t chunk_size) {
    shared< Chunk > chunk = nullptr;
    uint64_t vdev_order;
    {
        std::unique_lock lk{chunk_mgmt_mutex_};
        {
            auto cur = load_state();
            vdev_order = cur->next_vdev_order;
        }

        if (chunk_pool_) {
            chunk = chunk_pool_->try_get_chunk(chunk_size);
            if (chunk != nullptr) {
                LOGDEBUG("Reusing pooled chunk {} with vdev_order={}", chunk->chunk_id(), vdev_order);
                co_await chunk->physical_dev()->reactivate_chunk(chunk, vdev_order);
            }
        }
    }

    if (chunk == nullptr) {
        // No pooled chunk available: allocate a new one on the pdev with the most free space.
        if (pdevs_.empty()) {
            throw std::runtime_error("No pdevs available for expand in vdev '" + name_ + "'");
        }
        shared< PhysicalDev > best_pdev = pdevs_[0];
        for (size_t i = 1; i < pdevs_.size(); ++i) {
            if (pdevs_[i]->data_size() > best_pdev->data_size()) {
                best_pdev = pdevs_[i];
            }
        }
        chunk = co_await best_pdev->create_chunk(vdev_id_, chunk_size, vdev_order);
    }

    on_chunk_added(chunk, /*newly_created=*/true);
    co_return chunk;
}

folly::coro::Task< uint32_t > VirtualDev::shrink(ChunkToShrink which, uint32_t specific_chunk_id) {
    shared< Chunk > chunk;
    {
        std::lock_guard lk{chunk_mgmt_mutex_};
        auto cur = load_state();
        if (which == ChunkToShrink::Last) {
            if (cur->chunks_by_vdev_order.empty()) {
                throw std::out_of_range("No chunks to shrink in vdev '" + name_ + "'");
            }
            chunk = cur->chunks_by_vdev_order.back();
        } else {
            auto it = cur->all_chunks.find(specific_chunk_id);
            if (it == cur->all_chunks.end()) {
                throw std::out_of_range("Chunk " + std::to_string(specific_chunk_id) + " not found in vdev '" + name_ +
                                        "'");
            }
            chunk = it->second;
        }
    }

    const uint32_t chunk_id = chunk->chunk_id();
    on_chunk_removed(chunk);

    auto& pdev = chunk->physical_dev();
    if (chunk_pool_ && chunk_pool_->has_room(chunk->info().chunk_size)) {
        co_await pdev->deactivate_chunk(chunk);
        chunk_pool_->return_chunk(chunk);
        LOGDEBUG("Chunk {} deactivated and moved to pool", chunk_id);
    } else {
        co_await pdev->remove_chunk(chunk);
        LOGDEBUG("Chunk {} removed", chunk_id);
    }

    co_return chunk_id;
}

folly::coro::Task< void > VirtualDev::destroy() {
    std::lock_guard lk{chunk_mgmt_mutex_};
    LOGINFO("Destroying VirtualDev '{}' (id={})", name_, vdev_id_);

    // Stage 1: mark free and persist.
    {
        auto new_state = clone_state();
        new_state.vdev_info.set_free();
        new_state.vdev_info.compute_checksum();
        store_state(std::move(new_state));
        co_await write_vdev_info();
    }
    LOGINFO("VirtualDev '{}': stage 1 complete", name_);

    // Stage 2: remove all chunks belonging to this vdev from every pdev.
    for (auto& pdev : pdevs_) {
        co_await pdev->remove_chunks_for_vdev(vdev_id_);
    }

    LOGINFO("VirtualDev '{}' fully destroyed", name_);
}

// ──────────────────────────────────────────────────────────────────────────────
// Public APIs: I/Os
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > VirtualDev::write(const IOBuffer& buf, const BlkId& bid) {
    auto [dev_offset, chunk] = to_dev_offset(bid);
    VDEV_LOG(DEBUG, name_, "write: blk_num={} nblks={} chunk={} dev_offset={} buf_size={}", bid.blk_num(),
             bid.blk_count(), bid.chunk_num(), dev_offset, buf.size());
    co_await chunk->physical_dev()->write(buf, dev_offset);
}

folly::coro::Task< void > VirtualDev::writev(const std::vector< IOBuffer >& bufs, const BlkId& bid) {
    auto [dev_offset, chunk] = to_dev_offset(bid);
    co_await chunk->physical_dev()->writev(bufs, dev_offset);
}

folly::coro::Task< void > VirtualDev::writev(const std::vector< sisl::ByteArray >& bufs, const BlkId& bid) {
    auto [dev_offset, chunk] = to_dev_offset(bid);
    co_await chunk->physical_dev()->writev(bufs, dev_offset);
}

folly::coro::Task< std::error_code > VirtualDev::read(IOBuffer& buf, const BlkId& bid) {
    auto [dev_offset, chunk] = to_dev_offset(bid);
    VDEV_LOG(DEBUG, name_, "read: blk_num={} nblks={} chunk={} dev_offset={} buf_size={}", bid.blk_num(),
             bid.blk_count(), bid.chunk_num(), dev_offset, buf.size());
    co_return co_await chunk->physical_dev()->read(buf, dev_offset);
}

folly::coro::Task< std::error_code > VirtualDev::readv(std::vector< IOBuffer >& bufs, const BlkId& bid) {
    auto [dev_offset, chunk] = to_dev_offset(bid);
    co_return co_await chunk->physical_dev()->readv(bufs, dev_offset);
}

folly::coro::Task< void > VirtualDev::format() {
    auto state = load_state();
    for (auto& [chunk_id, chunk] : state->all_chunks) {
        co_await chunk->physical_dev()->write_zero(chunk->info().chunk_size, chunk->start_offset());
    }
    LOGINFO("VirtualDev '{}' formatted", name_);
}

folly::coro::Task< void > VirtualDev::fsync() {
    auto state = load_state();
    std::unordered_set< uint32_t > seen;
    for (auto& [_, chunk] : state->all_chunks) {
        const uint32_t pdev_id = chunk->physical_dev()->pdev_id();
        if (seen.emplace(pdev_id).second) {
            co_await chunk->physical_dev()->fsync();
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Public APIs: Block Allocations
// ──────────────────────────────────────────────────────────────────────────────
BlkAllocStatus VirtualDev::alloc_contiguous_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkId& out_blkid) {
    blk_alloc_hints contig_hints = hints;
    contig_hints.is_contiguous = true;

    BlkIds blkids;
    BlkAllocStatus st = alloc_blks(nblks, contig_hints, blkids);
    if (st == BlkAllocStatus::SUCCESS && blkids.size() == 1) {
        out_blkid = blkids.front();
    }
    return st;
}

BlkAllocStatus VirtualDev::alloc_blks(blk_count_t nblks, const blk_alloc_hints& hints, BlkIds& out_blkids) {
    auto state = load_state();
    const uint64_t max_attempts = hints.chunk_id_hint.has_value() ? 1 : state->total_chunk_num;
    std::optional< uint32_t > last_failed;
    uint64_t attempt = 0;
    blk_count_t remaining = nblks;

    while (remaining > 0) {
        auto chunk = select_chunk_for_alloc(remaining, hints, last_failed);
        if (!chunk) {
            return out_blkids.empty() ? BlkAllocStatus::SPACE_FULL : BlkAllocStatus::PARTIAL;
        }
        if (!chunk->has_blk_allocator()) {
            return BlkAllocStatus::FAILED;
        }

        BlkIds blkids;
        BlkAllocStatus st = chunk->blk_allocator_mutable()->alloc(remaining, hints, blkids);
        if (st == BlkAllocStatus::SUCCESS || (st == BlkAllocStatus::PARTIAL && hints.partial_alloc_ok)) {
            for (auto const& bid : blkids) {
                out_blkids.push_back(bid);
                remaining -= bid.blk_count();
            }
            continue;
        }
        if (!hints.can_look_for_other_chunk || hints.chunk_id_hint.has_value()) {
            return out_blkids.empty() ? st : BlkAllocStatus::PARTIAL;
        }
        if (++attempt >= max_attempts) {
            return out_blkids.empty() ? BlkAllocStatus::SPACE_FULL : BlkAllocStatus::PARTIAL;
        }
        last_failed = chunk->chunk_id();
    }
    return BlkAllocStatus::SUCCESS;
}

void VirtualDev::free_blk(const BlkId& bid) {
    auto state = load_state();
    auto it = state->all_chunks.find(bid.chunk_num());
    if (it == state->all_chunks.end()) {
        LOGERROR("free_blk: missing chunk {}", bid.chunk_num());
        return;
    }
    if (it->second->has_blk_allocator()) {
        it->second->blk_allocator_mutable()->free(bid);
    }
}

BlkAllocStatus VirtualDev::commit_blk(const BlkId& bid) {
    auto state = load_state();
    auto it = state->all_chunks.find(bid.chunk_num());
    if (it == state->all_chunks.end()) {
        return BlkAllocStatus::INVALID_DEV;
    }
    if (!it->second->has_blk_allocator()) {
        return BlkAllocStatus::FAILED;
    }
    return it->second->blk_allocator_mutable()->commit(bid);
}

void VirtualDev::recovery_completed() {
    auto state = load_state();
    for (auto& [_, c] : state->all_chunks) {
        if (c->has_blk_allocator()) { c->blk_allocator_mutable()->recovery_completed(); }
    }
}

void VirtualDev::init_blk_allocator(cshared< Chunk >& chunk) {
    if (chunk) {
        construct_blk_allocator(chunk, std::nullopt);
    } else {
        auto state = load_state();
        for (auto& [_, c] : state->all_chunks) {
            construct_blk_allocator(c, std::nullopt);
        }
    }
}

void VirtualDev::load_blk_allocator(const std::unordered_map< uint32_t, sisl::ByteArray >& chunk_buffers) {
    auto state = load_state();
    for (auto& [chunk_id, c] : state->all_chunks) {
        auto it = chunk_buffers.find(chunk_id);
        if (it != chunk_buffers.end()) {
            construct_blk_allocator(c, it->second);
        } else {
            construct_blk_allocator(c);
        }
    }
}

void VirtualDev::load_blk_allocator(uint32_t chunk_id, const sisl::ByteArray& buffer) {
    auto state = load_state();
    auto it = state->all_chunks.find(chunk_id);
    if (it == state->all_chunks.end()) {
        LOGWARN("load_blk_allocator: chunk_id={} not found in VDev '{}'", chunk_id, name_);
        return;
    }
    construct_blk_allocator(it->second, buffer);
}

// ──────────────────────────────────────────────────────────────────────────────
// Public APIs: Getters
// ──────────────────────────────────────────────────────────────────────────────
std::vector< shared< Chunk > > VirtualDev::get_chunks() const {
    auto state = load_state();
    std::vector< shared< Chunk > > result;
    result.reserve(state->all_chunks.size());
    for (auto& [_, c] : state->all_chunks) {
        result.push_back(c);
    }
    return result;
}

std::vector< shared< Chunk > > VirtualDev::get_chunks_by_vdev_order() const {
    return load_state()->chunks_by_vdev_order;
}

shared< Chunk > VirtualDev::get_nth_chunk(size_t n) const {
    auto& v = load_state()->chunks_by_vdev_order;
    return (n < v.size()) ? v[n] : nullptr;
}

shared< Chunk > VirtualDev::get_chunk(uint32_t chunk_id) const {
    auto& m = load_state()->all_chunks;
    auto it = m.find(chunk_id);
    return (it != m.end()) ? it->second : nullptr;
}

uint64_t VirtualDev::size() const {
    return load_state()->total_vdev_size;
}
uint64_t VirtualDev::num_chunks() const {
    return load_state()->total_chunk_num;
}
uint64_t VirtualDev::chunk_size_bytes() const {
    return load_state()->vdev_info.chunk_size;
}
VDevInfo VirtualDev::get_vdev_info() const {
    return load_state()->vdev_info;
}

// ──────────────────────────────────────────────────────────────────────────────
// Private Helpers - Initializers
// ──────────────────────────────────────────────────────────────────────────────
void VirtualDev::adjust_vdev_params(VDevParameters& p) {
    constexpr uint64_t MIN_CHUNK_SIZE = 16ull * 1024 * 1024;
    constexpr uint32_t MAX_CHUNKS_IN_SYSTEM = 65535;

    // Empty dynamic vdev starting with no chunks — nothing to adjust.
    if (p.num_chunks == 0) {
        return;
    }

    if (p.vdev_size == 0) {
        throw std::invalid_argument("VDev size cannot be 0: " + p.vdev_name);
    }

    const uint64_t max_num_chunks = std::min(to_u64(p.vdev_size / MIN_CHUNK_SIZE), to_u64(MAX_CHUNKS_IN_SYSTEM));

    if (p.num_chunks != 0) {
        const uint32_t min_chunks = to_u32((p.vdev_size - 1) / to_u64(Chunk::MAX_CHUNK_SIZE) + 1);
        p.num_chunks = std::max(p.num_chunks, min_chunks);
        p.num_chunks = std::min(p.num_chunks, to_u32(max_num_chunks));
        const uint64_t unit = to_u64(p.num_chunks) * p.blk_size;
        p.vdev_size = (p.vdev_size / unit) * unit;
        p.chunk_size = p.vdev_size / p.num_chunks;
    } else if (p.chunk_size != 0) {
        p.chunk_size = std::max(p.chunk_size, MIN_CHUNK_SIZE);
        p.chunk_size = ((p.chunk_size + p.blk_size - 1) / p.blk_size) * p.blk_size;
        p.vdev_size = (p.vdev_size / p.chunk_size) * p.chunk_size;
        p.num_chunks = to_u32(p.vdev_size / p.chunk_size);
    } else {
        throw std::invalid_argument("Both num_chunks and chunk_size are 0 for vdev: " + p.vdev_name);
    }

    if (p.vdev_size % p.chunk_size != 0) {
        throw std::invalid_argument("vdev_size not a multiple of chunk_size for vdev: " + p.vdev_name);
    }
    if (p.chunk_size < MIN_CHUNK_SIZE) {
        throw std::invalid_argument("chunk_size < 16 MB for vdev: " + p.vdev_name);
    }
    if (p.num_chunks > MAX_CHUNKS_IN_SYSTEM) {
        throw std::invalid_argument("num_chunks > MAX_CHUNKS_IN_SYSTEM for vdev: " + p.vdev_name);
    }
}

std::vector< shared< PhysicalDev > > VirtualDev::pick_pdevs(const std::vector< shared< PhysicalDev > >& pdevs,
                                                            MultiPDevOpts opts) {
    if (pdevs.empty()) {
        throw std::invalid_argument("No pdevs available");
    }
    switch (opts) {
    case MultiPDevOpts::AllPDevStriped:
        return pdevs;
    case MultiPDevOpts::AllPDevMirrored:
        throw std::runtime_error("AllPDevMirrored is not yet supported");
    case MultiPDevOpts::SingleFirstPDev:
        return {pdevs[0]};
    case MultiPDevOpts::SingleRandomPDev: {
        std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution< size_t > dist{0, pdevs.size() - 1};
        return {pdevs[dist(rng)]};
    }
    }
    return {pdevs[0]};
}

// ──────────────────────────────────────────────────────────────────────────────
// Private Helpers - Chunk Management methods
// ──────────────────────────────────────────────────────────────────────────────
void VirtualDev::on_chunk_added(cshared< Chunk >& chunk, bool newly_created) {
    on_chunks_added(std::vector< shared< Chunk > >{chunk}, newly_created);
}

void VirtualDev::on_chunks_added(const std::vector< shared< Chunk > >& chunks, bool newly_created) {
    std::lock_guard lg{chunk_mgmt_mutex_};
    auto new_state = clone_state();
    new_state.chunks_by_vdev_order.clear();
    new_state.chunks_by_vdev_order.reserve(new_state.all_chunks.size() + chunks.size());

    for (auto& chunk : chunks) {
        const uint32_t chunk_id = chunk->chunk_id();
        const bool is_allocated = chunk->info().is_allocated();

        if (!is_allocated) {
            LOGDEBUG("Found inactive chunk {} during recovery; adding to pool", chunk_id);
            if (chunk_pool_) {
                chunk_pool_->return_chunk(chunk);
            }
            return;
        }

        new_state.pdevs.insert(chunk->info().vdev_id);
        new_state.all_chunks.emplace(chunk_id, chunk);
        ++new_state.total_chunk_num;
        new_state.total_vdev_size += chunk->info().chunk_size;

        const uint64_t ord = chunk->vdev_order();
        if (ord + 1 > new_state.next_vdev_order) {
            new_state.next_vdev_order = ord + 1;
        }

        // Newly created chunks need their blk allocator constructed;
        if (newly_created) {
            construct_blk_allocator(chunk, std::nullopt);
        }
    }

    for (auto& [_, c] : new_state.all_chunks) {
        new_state.chunks_by_vdev_order.push_back(c);
    }
    std::sort(new_state.chunks_by_vdev_order.begin(), new_state.chunks_by_vdev_order.end(),
              [](const auto& a, const auto& b) { return a->vdev_order() < b->vdev_order(); });
    new_state.chunk_selector = build_chunk_selector(chunk_selector_type_, new_state.chunks_by_vdev_order);
    store_state(std::move(new_state));
}

void VirtualDev::on_chunk_removed(cshared< Chunk >& chunk) {
    std::lock_guard lg{chunk_mgmt_mutex_};
    const uint32_t chunk_id = chunk->chunk_id();

    auto new_state = clone_state();
    new_state.all_chunks.erase(chunk_id);
    --new_state.total_chunk_num;
    new_state.total_vdev_size -= to_u64(chunk->info().chunk_size);

    new_state.chunks_by_vdev_order.clear();
    new_state.chunks_by_vdev_order.reserve(new_state.all_chunks.size());
    for (auto& [_, c] : new_state.all_chunks) {
        new_state.chunks_by_vdev_order.push_back(c);
    }
    std::sort(new_state.chunks_by_vdev_order.begin(), new_state.chunks_by_vdev_order.end(),
              [](const auto& a, const auto& b) { return a->vdev_order() < b->vdev_order(); });
    new_state.chunk_selector = build_chunk_selector(chunk_selector_type_, new_state.chunks_by_vdev_order);
    store_state(std::move(new_state));
}

shared< IChunkSelector > VirtualDev::build_chunk_selector(ChunkSelectorType type,
                                                          const std::vector< shared< Chunk > >& chunks) {
    if (chunks.size() == 1) {
        return std::make_shared< OnlyOneChunkSelector >(chunks);
    }
    switch (type) {
    case ChunkSelectorType::RoundRobin:
    case ChunkSelectorType::Custom:
        return std::make_shared< RoundRobinChunkSelector >(chunks);
    case ChunkSelectorType::Random:
        return std::make_shared< RandomChunkSelector >(chunks);
    case ChunkSelectorType::MostAvailableSpace:
        return std::make_shared< MostAvailableSpaceSelector >(chunks);
    case ChunkSelectorType::OnlyOne:
        return std::make_shared< OnlyOneChunkSelector >(chunks);
    }
    return std::make_shared< RoundRobinChunkSelector >(chunks);
}

void VirtualDev::enable_chunk_pooling(size_t pool_limit) {
    chunk_pool_.emplace(pool_limit);
    LOGINFO("VirtualDev '{}': enabled chunk pooling limit={}", name_, pool_limit);
}

folly::coro::Task< std::pair< shared< Chunk >, bool > > VirtualDev::get_or_create_nth_chunk(size_t n) {
    shared< Chunk > existing;
    size_t current_count;
    {
        auto state = load_state();
        current_count = state->chunks_by_vdev_order.size();
        if (n < current_count) {
            existing = state->chunks_by_vdev_order[n];
        }
    }

    if (existing) {
        co_return std::make_pair(std::move(existing), false);
    }

    if (n != current_count) {
        throw std::invalid_argument("Cannot create chunk at position " + std::to_string(n) + " — current count is " +
                                    std::to_string(current_count));
    }

    auto chunk = co_await expand(incremental_chunk_size_);
    co_return std::make_pair(std::move(chunk), true);
}

size_t VirtualDev::num_chunks_actual() const {
    return load_state()->all_chunks.size();
}

shared< Chunk > VirtualDev::select_chunk_for_alloc(blk_count_t nblks, const blk_alloc_hints& hints,
                                                   std::optional< uint32_t > last_failed_id) const {
    if (hints.chunk_id_hint.has_value()) {
        auto state = load_state();
        auto it = state->all_chunks.find(hints.chunk_id_hint.value());
        return (it != state->all_chunks.end()) ? it->second : nullptr;
    }

    auto state = load_state();
    const auto& sel = state->chunk_selector;
    if (!sel) {
        return nullptr;
    }

    if (!last_failed_id) {
        return sel->select_chunk(nblks, hints);
    }
    return sel->get_chunk_after(*last_failed_id);
}

std::pair< uint64_t, shared< Chunk > > VirtualDev::to_dev_offset(const BlkId& bid) const {
    auto state = load_state();
    auto it = state->all_chunks.find(bid.chunk_num());
    if (it == state->all_chunks.end()) {
        throw std::out_of_range("Chunk " + std::to_string(bid.chunk_num()) + " not found in vdev '" + name_ + "'");
    }
    const uint64_t dev_offset = to_u64(bid.blk_num()) * to_u64(blk_size_) + it->second->start_offset();
    return {dev_offset, it->second};
}

// ──────────────────────────────────────────────────────────────────────────────
// Private Helpers - VDevInfo management
// ──────────────────────────────────────────────────────────────────────────────
uint64_t VDevInfo::vdev_info_offset(uint32_t vdev_id) {
    return HSSuperBlk::vdev_sb_offset() + HSSuperBlk::vdev_slot_bitmap_size() + (to_u64(vdev_id) * SIZE);
}

void VirtualDev::adjust_vdev_info() {
    std::lock_guard lk{chunk_mgmt_mutex_};
    auto new_state = clone_state();

    uint64_t total_size = 0;
    uint64_t total_count = 0;
    for (auto& [_, c] : new_state.all_chunks) {
        total_size += c->info().chunk_size;
        ++total_count;
    }
    new_state.total_vdev_size = total_size;
    new_state.total_chunk_num = total_count;
    store_state(std::move(new_state));

    LOGINFO("Adjusted VDev '{}' stats: size={} num_chunks={}", name_, total_size, total_count);
}

folly::coro::Task< void > VirtualDev::write_vdev_info() {
    // Recompute checksum then mirror the VDevInfo record to every pdev for redundancy.
    VDevInfo vinfo;
    {
        auto state = load_state();
        vinfo = state->vdev_info;
    }
    vinfo.compute_checksum();

    IOBuffer buf{sizeof(VDevInfo)};
    std::memcpy(buf.bytes(), vinfo.to_bytes(), sizeof(VDevInfo));

    const uint64_t offset = VDevInfo::vdev_info_offset(vdev_id_);
    for (auto& pdev : pdevs_) {
        co_await pdev->write_super_block(buf, offset);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Private Helpers - Blk Allocator management
// ──────────────────────────────────────────────────────────────────────────────
void VirtualDev::construct_blk_allocator(cshared< Chunk >& chunk, std::optional< sisl::ByteArray > buffer) {
    if (allocator_type_ == BlkAllocatorType::None) {
        return;
    }

    const uint32_t align_size = chunk->physical_dev()->align_size();
    const std::string alloc_name = name_ + "_chunk_" + std::to_string(chunk->chunk_id());

    if (allocator_type_ == BlkAllocatorType::SlabCompact || allocator_type_ == BlkAllocatorType::SlabExtend) {
        SlabBlkAllocConfig cfg{blk_size_, align_size, align_size, chunk->info().chunk_size, persist_blk_alloced_,
                               alloc_name};
        cfg.alloc_mode =
            (allocator_type_ == BlkAllocatorType::SlabCompact) ? AllocMode::CompactAlloc : AllocMode::ExpandedAlloc;
        chunk->set_block_allocator(
            std::make_shared< SlabBlkAllocator >(cfg, std::move(buffer), to_u32(chunk->chunk_id())));
    }
}
} // namespace homestore
