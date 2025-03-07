#pragma once

#include <vector>
#include <sisl/fds/concurrent_insert_vector.hpp>
#include <homestore/blk.h>
#include <homestore/btree/btree_base.hpp>
#include <homestore/checkpoint/cp_mgr.hpp>
#include "common/large_id_reserver.hpp"

namespace homestore {
class COWBtreeCPContext;
class VirtualDev;

class COWBtree : public UnderlyingBtree {
public:
    struct Journal;

public:
    COWBtree(BtreeBase* bt, shared< VirtualDev > vdev, std::vector< sisl::byte_view > journal_bufs);
    virtual ~COWBtree() = default;

    bnodeid_t generate_node_id();
    void add_to_dirty_list(BtreeNodePtr const& node, COWBtreeCPContext* cp_ctx);
    void add_to_remove_list(bnodeid_t node_id, COWBtreeCPContext* cp_ctx);

    void on_root_changed(BtreeNodePtr const& new_root, COWBtreeCPContext* cp_ctx);
    void on_btree_destroyed();

    BlkId get_blkid_for_nodeid(bnodeid_t nodeid) const;
    uint64_t used_size() const;
    uint32_t align_size() const;

    std::tuple< bool, unique< Journal >, bool > flush_nodes(COWBtreeCPContext* cp_ctx);
    void flush_map_and_sb(COWBtreeCPContext* cp_ctx);
    void flush_sb(COWBtreeCPContext* cp_ctx);

public:
    using CompactNodeId = uint32_t;

#pragma pack(1)
    struct CompactBlkId {
        blk_num_t is_valid : 1;
        blk_num_t blk_num : 31;
        chunk_num_t chunk_num;

        CompactBlkId() : is_valid{false} {}
        CompactBlkId(BlkId const& b) : is_valid{true}, blk_num{b.blk_num()}, chunk_num{b.chunk_num()} {}
        CompactBlkId(BlkId const& b, uint16_t offset) :
                is_valid{true}, blk_num{b.blk_num() + offset}, chunk_num{b.chunk_num()} {}

        BlkId to_blkid() const { return is_valid ? BlkId{blk_num, 1u, chunk_num} : BlkId{}; };
    };
#pragma pack()

#pragma pack(1)
    struct SuperBlock {
        cp_id_t cp_id;          // CPID when this superblock was written
        uint16_t num_map_heads; // Total number of map heads
        BlkId map_heads[1];     // Array of heads of chain which contains the blkid map data

        static uint32_t max_map_heads(uint32_t sb_size) {
            return (sb_size - sizeof(SuperBlock) + sizeof(BlkId)) / sizeof(BlkId);
        }
    };
    static_assert(sizeof(SuperBlock) < 512, "Expected superblk to be within the btree superblk underlying btree size");
#pragma pack()

    struct Journal {
#pragma pack(1)
        struct Header {
            uint32_t ordinal;              // Journal for which btree ordinal
            uint32_t size{sizeof(Header)}; // Size of this journal
            uint32_t num_flush_units{0};   // Number of flush units in this journal
            uint32_t num_delete_units{0};  // Number of nodes removed for this btree.

            // Followed by an array of FlushUnitentry and then array of Deleted
            // Nodeids
        };
#pragma pack()

        sisl::io_blob_safe m_base_buf;
        uint8_t* m_cur_ptr;

        Journal(uint32_t ordinal, uint32_t initial_size) :
                m_base_buf{std::max(initial_size, uint32_cast(sizeof(Header))), meta_service().align_size(),
                           sisl::buftag::metablk} {
            Header* hdr = new (m_base_buf.bytes()) Header();
            hdr->size = initial_size;
            hdr->ordinal = ordinal;
            m_cur_ptr = m_base_buf.bytes() + sizeof(Header);
        }

        uint8_t* allocate(uint32_t num_bytes) {
            if (available_space() < num_bytes) {
                // We need to realloc the buffer and adjust the pointers. By default try to increase 50% more everytime
                // (instead of doubling).
                auto const cur_size = occupied_size();
                m_base_buf.buf_realloc(
                    std::max(num_bytes - available_space(), m_base_buf.size() + m_base_buf.size() / 2),
                    meta_service().align_size(), sisl::buftag::metablk);
                m_cur_ptr = m_base_buf.bytes() + cur_size;
                header()->size += num_bytes;
            }
            auto ret_ptr = m_cur_ptr;
            m_cur_ptr += num_bytes;
            return ret_ptr;
        }

        uint8_t* make_room(uint32_t num_bytes) {
            if (available_space() < num_bytes) {
                // We need to realloc the buffer and adjust the pointers.
                // By default try to increase 50% more everytime (instead of
                // doubling).
                m_base_buf.buf_realloc(
                    std::max(num_bytes - available_space(), m_base_buf.size() + m_base_buf.size() / 2),
                    meta_service().align_size(), sisl::buftag::metablk);
                m_cur_ptr = m_base_buf.bytes() + occupied_size();
            }
            return m_cur_ptr;
        }

        sisl::io_blob& raw_buf() { return m_base_buf; }
        Header* header() { return r_cast< Header* >(m_base_buf.bytes()); }
        uint32_t occupied_size() const { return m_cur_ptr - m_base_buf.cbytes(); }
        uint32_t available_space() const { return (m_base_buf.size() - occupied_size()); }
    };

    using BNodeIDMap = std::map< CompactNodeId, CompactBlkId >;

