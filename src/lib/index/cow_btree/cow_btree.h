#pragma once

#include <homestore/blk.h>
#include <homestore/btree/btree.hpp>
#include "common/large_id_reserver.hpp"

namespace homestore {
class Flusher;

class COWBtree : public UnderlyingBtree {
public:
    using CompactNodeId = uin32_t;
    using MapLocation = BlkId;
    using NodeLocation = CompactBlkId;

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
        cp_id_t cp_id;            // CPID when this superblock was written
        uint16_t num_map_heads;   // Total number of map heads
        MapLocation map_heads[1]; // Array of heads of chain which contains the blkid map data
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

        sisl::io_blob_safe base_buf_;
        uint8_t* cur_ptr_;
        uint32_t used_size_;

        Journal(uint32_t ordinal, uint32_t initial_size) :
                base_buf_{std::max(initial_size, sizeof(Header)), meta_service().align_size(), sisl::buftag::meta} {
            Header* hdr = new (base_buf_.bytes_) Header();
            hdr->size = initial_size;
            hdr->ordinal = ordinal;
            cur_ptr_ = base_buf_.bytes_ + sizeof(Header);
        }

        uint8_t* allocate(uint32_t num_bytes) {
            if (available_space() < num_bytes) {
                // We need to realloc the buffer and adjust the pointers. By default try to increase 50% more everytime
                // (instead of doubling).
                auto const cur_size = occupied_size();
                base_buf_.buf_realloc(std::max(num_bytes - available_space(), r_cast< double >(base_buf_.size_) * 1.5),
                                      meta_service().align_size(), sisl::buftag::meta);
                cur_ptr_ = base_buf_.bytes_ + cur_size;
                header()->size += num_bytes;
            }
            auto ret_ptr = cur_ptr;
            cur_ptr_ += num_bytes;
            return ret_ptr;
        }

        uint8_t* make_room(uint32_t num_bytes) {
            if (available_space() < num_bytes) {
                // We need to realloc the buffer and adjust the pointers.
                // By default try to increase 50% more everytime (instead of
                // doubling).
                base_buf_.buf_realloc(std::max(num_bytes - available_space(), r_cast< double >(base_buf_.size_) * 1.5),
                                      meta_service().align_size(), sisl::buftag::meta);
                cur_ptr_ = base_buf_.bytes_ + occupied_size();
            }
            return cur_ptr_;
        }

        Header* header() { return r_cast< Header* >(base_buf_.bytes_); }
        uint32_t occupied_size() const { return cur_ptr_ - base_buf_.bytes_; }
        uint32_t available_space() const { return (base_buf_.size - occupied_size()); }
    };

    using BNodeIDMap = std::map< CompactNodeId, NodeLocation >;

    struct FullBNodeIdMap {
        //
        // Why std::map with mutex instead of undrdered_map or concurrenthashmap?
        //
        // We persist this map in sorted by nodeid fashion, so as to pack consecutive nodes together. Given that we try
        // to allocate node ids in consective manner, such structure would result in significant savings in persisting
        // data size and thus performance.
        BNodeIDMap map_;
        iomgr::FiberManagerLib::shared_mutex mtx_;

        // Why persisting as a chain instead of meta_blks
        //
        // Metablk as of now expects the entire map to be created in one large memory area and then persist them in
        // pieces synchronously. For such a large map, this could be very slow, since only 1 thread will be doing IO for
        // large map. The approach here uses link of the blkid (similar to metablk_mgr), but we persist it everytime we
        // need to find a fragment or break in chain (every link) and also concurrently. This should speed up the
        // persistence of the map.   // List of locations where bnodeid maps are chained together
        //
        std::vector< MapLocation > map_locations_;

        // Keeping track of number of updates since last full map flush. This prevents unnecessary full flush on dormant
        // btrees
        std::atomic< uint64_t > updates_since_last_flush_{0};

#pragma pack(1)
        struct IndirectLocationSB {
            uint32_t num_locations_head;
            uint32_t checksum;
            BlkId location_heads[1];

            static uint32_t header_size() { return (sizeof(IndirectLocationSB) - sizeof(BlkId)); }
        };
#pragma pack()
    };

    using DirtyNodeList = sisl::ConcurrentInsertVector< BtreeNodePtr >;
    using DeletedNodeList = sisl::ConcurrentInsertVector< CompactNodeId >;

    struct CPSession {
    public:
        /////////////// All Dirtying operation related ///////////////////////
        COWBtree& bt_;
        cp_id_t cp_id_{-1};
        DirtyNodeList modified_nodes_;
        DeletedNodeList deleted_nodes_;
        std::atomic< bnodeid_t > new_root_id_{empty_bnodeid};
        std::atomic< int64_t > node_count_changes_{0};
        // unique< Flusher > flusher_;

