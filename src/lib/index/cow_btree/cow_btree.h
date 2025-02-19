#pragma once

#include <homestore/blk.h>
#include <homestore/btree/btree.hpp>
#include "common/large_id_reserver.hpp"

namespace homestore {
class COWBtree {
    using CompactNodeId = uin32_t;

#pragma pack(1)
    struct CompactBlkId {
        blk_num_t is_valid : 1;
        blk_num_t blk_num : 31;
        chunk_num_t chunk_num;

        CompactBlkId() : is_valid{false} {}
        CompactBlkId(BlkId const& b) : is_valid{true}, blk_num{b.blk_num()}, chunk_num{b.chunk_num()} {}
        CompactBlkId(btree_blk_id const& b) : is_valid{true}, blk_num{b.blk_num}, chunk_num{b.chunk_num} {}

        BlkId to_blkid() const { return is_valid ? BlkId{blk_num, 1u, chunk_num} : BlkId{}; };
    };
#pragma pack()

    struct FullBNodeIdMap {
        std::map< CompactNodeId, CompactBlkId > map_;
        iomgr::FiberManagerLib::shared_mutex mtx_;

        std::vector< BlkId > chain_locations_;    // List of locations where bnodeid maps are chained together
        std::vector< BlkId > freeable_locations_; // Array of locations which can be freed once full map is written
    };

private:
    FullBNodeIdMap m_bnodeid_map;
    BtreeBase* m_base_btree;
    LargeIDReserver m_nodeid_generator;
    superblk< index_table_sb > m_sb;
    shared< VirtualDev > m_vdev;

    uint32_t m_btree_ordinal;
    uint64_t m_ordinal_shifted;

    std::array< sisl::ConcurrentInsertVector< BtreeNodePtr >, 2 > m_dirty_bufs;

public:
};
} // namespace homestore