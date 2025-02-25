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

class COWBtreeStore : public BtreeStore {
public:
#pragma pack(1)
    struct Journal : public IndexStoreSuperBlock {
    public:
        cp_id_t cp_id;                   // CP Id for this journal, we have one meta blk which contains journal per CP
        uint32_t size;                   // Total journal size
        uint32_t num_btrees{0};          // Total number of btrees updated in this
        uint32_t total_written_nodes{0}; // Total number of nodes written in this journal
        uint32_t total_removed_nodes{0}; // Total number of nodes removed in this journal

        // Followed by multiple cowbtree journals
    };
#pragma pack()

private:
    sisl::SimpleCache< bnodeid_t, BtreeNodePtr > m_cache;
    std::shared_ptr< VirtualDev > m_vdev;
    std::atomic< uint64_t > m_next_bnode_id{0};
    meta_blk* m_btree_journal_mblk;
    uint32_t const m_vdev_blks_per_node;

    // All loaded journals arranged by the btree ordinals
    std::unordered_map< uint32_t, std::vector< sisl::byte_view > > m_journals_by_btree;

    // All journals maintained (sorted) by its cp_id
    std::vector< superblk< IndexStoreSuperBlock > > m_journals_by_cpid;

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

struct COWBtreeJournal {
public:
private:
    sisl::io_blob_safe base_buf_;
    uint8_t* cur_ptr_;
    uint32_t used_size_;

#pragma pack(1)
    struct Header {
        cp_id_t cp_id;                   // btree journal is one per cp, this uniquely identifies this journal
        uint32_t num_btrees{0};          // Total number of btrees updated in this
        uint32_t total_written_nodes{0}; // Total number of nodes written in this journal
        uint32_t total_removed_nodes{0}; // Total number of nodes removed in this journal
        uint32_t size{sizeof(Header)};   // Total size of this journal
    };
#pragma pack()

public:
    COWBtreeJournal(uint32_t initial_size);
    uint8_t* make_room(uint32_t num_bytes);
    void one_btree_filled(uint32_t num_bytes, uint32_t n_nodes_written, uint32_t n_nodes_removed);

private:
    Header* header() { return r_cast< Header* >(base_buf_.bytes_); }
    uint32_t occupied_size() const { return header()->size; }
    uint32_t available_space() const { return (base_buf_.size - occupied_size()); }
};
} // namespace homestore