        /////////////// Common flushing related entitites ///////////////////////
        ENUM(FlushState, uint8_t, DIRTYING, NODES_FLUSHING, NODES_FLUSHED, MAP_FLUSHING, MAP_FLUSHED, ALL_DONE);
        iomgr::FiberManagerLib::mutex flush_mtx_;
        FlushState state_;
        int32_t flushing_req_count_{0};

        /////////////// Node flush related entities ///////////////////////
        std::vector< BlkId > node_locations_;
        size_t next_location_idx_; // Next blkid to pick for next unit
        DirtyNodeList::iterator modified_it_;  // Iterator of the dirtied nodes
        DeletedNodeList::iterator deleted_it_; // Iterator of the deleted nodes
        uint32_t modified_count_;
        uint32_t deleted_count_; // Cache them since deleted_nodes_.size() is an expensive operation
        unique< Journal > journal_;

        /////////////// Map and SB flush related entities ///////////////////////
        BNodeIdMap::iterator next_full_map_it_;
        uint32_t parallel_flush_range_{0};
        std::vector< std::vector< MapLocation > > loc_array_list_;
        bool sb_persist_needed_{false};

    public:
        cp_id_t cp_id() const { return cp_id_; }

        Journal* journal() { return journal_.get(); }

        void start(COWBtree& bt, cp_id_t cp_id);

        void finish() {
            cp_id_ = -1;
            flusher_.reset();
            modified_nodes_.clear();
            modified_nodes_.clear();
            new_root_id_.store(empty_bnodeid);
            node_count_changes_.store(0);
        }

        bool prepare_to_flush_nodes(COWbtree& btree, COWBtreeCPContext* cp_ctx);
        std::tuple< BlkId, DirtyNodeList::iterator, sisl::blob > next_dirty();
        std::tuple< DeletedNodeList::iterator, DeletedNodeList::iterator, sisl::blob > next_deleted();
        std::pair< bnodeid_t, int64_t > next_sb_updates();
        bool done_flushing_nodes();

        bool prepare_to_flush_map(COWBtreeCPContext* cp_ctx);

    private:
        FullBNodeIdMap m_bnodeid_map;
        BtreeBase* m_base_btree;
        LargeIDReserver m_nodeid_generator;
        shared< VirtualDev > m_vdev;

        uint32_t m_btree_ordinal;
        uint64_t m_ordinal_shifted;

        // All dirty items for a btree for each cp is tracked here (instead in cp_ctx)
        std::array< CPSession, MAX_CONCURRENT_CPS > m_cp_sessions;

        // Flush related structures
        uint32_t m_max_nodes_per_flush;
        iomgr::FiberManagerLib::mutex m_flush_mtx;

    public:
        COWBtree(BtreeBase* bt, shared< VirtualDev > vdev, std::vector< sisl::byte_view > const& journal_bufs);
        ~COWBtree() = default;
        bnodeid_t generate_node_id();
        void add_to_dirty_list(BtreeNodePtr const& node, COWBtreeCPContext* cp_ctx);
        void add_to_remove_list(bnodeid_t node_id, COWBtreeCPContext* cp_ctx);

        sisl::io_blob_safe cp_flush(COWBtreeCPContext* cp_ctx);

    private:
        void update_bnode_map(CompactNodeId nodeid, NodeLocation blkid);
        void delete_from_bnode_map(CompactNodeId nodeid);
        void recover_full_bnode_map(BlkId const& map_loc);
        void apply_incremental_map(sisl::byte_view const& journal_buf);

        CPSession* cp_session(cp_id_t cp_id) { return &m_cp_sessions[cp_id % MAX_CONCURRENT_CPS]; }
        DirtyList& dirtylist(CPContext* cp_ctx) { return m_dirty_list[cp_ctx->id() % MAX_CONCURRENT_CPS]; }

        BtreeSuperBlock const& bt_super_blk() const { return super_blk()->btree_sb; }
        BtreeSuperBlock& bt_super_blk() {
            return const_cast< BtreeSuperBlock& >(s_cast< const COWBtree* >(this)->bt_super_blk());
        }

        COWBtreeSuperBlock const& cow_bt_super_blk() const {
            return *(r_cast< COWBtreeSuperBlock const* >(bt_super_blk().underlying_btree_sb));
        }
        COWBtreeSuperBlock& cow_bt_super_blk() {
            return const_cast< COWBtreeSuperBlock& >(s_cast< const COWBtree* >(this)->cow_bt_super_blk());
        }
    };
} // namespace homestore