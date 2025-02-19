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
#pragma once

#include <vector>
#include <atomic>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <sisl/cache/simple_cache.hpp>

#include <homestore/btree/btree_store.hpp>
#include <homestore/superblk_handler.hpp>
#include <homestore/checkpoint/cp_mgr.hpp>

namespace homestore {
#pragma pack(1)
struct cow_btree_sb_header {
    uint64_t sb_size{sizeof(cow_btree_sb_header)}; // Overall size of this sb including header
    uint64_t next_bnode_id{0};                     // Next bnode id to generate upon restart
    uint64_t num_entries{0};                       // Total number of map entries
    cp_id_t last_written_cp_id{-1};                // Which cp id this was persisted on
};
#pragma pack()

struct cow_btree_sb {
public:
    cow_btree_sb_header header;
    std::pair< bnodeid_t, BlkId > entries[1]; // Followed by an array of entries

    static cow_btree_sb* create(uint64_t max_entries, cp_id_t preparing_cp_id) {
        hs_utils::iobuf_alloc(sizeof(cow_btree_sb_header)
    }
    cow_btree_sb(cp_id_t preparing_cp_id) {
        header.last_written_cp_id = preparing_cp_id;
    }

    void add_entry(bnodeid_t node_id, BlkId const& blkid) {
        entries[header.num_entries++] = std::make_pair(node_id, blkid);
    }
};

class COWBtreeStore : public BtreeStoreBase {
private:
#pragma pack(1)
    struct btree_blk_id {
        blk_num_t blk_num;
        chunk_num_t chunk_num;
        uint16_t modified_cp_id;

        btree_blk_id(BlkId const& bid, cp_id_t cp_id) :
                blk_num{bid.blk_num()}, chunk_num{bid.chunk_num()}, modified_cp_id{cp_id % sizeof(uint16_t)} {}
    };
#pragma pack()

private:
    sisl::SimpleCache< bnodeid_t, BtreeNodePtr > m_cache;
    folly::ConcurrentHashMap< bnodeid_t, btree_blk_id > m_bnode_map;
    std::shared_ptr< VirtualDev > m_vdev;
    std::atomic< uint64_t > m_next_bnode_id{0};
    uint32_t m_max_nodes_per_flush;
    void* m_bnode_map_base_mblk;
    uint32_t const m_vdev_blks_per_node;

public:
    COWBtreeStore(shared< VirtualDev > vdev, std::pair< meta_blk*, sisl::byte_view > sb,
                  shared< sisl::Evictor > evictor, uint32_t node_size);
    virtual ~COWBtreeStore() = default;

    std::string store_type() const override { return "COW_BTREE"; }

    ////////////////// Override Implementation of underlying store requirements //////////////////
    BtreeNodePtr create_node(BtreeBase& btree, bool is_leaf) override;

    btree_status_t write_node(BtreeBase& btree, const BtreeNodePtr& node, void* context) override;

    btree_status_t transact_write_nodes(BtreeBase& btree, const folly::small_vector< BtreeNodePtr, 3 >& new_nodes,
                                        const BtreeNodePtr& left_child_node, const BtreeNodePtr& parent_node,
                                        void* context) override;

    btree_status_t read_node(BtreeBase& btree, bnodeid_t id, BtreeNodePtr& node) const override;

    btree_status_t refresh_node(BtreeBase& btree, const BtreeNodePtr& node, bool for_read_modify_write,
                                void* context) const override;

    void remove_node(BtreeBase& btree, const BtreeNodePtr& node, void* context) override;

    btree_status_t on_root_changed(BtreeBase& btree, BtreeNodePtr const& root, void* context) override;
};

} // namespace homestore
