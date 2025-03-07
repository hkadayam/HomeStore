/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <homestore/homestore.hpp>
#include <homestore/index_service.hpp>
#include <homestore/btree/detail/btree_node.hpp>

#include "common/homestore_utils.hpp"
#include "common/homestore_assert.hpp"
#include "device/virtual_dev.hpp"
#include "device/physical_dev.hpp"
#include "device/chunk.h"
#include "index/cow_btree/cow_btree_store.h"
//#include "index/inplace_btree/inplace_btree_store.h"
#include "index/mem_btree/mem_btree_store.h"

namespace homestore {
IndexService& index_service() { return hs()->index_service(); }

IndexService::IndexService(std::unique_ptr< IndexServiceCallbacks > cbs,
                           std::vector< ServiceSubType > const& sub_types) :
        m_svc_cbs{std::move(cbs)} {
    m_ordinal_reserver = std::make_unique< sisl::IDReserver >();
    meta_service().register_handler(
        "index_table",
        [this](meta_blk* mblk, sisl::byte_view buf, size_t size) {
            superblk< IndexSuperBlock > sb("index_table");
            sb.load(buf, mblk);
            m_index_sbs.emplace_back(std::move(sb));
        },
        nullptr);

    meta_service().register_handler(
        "index_store",
        [this](meta_blk* mblk, sisl::byte_view buf, size_t size) {
            superblk< IndexStoreSuperBlock > sb("index_store");
            sb.load(buf, mblk);
            m_store_sbs.emplace_back(std::move(sb));
        },
        nullptr);
}

void IndexService::create_vdev(ServiceSubType sub_type, uint64_t size, HSDevType devType, uint32_t num_chunks) {
    auto const atomic_page_size = hs()->device_mgr()->atomic_page_size(devType);
    hs_vdev_context vdev_ctx;
    vdev_ctx.type = hs_vdev_type_t::INDEX_VDEV;
    vdev_ctx.sub_type = sub_type;

    hs()->device_mgr()->create_vdev(vdev_parameters{.vdev_name = "index",
                                                    .vdev_size = size,
                                                    .num_chunks = num_chunks,
                                                    .blk_size = atomic_page_size,
                                                    .dev_type = devType,
                                                    .alloc_type = blk_allocator_type_t::varsize,
                                                    .chunk_sel_type = chunk_selector_type_t::ROUND_ROBIN,
                                                    .multi_pdev_opts = vdev_multi_pdev_opts_t::ALL_PDEV_STRIPED,
                                                    .context_data = vdev_ctx.to_blob()});
}

shared< VirtualDev > IndexService::open_vdev(ServiceSubType sub_type, const vdev_info& vinfo, bool load_existing) {
    auto const vdev =
        std::make_shared< VirtualDev >(*(hs()->device_mgr()), vinfo, nullptr /* event_cb */, false /* auto_recovery */);
    m_vdevs.insert(std::make_pair(sub_type, vdev));
    return vdev;
}

void IndexService::start() {
    if (m_store_sbs.size()) {
        // Segregate the index store super blocks based on the store type
        std::unordered_map< IndexStore::Type, std::vector< superblk< IndexStoreSuperBlock > > > m;
        for (auto& sb : m_store_sbs) {
            m[sb->index_store_type].emplace_back(std::move(sb));
        }

        for (auto& [store_type, sbs] : m) {
            lookup_or_create_store(store_type, std::move(sbs));
        }
    }

    // Load any index tables which are to loaded from meta blk
    for (auto& sb : m_index_sbs) {
        m_ordinal_reserver->reserve(sb->ordinal);
        add_index_table(m_svc_cbs->on_index_table_found(std::move(sb)));
    }

    // Notify each index store that we have completed recovery
    std::unique_lock lg(m_index_map_mtx);
    for (auto& [type, store] : m_index_stores) {
        store->on_recovery_completed();
    }
}

void IndexService::stop() {
    m_index_map.clear();
    m_ordinal_index_map.clear();

    for (auto& [type, store] : m_index_stores) {
        store.reset();
    }
}

shared< VirtualDev > IndexService::get_vdev(ServiceSubType sub_type) {
    auto it = m_vdevs.find(sub_type);
    HS_REL_ASSERT(it != m_vdevs.end(), "Vdev not found for sub_type={}, vdev not created/opened?", sub_type);
    return it->second;
}

shared< IndexStore > IndexService::lookup_or_create_store(IndexStore::Type store_type,
                                                          std::vector< superblk< IndexStoreSuperBlock > > sbs) {
    std::unique_lock lg(m_index_map_mtx);
    auto it = m_index_stores.find(store_type);
    if (it != m_index_stores.end()) { return it->second; }

    shared< IndexStore > store;

    switch (store_type) {
    case IndexStore::Type::COPY_ON_WRITE_BTREE:
        store =
            std::make_shared< COWBtreeStore >(get_vdev(ServiceSubType::INDEX_BTREE_COPY_ON_WRITE), std::move(sbs),
                                              hs()->evictor(), hs()->device_mgr()->atomic_page_size(HSDevType::Fast));
        break;

    case IndexStore::Type::INPLACE_BTREE:
#if 0
        store = std::make_shared< InPlaceBtreeStore >(get_vdev(ServiceSubType::INDEX_BTREE_INPLACE), std::move(sbs),
                                                      hs()->evictor(),
                                                      hs()->device_mgr()->atomic_page_size(HSDevType::Fast));
#endif
        break;

    case IndexStore::Type::MEM_BTREE:
        store = std::make_shared< MemBtreeStore >();
        break;

    default:
        HS_REL_ASSERT(false, "Unsupported index store type {}", store_type);
        break;
    }
    m_index_stores.emplace(std::pair(store_type, store));
    return store;
}

void IndexService::add_index_table(const shared< Index >& index) {
    std::unique_lock lg(m_index_map_mtx);
    m_index_map.insert(std::make_pair(index->uuid(), index));
    m_ordinal_index_map.insert(std::make_pair(index->ordinal(), index));
}

void IndexService::remove_index_table(const shared< Index >& index) {
    // It will call the destroy and let the index store calls the remove index table entry when it is ready to purge.
    index->destroy();
}

void IndexService::remove_index_table_entry(uuid_t uuid) {
    std::unique_lock lg(m_index_map_mtx);
    auto it = m_index_map.find(uuid);
    if (it == m_index_map.end()) { return; }
    m_ordinal_index_map.erase(it->second->ordinal());
    m_ordinal_reserver->unreserve(it->second->ordinal());
    m_index_map.erase(it);
}

shared< Index > IndexService::get_index_table(uuid_t uuid) const {
    std::shared_lock lg(m_index_map_mtx);
    auto const it = m_index_map.find(uuid);
    return (it != m_index_map.cend()) ? it->second : nullptr;
}

shared< Index > IndexService::get_index_table(uint32_t ordinal) const {
    std::shared_lock lg(m_index_map_mtx);
    auto const it = m_ordinal_index_map.find(ordinal);
    return (it != m_ordinal_index_map.cend()) ? it->second : nullptr;
}

std::vector< shared< Index > > IndexService::get_all_index_tables() const {
    std::shared_lock lg(m_index_map_mtx);
    std::vector< shared< Index > > v;
    std::transform(m_index_map.begin(), m_index_map.end(), std::back_inserter(v),
                   [](auto const& kv) { return kv.second; });
    return v;
}

uint32_t IndexService::reserve_ordinal() { return m_ordinal_reserver->reserve(); }

uint64_t IndexService::used_size() const {
    auto size{0};
    std::unique_lock lg{m_index_map_mtx};
    for (auto& [id, index] : m_index_map) {
        size += index->used_size();
    }
    return size;
}
} // namespace homestore
