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

#include <cstring>
#include <stdexcept>

#include "common/defs.h"
#include "homestore/managers.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/virtual_dev.h"

namespace homestore {

using sisl::IoBufOwn;

// ──────────────────────────────────────────────────────────────────────────────
// create
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > MetaBlkManager::create(uint64_t chunk_size) {
    VDevParameters params;
    params.vdev_name = "meta_vdev";
    params.blk_size = 512;
    params.initial_chunk_size = chunk_size; // single chunk to start; vdev expands in chunk_size increments on demand
    params.initial_num_chunks = 1;
    params.dev_type = HSDevType::Fast;
    params.multi_pdev_opts = MultiPDevOpts::AllPDevStriped;
    params.alloc_type = BlkAllocatorType::SlabExtend;
    params.chunk_sel_type = ChunkSelectorType::OnlyOne;
    params.persist_blk_alloced = false;

    shared< VirtualDev > vdev = co_await device_mgr().create_vdev(std::move(params));
    co_await vdev->format();

    const size_t blk_sz = vdev->block_size();
    const size_t total_meta_sz = MetaBlkSuperHeader::SIZE + MAX_META_CLIENTS * MetaClientInfo::SIZE;
    const auto nblks = static_cast< blk_count_t >((total_meta_sz + blk_sz - 1) / blk_sz);

    const chunk_num_t chunk_id = vdev->get_nth_chunk(0)->chunk_id();
    const BlkId client_info_bid{0u, nblks, chunk_id};
    vdev->commit_blk(client_info_bid);

    auto mgr = shared< MetaBlkManager >(new MetaBlkManager{});
    mgr->meta_vdev_ = vdev;
    mgr->client_info_bid_ = client_info_bid;
    mgr->client_slots_.assign(MAX_META_CLIENTS, 0);

    // Write super-header + empty client-info slots in one shot.
    IoBufOwn buf{to_u32(MetaBlkSuperHeader::SIZE + MetaClientInfo::SIZE * MAX_META_CLIENTS)};

    const MetaBlkSuperHeader super = MetaBlkSuperHeader::make();
    std::memcpy(buf.bytes(), &super, MetaBlkSuperHeader::SIZE);

    for (size_t slot = 0; slot < MAX_META_CLIENTS; ++slot) {
        const MetaClientInfo empty_slot = MetaClientInfo::make_free();
        uint8_t* dest = buf.bytes() + MetaBlkSuperHeader::SIZE + slot * MetaClientInfo::SIZE;
        std::memcpy(dest, &empty_slot, MetaClientInfo::SIZE);
    }

    co_await vdev->write(buf, client_info_bid);

    Managers::init_meta_mgr(std::move(mgr));
}

// ──────────────────────────────────────────────────────────────────────────────
// load
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > MetaBlkManager::load() {
    shared< VirtualDev > vdev = device_mgr().get_vdev("meta_vdev");

    // Meta vdev is non-persistent: construct fresh block allocators so alloc/commit work after reload.
    vdev->init_blk_allocator();

    const size_t blk_sz = vdev->block_size();
    const size_t total_meta_sz = MetaBlkSuperHeader::SIZE + MAX_META_CLIENTS * MetaClientInfo::SIZE;
    const auto nblks = static_cast< blk_count_t >((total_meta_sz + blk_sz - 1) / blk_sz);

    const chunk_num_t chunk_id = vdev->get_nth_chunk(0)->chunk_id();
    const BlkId client_info_bid{0u, nblks, chunk_id};
    vdev->commit_blk(client_info_bid);

    auto mgr = shared< MetaBlkManager >(new MetaBlkManager{});
    mgr->meta_vdev_ = vdev;
    mgr->client_info_bid_ = client_info_bid;
    mgr->client_slots_.assign(MAX_META_CLIENTS, 0);

    co_await mgr->load_client_info_from_disk();

    Managers::init_meta_mgr(std::move(mgr));
}

// ──────────────────────────────────────────────────────────────────────────────
// register_client
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< MetaClient > MetaBlkManager::register_client(std::string name) {
    // Determine client_id and whether this is a recovery case.
    // Lock is released before the (potentially slow) client create/load.
    uint8_t client_id{};
    bool is_recovery{false};
    MetaClientInfo recovered_info{};

    {
        auto lock = co_await mgmt_mutex_.co_scoped_lock();

        auto it = recovered_clients_.find(name);
        if (it != recovered_clients_.end()) {
            recovered_info = it->second;
            client_id = recovered_info.client_id;
            client_slots_[client_id] = 1;
            recovered_clients_.erase(it);
            is_recovery = true;
        } else {
            client_id = static_cast< uint8_t >(reserve_slot_internal(client_slots_));
        }
    }

    if (is_recovery) {
        co_return co_await MetaClient::load(std::move(recovered_info), meta_vdev_);
    } else {
        co_return co_await MetaClient::create(std::move(name), client_id, meta_vdev_);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// deregister_client
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > MetaBlkManager::deregister_client(const MetaClient& client) {
    const uint8_t cid = co_await client.client_id();

    {
        auto lock = co_await mgmt_mutex_.co_scoped_lock();
        client_slots_[cid] = 0;
    }

    // Write a free MetaClientInfo slot to disk.
    const size_t blk_sz = meta_vdev_->block_size();
    const size_t n_super_blks = (MetaBlkSuperHeader::SIZE + blk_sz - 1) / blk_sz;
    const size_t info_nblks = (MetaClientInfo::SIZE + blk_sz - 1) / blk_sz;
    const uint32_t blk_num = static_cast< uint32_t >(cid * info_nblks + n_super_blks);
    const chunk_num_t chunk_id = meta_vdev_->get_nth_chunk(0)->chunk_id();
    const BlkId info_bid{blk_num, static_cast< blk_count_t >(info_nblks), chunk_id};

    MetaClientInfo freed = MetaClientInfo::make_free();
    IoBufOwn buf{to_u32(MetaClientInfo::SIZE)};
    std::memcpy(buf.bytes(), &freed, MetaClientInfo::SIZE);
    co_await meta_vdev_->write(buf, info_bid);
}

// ──────────────────────────────────────────────────────────────────────────────
// load_client_info_from_disk  (private)
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > MetaBlkManager::load_client_info_from_disk() {
    const size_t total_sz = MetaBlkSuperHeader::SIZE + MAX_META_CLIENTS * MetaClientInfo::SIZE;

    META_LOG(DEBUG, "load_client_info_from_disk: reading {} bytes from blk_num={}", total_sz,
             client_info_bid_.blk_num());
    IoBufOwn buf{to_u32(total_sz)};
    auto err = co_await meta_vdev_->read(buf, client_info_bid_);
    if (err) { throw std::runtime_error{"MetaBlkManager: failed to read client info area"}; }

    // Validate super-header.
    const auto& super = *reinterpret_cast< const MetaBlkSuperHeader* >(buf.bytes());
    if (!super.is_valid()) { throw std::runtime_error{"MetaBlkManager: invalid super-header magic/version"}; }
    META_LOG(DEBUG, "load_client_info_from_disk: super-header valid, scanning {} client slots", MAX_META_CLIENTS);

    // Scan all client slots.
    for (size_t slot = 0; slot < MAX_META_CLIENTS; ++slot) {
        const uint8_t* slot_ptr = buf.bytes() + MetaBlkSuperHeader::SIZE + slot * MetaClientInfo::SIZE;

        MetaClientInfo info;
        std::memcpy(&info, slot_ptr, MetaClientInfo::SIZE);

        if (info.is_allocated() && info.validate_crc()) {
            info.client_id = static_cast< uint8_t >(slot); // Authoritative source
            client_slots_[slot] = 1;
            recovered_clients_.emplace(info.get_client_name(), info);
            META_LOG(DEBUG, "load_client_info_from_disk: recovered client slot={} name={} first_blk_valid={}", slot,
                     info.get_client_name(), info.first_blkid.is_valid());
        }
    }
    META_LOG(DEBUG, "load_client_info_from_disk: {} clients recovered", recovered_clients_.size());
}

// ──────────────────────────────────────────────────────────────────────────────
// reserve_slot_internal  (private, static)
// ──────────────────────────────────────────────────────────────────────────────
size_t MetaBlkManager::reserve_slot_internal(std::vector< uint8_t >& slots) {
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] == 0) {
            slots[i] = 1;
            return i;
        }
    }
    throw std::runtime_error{"MetaBlkManager: no free client slots available"};
}

} // namespace homestore