    struct FullBNodeIdMap {
        //
        // Why std::map with mutex instead of undrdered_map or concurrenthashmap?
        //
        // We persist this map in sorted by nodeid fashion, so as to pack consecutive nodes together. Given that we try
        // to allocate node ids in consective manner, such structure would result in significant savings in persisting
        // data size and thus performance.
        BNodeIDMap m_map;
        mutable iomgr::FiberManagerLib::shared_mutex m_mtx;

        // Why persisting as a chain instead of meta_blks
        //
        // Metablk as of now expects the entire map to be created in one large memory area and then persist them in
        // pieces synchronously. For such a large map, this could be very slow, since only 1 thread will be doing IO for
        // large map. The approach here uses link of the blkid (similar to metablk_mgr), but we persist it everytime we
        // need to find a fragment or break in chain (every link) and also concurrently. This should speed up the
        // persistence of the map.   // List of locations where bnodeid maps are chained together
        //
        std::vector< BlkId > m_locations;

        // Keeping track of number of updates in the map since last full map flush. This prevents unnecessary full flush
        // on dormant btrees
        std::atomic< uint64_t > m_updates_since_last_flush{0};
    };

    using DirtyNodeList = sisl::ConcurrentInsertVector< BtreeNodePtr >;
    using DeletedNodeList = sisl::ConcurrentInsertVector< CompactNodeId >;

    struct CPSession {
    public:
        /////////////// All Dirtying operation related ///////////////////////
        COWBtree& m_bt;
        cp_id_t m_cp_id{-1};
        DirtyNodeList m_modified_nodes;
        DeletedNodeList m_deleted_nodes;
        std::atomic< bnodeid_t > m_new_root_id{empty_bnodeid};

        /////////////// Common flushing related entitites ///////////////////////
        SCOPED_ENUM_DECL(FlushState, uint8_t);
        iomgr::FiberManagerLib::mutex m_flush_mtx;
        FlushState m_state;
        int32_t m_flushing_req_count{0};

        /////////////// Node flush related entities ///////////////////////
        std::vector< BlkId > m_node_locations;
        size_t m_next_location_idx;             // Next blkid to pick for next unit
        DirtyNodeList::iterator m_modified_it;  // Iterator of the dirtied nodes
        DeletedNodeList::iterator m_deleted_it; // Iterator of the deleted nodes
        uint32_t m_modified_count;              // Cache the modified nodes
        uint32_t m_deleted_count;               // Cache them since deleted_nodes_.size() is an expensive operation
        unique< Journal > m_journal;

        /////////////// Map and SB flush related entities ///////////////////////
        BNodeIDMap::iterator m_next_full_map_it;
        uint32_t m_parallel_flush_range{0};
        std::vector< std::vector< BlkId > > m_location_chains;

    public:
        CPSession(COWBtree& bt) : m_bt{bt} {}
        bool prepare_to_flush_nodes(COWBtreeCPContext* cp_ctx);
        std::tuple< BlkId, DirtyNodeList::iterator, sisl::blob > next_dirty();
        std::tuple< DeletedNodeList::iterator, DeletedNodeList::iterator, sisl::blob > next_deleted();
        bnodeid_t new_root();
        bool done_flushing_nodes();

        std::vector< std::pair< COWBtree::CompactNodeId, COWBtree::CompactBlkId > >
        prepare_to_flush_map(COWBtreeCPContext* cp_ctx);
        std::pair< bool, std::vector< std::vector< BlkId > > > done_flushing_map(std::vector< BlkId > map_locations);

        bool flush_sb(COWBtreeCPContext* cp_ctx);
        void finish();
    };

    friend class CPSession;

private:
    FullBNodeIdMap m_bnodeid_map;
    BtreeBase* m_base_btree;
    LargeIDReserver m_nodeid_generator;
    shared< VirtualDev > m_vdev;

    uint32_t m_btree_ordinal;
    uint64_t m_ordinal_shifted;

    // All dirty items for a btree for each cp is tracked here (instead in cp_ctx)
    std::array< unique< CPSession >, CPManager::max_concurent_cps > m_cp_sessions;

    // Flush related structures
    iomgr::FiberManagerLib::mutex m_flush_mtx;

private:
    void update_bnode_map(CompactNodeId nodeid, CompactBlkId blkid, bool in_recovery);
    void delete_from_bnode_map(CompactNodeId nodeid, bool in_recovery);
    void recover_bnode_map(BlkId const& map_loc);
    BlkId lookup_bnode_map(CompactNodeId nodeid) const;
    void apply_incremental_map(sisl::byte_view const& journal_buf);

    CPSession* cp_session(cp_id_t cp_id);

    BtreeSuperBlock const& bt_super_blk() const {
        return *(r_cast< BtreeSuperBlock const* >(m_base_btree->super_blk()->underlying_index_sb.data()));
    }

    BtreeSuperBlock& bt_super_blk() {
        return const_cast< BtreeSuperBlock& >(s_cast< const COWBtree* >(this)->bt_super_blk());
    }

    SuperBlock const& cow_bt_super_blk() const {
        return *(r_cast< SuperBlock const* >(bt_super_blk().underlying_btree_sb.data()));
    }

    SuperBlock& cow_bt_super_blk() {
        return const_cast< SuperBlock& >(s_cast< const COWBtree* >(this)->cow_bt_super_blk());
    }
};

SCOPED_ENUM_DEF(COWBtree::CPSession, FlushState, uint8_t, DIRTYING, NODES_FLUSHING, NODES_FLUSHED, MAP_FLUSHING,
                MAP_FLUSHED, ALL_DONE);
} // namespace homestore