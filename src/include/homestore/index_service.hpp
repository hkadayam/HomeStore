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
#include <memory>
#include <unordered_map>
#include <vector>

#include <iomgr/iomgr.hpp>
#include <sisl/fds/id_reserver.hpp>
#include <homestore/homestore_decl.hpp>
#include <homestore/btree/details/btree_internal.hpp>
#include <homestore/superblk_handler.hpp>

namespace homestore {

class Index;
class VirtualDev;

class IndexStore {
public:
    ENUM(Type, uint8_t, MEM_BTREE, COPY_ON_WRITE_BTREE, INPLACE_BTREE);
    virtual std::string store_type() const = 0;
};

#pragma pack(1)
struct IndexSuperBlock {
    static constexpr uint64_t magic{0xbedabb1e};
    static constexpr uint32_t version{0x3};

    // Common Area for all index implementations
    uint64_t magic{indx_sb_magic};
    uint32_t version{indx_sb_version};
    uuid_t uuid;                       // UUID of the index
    uuid_t parent_uuid;                // UUID of the parent container of index (controlled by user)
    IndexStore::Type index_store_type; // Underlying store type for this index
    uint32_t ordinal;                  // Ordinal of the Index (unique within the homestore instance)

    // Btree based implementations superblock area
    struct BtreeSuperBlock {
        bnodeid_t root_node{empty_bnodeid}; // Btree Root Node ID
        int64_t index_size{0};              // Size of the Index

        union {
            struct COWBtreeSuperBlock {
                BlkId full_map_location; // Location of any btree map (applicable for COWBtree only so far)
            };

            struct InPlaceBtreeSuperBlock {
                uint64_t root_link_version{0}; // Link version to btree root node
            }

            COWBtreeSuperBlock cow_sb;
            InPlaceBtreeSuperBlock ip_sb;
        } u;
    };
    BtreeSuperBlock btree_sb;

    // User area of the superblock, which can be updated with cp guard.
    uint32_t user_sb_size;    // Size of the user superblk
    uint8_t user_sb_bytes[0]; // Raw bytes of the sb. Better to access with helper routine below

    sisl::blob user_sb() { return sisl::blob{&user_sb_bytes[0], user_sb_size}; }
};

struct IndexStoreSuperBlock {
    IndexStore::Type index_store_type;
};

#pragma pack()

class IndexServiceCallbacks {
public:
    virtual ~IndexServiceCallbacks() = default;
    virtual shared< Index > on_index_table_found(superblk< IndexSuperBlock >&&) {
        assert(0);
        return nullptr;
    }
};

class Index {
protected:
    superblk< IndexSuperBlock > m_sb;
    bool const m_is_ephemeral; // Is it a persistent btree?

public:
    Index(bool is_ephermal) : m_is_ephemeral{is_ephermal} {}
    bool is_ephemeral() const { return m_is_ephemeral; }

    // Destroys the index and remove all its resources. This could be delayed call as in actual destroy could
    // potentially takes place in subsequent checkpoints. Hence caller should not assume that destroy is completed
    // instantly. This is an idempotent call and the implementer of this method needs to support that.
    virtual void destroy() = 0;
    bool is_destroy_pending() const = 0;

    // Getters
    uuid_t uuid() const override { return m_sb->uuid; }
    uint64_t used_size() const override { return m_sb->index_size; }
    superblk< IndexSuperBlock >& mutable_super_blk() { return m_sb; }
    const superblk< IndexSuperBlock >& mutable_super_blk() const { return m_sb; }
};

struct IndexMetaInfo {
    meta_blk* mblk;
    sisl::byte_view mbuf;

    IndexMetaInfo(meta_blk* blk, sisl::byte_view b) : mblk{blk}, mbuf{std::move(b)} {}
    uint8_t* raw_buf() { return mbuf.bytes(); }
    sisl::byte_view& buf() { return mbuf; }
};

class IndexService {
private:
    unique< IndexServiceCallbacks > m_svc_cbs;
    unique< IndexWBCacheBase > m_wb_cache;
    shared< VirtualDev > m_vdev;
    std::vector< IndexMetaInfo > m_index_sbs;
    std::vector< IndexMetaInfo > m_store_sbs;
    unique< sisl::IDReserver > m_ordinal_reserver;
    std::unordered_map< IndexStore::Type, unique< IndexStore > > m_index_stores;

    mutable std::shared_mutex m_index_map_mtx;
    std::map< uuid_t, shared< Index > > m_index_map;
    std::unordered_map< uint32_t, shared< Index > > m_ordinal_index_map;

public:
    IndexService(unique< IndexServiceCallbacks > cbs);

    // Creates the vdev that is needed to initialize the device
    void create_vdev(uint64_t size, HSDevType devType, uint32_t num_chunks);

    // Open the existing vdev which is represnted by the vdev_info_block
    shared< VirtualDev > open_vdev(const vdev_info& vb, bool load_existing);

    // Start the Index Service
    void start();

    // Stop the Index Service
    void stop();

    // Add/Remove Index Table to/from the index service
    void add_index_table(shared< Index > const& tbl);
    void remove_index_table(shared< Index > const& tbl);
    shared< Index > get_index_table(uuid_t uuid) const;
    shared< Index > get_index_table(uint32_t ordinal) const;
    std::vector< shared< Index > > get_all_index_tables() const;

    IndexStore* lookup_store(IndexStore::Type store_type);
    uint64_t used_size() const;
    uint32_t node_size() const;
};

extern IndexService& index_service();

} // namespace homestore
