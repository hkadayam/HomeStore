#include "index/cow_btree/cow_btree.h"

namespace homestore {
static constexpr uint64_t btree_nodeid_bits = sizeof(uint32_t) * 8;
static constexpr uint64_t btree_ordinal_bits = 64 - btree_nodeid_bits;
static constexpr uint64_t btree_nodeid_mask = ((1ull << btree_nodeid_bits) - 1);
static constexpr uint64_t btree_ordinal_mask = ((1ull << btree_ordinal_bits) - 1) << btree_nodeid_bits;

static constexpr uint32_t initial_bnodeid_map_persistent_size = 512 * 1024;

static inline COWBtreeNode* to_cow_btree_node(BtreeNodePtr const& n) {
    return r_cast< COWBtreeNode* >(uintptr_cast(n.get()) - sizeof(COWBtreeNode));
}

static inline CompactNodeId to_compact_nodeid(bnodeid_t node_id) { return nodeid & btree_nodeid_mask; }

static inline CompactNodeId get_compact_nodeid(COWBtreeNode* node) {
    return to_compact_nodeid(cow_node->to_btree_node()->node_id());
}

static BlkId alloc_blks_or_fail(VirtualDev* vdev, uint32_t size, blk_alloc_hints const& hints) {
    BlkId out_blkid;
    BlkAllocStatus status = vdev->alloc_contiguous_blks(size, hints, out_blkid);
    HS_REL_ASSERT_EQ(status, BlkAllocStatus::SUCCESS,
                     "No space to write the bnode map, which cannot be proceeded further, crashing the system for now");
    return out_blkid;
}

COWBtree::COWBtree(BtreeBase* bt, shared< VirtualDev > vdev, std::vector< sisl::byte_view > const& journal_bufs) :
        m_base_btree{bt},
        m_nodeid_generator(std::numeric_limits< uint32_t >::max()),
        m_vdev{std::move(vdev)},
        m_btree_ordinal{bt->super_blk()->ordinal},
        m_ordinal_shifted{m_btree_ordinal << btree_nodeid_bits},
        m_max_nodes_per_flush{((HS_DYNAMIC_CONFIG(btree->max_btree_write_size_per_io) - 1) / bt->node_size()) + 1} {
    // If we have full map persisted before, recover that
    for (uint32_t i{0}; i < cow_bt_super_blk().num_map_heads(); ++i) {
        recover_bnode_map(cow_bt_super_blk().map_heads[i]);
    }

    // Apply all incremental journal entries containing map updates/removes. Each journal_buf listed here corresponding
    // to a journal written as part of cps, sorted by the cp_id
    for (auto const& journal_buf : journal_bufs) {
        apply_incremental_map(journal_buf);
    }
}

bnodeid_t COWBtree::generate_node_id() { return (m_ordinal_shifted | m_nodeid_generator.reserve()); }

void COWBtree::add_to_dirty_list(BtreeNodePtr const& node, COWBtreeCPContext* cp_ctx) {
    cp_session(cp_ctx->id())->m_modified_nodes.insert(node);
    cp_ctx->m_dirty_node_count.increment(1);
}

void COWBtree::add_to_remove_list(bnodeid_t node_id, cp_id_t cp_id) {
    cp_session(cp_ctx->id())->m_deleted_nodes.insert(node);
    cp_ctx->m_removed_node_count.increment(1);
}

void COWBtree::on_root_changed(BtreeNodePtr const& new_root, COWBtreeCPContext* cp_ctx) {
    cp_session(cp_ctx->id())->m_new_root_id.store(new_root->node_id());
}

void COWBtree::on_btree_destroyed() {
    // Walk through the entire map and free all the node blks and then free the map blks. This whole operation needs to
    // be done under a lock CPGuard, because during this process a CP should not be taken.
    CPGuard cpg;

    {
        // Free all the blks allocated for the nodes
        std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.m_mtx);
        for (auto const [nodeid, blkid] : m_bnode_map.m_map) {
            m_vdev->free_blk(blkid.to_blkid());
        }

        // Free all the blks allocated for the map
        for (auto const& locs : m_bnodeid_map.m_locations) {
            m_vdev->free_blk(blkid.to_blkid());
        }

        // Reset the map, cp_session etc.
        m_bnode_map.m_map.clear();
        m_updates_since_last_flush = 0;
        m_bnode_map.m_locations.clear();
    }

    // Reset all the dirty nodes, deleted nodes etc.
    for (auto& cp_session : m_cp_sessions) {
        cp_session.reset();
    }

    // Destroy this btree's superblk, so that it can be re-initialized again.
    m_base_btree->super_blk().destroy();
}

// FlushUnit represents one contiguous block where all btree nodes that can be packed are done and written at once
struct NodeFlushUnit {
#pragma pack(1)
    struct JournalEntry {
        CompactBlkId nodes_location; // Location where nodes from this unit are written
        uint16_t n_nodes{0};         // Total number of nodes written
        CompactNodeId nodes[1];      // Array of node ids written in the blk above

        uint32_t size() const { size(n_nodes); }
        static uint32_t size(uint16_t num_nodes) {
            return sizeof(JournalEntry) + (num_nodes * sizeof(CompactNodeId)) - sizeof(CompactNodeId)
        }
    };
#pragma pack()

    COWBtreeCPContext* m_cp_ctx_;
    JournalEntry* m_jentry{nullptr};
    std::vector< const iovec* > m_iovs;
    BlkId m_nodes_location;
    uint32_t m_nodes_count{0};

    NodeFlushUnit(COWBtreeCPContext* cp_ctx, BlkId location, sisl::blob const& journal_area) :
            m_cp_ctx{cp_ctx},
            m_jentry{r_cast< JournalEntry* >(journal_area.bytes_)},
            m_iovs.reserve(location.blk_count()),
            m_nodes_location{location} {
        if (jentry) { jentry->nodes_location = location; }
    }

    void add(COWBtreeNode* cow_node) {
        HS_DBG_ASSERT_LT(m_nodes_count, m_nodes_location.blk_count(), "Adding more nodes than node allocated for");
        iovs_.emplace_back(iovec{.iov_base = cow_node->get_flush_version_buf(m_cp_ctx_->id()),
                                 .iov_len = cow_node->to_btree_node()->node_size()});
        ++m_nodes_count;
        if (m_jentry) { m_jentry->nodes[m_jentry->n_nodes++] = get_compact_nodeid(cow_node); }
    }
};

struct BNodeMapWriteUnit {
#pragma pack(1)
    struct Header {
        CompactBlkId next_unit_location; // Location of where the next meta (for map) is present
        uint32_t size{sizeof(Header)};   // Total size of this unit.
        uint32_t n_entries{0};           // Total number of entries in this unit
        uint32_t checksum{0};            // Checksum excluding this header
    };

    // One Entry per continguos nodeids.
    struct MapEntry {
        CompactNodeId nodeid_start{0};
        uint16_t nodes_count{0};
        CompactBlkId nodes_locations[1];

        static size_t size(uint32_t count) {
            return sizeof(MapEntry) + (count ? (count - 1) * sizeof(CompactBlkId) : 0);
        }
        size_t size() const { return size(nodes_count); }

        bool merge_if_possible(CompactNodeId n, CompactBlkId b) {
            if (nodes_count == 0) {
                nodeid_start = n;
                nodes_locations[nodes_count++] = b;
                return true;
            } else if ((nodeid_start + nodes_count) == n) {
                nodes_locations[nodes_count++] = b;
                return true;
            }
            return false;
        }
    };
#pragma pack()

private:
    VirtualDev* m_vdev;
    sisl::io_blob_safe m_buf;
    uint32_t m_available_space{0};
    BlkId m_location;
    MapEntry* m_cur_entry{nullptr};

public:
    // Guess the size expecting 64 nodes packed together.
    static constexpr const uint32_t expected_nodes_packed_per_entry = 64;

    static uint32_t size_guess(uint32_t num_nodes) {
        return MapEntry::size(num_nodes / expected_nodes_packed_per_entry);
    }

    static constexpr uint32_t const min_blks_per_write_unit = 128;

    BNodeMapWriteUnit(VirtualDev* vdev, uint32_t nodes_count) : m_vdev{vdev} {
        m_available_space = sisl::round_up(size_guess(nodes_count), m_vdev->block_size());
        auto const reqd_blks = (m_available_space - 1) / m_vdev->block_size() + 1;

        // First allocate the blks and adjust the available space to how much ever we were able to allocate
        // contiguously.

        blk_alloc_hints hints = {.partial_alloc_ok = true,
                                 .min_blks_per_piece = std::min(reqd_blks, min_blks_per_write_unit)};
        m_location = alloc_blks_or_fail(m_vdev, m_available_space, hints);
        m_available_space = m_location.blk_count() * m_vdev->block_size();

        // Allocate buffer to hold up that much disk space we allocated.
        m_buf = sisl::io_blob_safe(m_available_space, vdev->align_size(), sisl::buftag::metablk);
        memset(m_buf.bytes_, 0, m_available_space);

        // Initialize the in-memory pointers
        new (m_buf.bytes_) Header();
        m_available_space -= sizeof(Header);
        m_cur_entry = r_cast< MapEntry* >(m_buf.bytes_ + sizeof(Header));
    }

    // Recovery constructor
    BNodeMapWriteUnit(VirtualDev* vdev, sisl::io_blob_safe buf, BlkId location) :
            m_vdev{vdev}, m_buf{std::move(buf)}, m_location{location} {
        HS_DBG_ASSERT_GE(m_buf.size_, header()->size, "Read buf is less than MapWriteUnit size on-disk");
        HS_REL_ASSERT_EQ(header()->crc, compute_crc(), "CRC Mismatch on MapWriteUnit");

        m_available_space = m_buf.size_ - header()->size;
        m_cur_entry = header()->n_entries ? r_cast< MapEntry* >(m_buf.bytes_ + sizeof(Header)) : nullptr;
    }

    bool has_room() const { return (m_available_space > MapEntry::size(1)); }

    bool is_empty() const { return (header()->size == sizeof(Header)); }

    void add_entry(CompactNodeId n, CompactBlkId b) {
        HS_REL_ASSERT_EQ(has_room(), true, "Calling add_entry without any room");
        if (m_cur_entry->merge_if_possible(n, b)) {
            header()->size += sizeof(CompactBlkId);
            m_available_space -= sizeof(CompactBlkId);
        } else {
            ++(header()->n_entries);
            m_cur_entry = r_cast< MapEntry* >(uintptr_cast(m_cur_entry) + m_cur_entry->size());
            m_cur_entry->merge_if_possible(n, b);

            m_available_space -= MapEntry::size(1u);
            header()->size += MapEntry::size(1u);
        }
    }

    MapEntry* next_entry() {
        MapEntry* ret_entry = m_cur_entry;
        if (m_cur_entry) {
            uint8_t* next_ptr = uintptr_cast(m_cur_entry) + m_cur_entry->size();
            m_cur_entry = (next_ptr > (m_buf.bytes_ + m_buf.size)) ? nullptr : r_cast< MapEntry* >(next_ptr);
        }
        return ret_entry;
    }

    void link(BNodeMapWriteUnit& next) { header()->next_unit_location = CompactBlkId{next->m_location}; }

    void finialize() {
        ++(header()->n_entries); // We increment as the last entry would be open until we finalize

        // Trim down the alloc size and actual blks (if we alloced them)
        auto const occupied_blks = m_location.blk_count() - (m_available_space / m_vdev->block_size());
        auto const [valid, freeable] = m_location.split(occupied_blks);
        m_vdev->free_blks(freeable);
        m_location = valid;
        m_available_space = 0;

        // Write the checksum
        auto const crc = crc32_ieee(init_crc32, s_cast< const uint8_t* >(header()) + sizeof(Header),
                                    header()->size - sizeof(Header));
        header()->checksum = crc;
    }

private:
    Header* header() { return r_cast< Header* >(m_buf.bytes_); }

    uint32_t compute_crc() const {
        return crc32_ieee(init_crc32, s_cast< const uint8_t* >(header()) + sizeof(Header),
                          header()->size - sizeof(Header));
    }
};

std::tuple< bool, sisl::io_blob_safe, bool > COWBtree::flush_nodes(COWBtreeCPContext* cp_ctx) {
    CPSession* session = cp_session(cp_ctx->id());

    // We prepare to flush nodes, by allocating blks in vdev to accomodate all the dirty blks. Its obviously not
    // possible to put all nodes in a single huge contiguous blk. However, it tries to allocate as big as possible and
    // then pack nodes inside these blks.
    if (!session->prepare_to_flush_nodes(*this, cp_ctx)) {
        // Already flushed the cp and moved on.
        return {false, nullptr, false};
    }

    // 3 steps on per btree CP node flush
    //
    // Step 1: Flush all the nodes by building flush units (with each unit consists of 1 contiguous blk worth) and
    // while doing so, keep updating the in-memory map as well as adding to incremental journal with the map
    // updates.
    do {
        auto [location, mod_it, journal_area] = session->next_dirty();
        if (!location.is_valid()) {
            break; // We are done with dirty buffers
        }

        NodeFlushUnit nfunit(cp_ctx, location, journal_area);
        for (uint16_t i{0}; i < location.blk_count(); ++i) {
            BtreeNodePtr node = *mod_it;
            ++mod_it;
            nfunit.add(to_cow_btree_node(node));

            // Keep updating the full inmemory map of nodeid and blkid.
            // IMPORTANT NODE: We do that before actually writing the data. It is ok to do so, under the assumption that
            // there will be no reads into the bnode map while this is being flushed because nodes are cached until
            // flush is completed. If for any reason we need to support skipping cache, then we should update this bnode
            // map after it has been written. We are doing this here as an optimization to avoid looping for every node
            // and then update.
            update_bnode_map(get_compact_nodeid(to_cow_btree_node(node)), CompactBlkId{location, i},
                             false /* in_recovery */);
        }

        auto err = m_vdev->sync_writev(nfunit.iovs_.data(), nfunit.iovs_.size(), nfunit.m_location);
        HS_REL_ASSERT(!err, "Flush of nodes failed during cp, best is to crash the system and retry on reboot");
    } while (true);

    if (session->done_flushing_nodes()) {
        //
        // Step 2: During cp io phase, all deleted nodes are tracked, we delete them from in-memory map and also
        // build the journal with this delete operation.
        //
        auto [it, end_it, journal_area] = session->next_deleted();
        uint8_t* delete_jentries = r_cast< CompactNodeId* >(journal_area.bytes_);
        uint32_t deleted_count = 0;
        while (it != end_it) {
            auto nodeid = *it;
            delete_from_bnode_map(nodeid, false /* in_recovery */);
            if (delete_jentries) { delete_jentries[m_deleted_count++] = nodeid; }
            ++it;
        }

        //
        // Step 3: Check if there are any changes that needs changes in superblock of the btree. If so modify them. We
        // do not persist them yet, because we need all fibers to finish flushing, write journal etc before committing
        // to superblock to disk. In this step, we just updated the superblk based on what has been dirtied earlier.
        //
        auto const new_root = session->new_root();
        bool sb_changed{false};
        if (new_root != empty_bnodeid) {
            bt_super_blk()->root_node = new_root;
            sb_changed = true
        }

        CP_PERIODIC_LOG(DEBUG, cp_ctx->id(), "Btree={} has flushed {} dirty nodes and deleted {} nodes", ordinal(),
                        session->modified_node_count(), session->deleted_node_count());
        m_bnodeid_map.m_updates_since_last_flush.fetch_add(session->modified_node_count() +
                                                           session->deleted_node_count());

        // If either map has to be updated or sb is changed, we need to hold onto the session and it will be completed
        // after that is done. Otherwise, we can complete the session now (which means all dirty node list, deleted node
        // list, journal and everything has been cleaned)
        if (sb_changed || cp_ctx->need_full_map_flush()) {
            return std::make_tuple(session->modified_node_count() || session->deleted_node_count(),
                                   std::move(session->m_journal), true);
        } else {
            auto const ret = std::make_tuple(session->modified_node_count() || session->deleted_node_count(),
                                             std::move(session->m_journal), sb_changed);
            session->finish();
            return ret;
        }
    } else {
        return std::make_tuple(false, nullptr, false);
    }
}

void COWBtree::flush_map_and_sb(COWBtreeCPContext* cp_ctx) {
    HS_DBG_ASSERT(cp_ctx->need_full_map_flush(), "Flush map called on a cp which doesn't need full map flush");

    if (m_bnodeid_map.m_updates_since_last_flush.load() == 0) {
        CP_PERIODIC_LOG(DEBUG, "For Btree={} there was no update of the bnodeid map since last flush, so ignoring");
        return;
    }

    CPSession* session = cp_session(cp_ctx->id());
    auto const [it, count] = session->prepare_to_flush_map(*this, cp_ctx);

    std::vector< BlkId > map_locations;
    // Worst Estimate of 1 entry per count packed in a single blk
    map_locations.reserve((MapEntry::size(1) * count) / m_vdev->block_size());

    while (count > 0) {
        BNodeMapWriteUnit munit = std::make_unique< BNodeMapWriteUnit >(m_dev.get(), count);
        if (!munit->has_room()) {
            auto new_unit = std::make_unique< BNodeMapWriteUnit >(m_dev, count);
            munit->link(*new_unit);
            munit->finalize();
            auto err = m_vdev->sync_write(munit->m_buf.bytes_, mwunit->m_buf.size_, mwunit->m_location);
            HS_REL_ASSERT(!err, "Flush of full map failed with err={}. best is to crash the system and replay", err);
            map_locations.emplace_back(munit->m_location);

            munit = std::move(new_unit);
        }
        munit->add_entry(it->first, it->second);
        ++it;
        --count;
    }

    if (!munit->is_empty()) {
        munit->finalize();

        auto err = m_vdev->sync_write(mwunit->m_buf.bytes, mwunit->m_buf.size, mwunit->m_location);
        HS_REL_ASSERT(!err, "Flush of full map failed with err={}. best is to crash the system and replay", err);

        map_locations.emplace_back(munit->m_location);
    }

    auto const [done, all_map_locations] = session->done_flushing_map(std::move(map_locations));
    if (!done) {
        // Still there are other fibers flushing the map.
        return;
    }

    // We are the last fiber to finish parallel flush of map, its time to update the superblk with all map locations and
    // flush the superblk and free up old map blks.
    SuperBlock* sb = cow_bt_super_blk().get();
    sb->num_map_heads = 0;
    sb->cp_id = cp_ctx->id();
    for (auto const& map_locs : all_map_locations) {
        sb->map_heads[sb->num_map_heads++] = map_locs[0]; // Pick head of each map locs from different fibers
    }

    // Persist the superblk now
    flush_sb(cp_ctx);

    // We have completed the flush of map and now we can free up the old map blks
    for (auto const& loc : m_bnodeid_map.m_locations) {
        m_vdev->free_blks(loc);
    }

    // We need to replace the previous map_locations in-memory with this new set of locations where map is written
    m_bnodeid_map.m_locations.clear();
    for (auto const& loc_array : all_map_locations) {
        m_bnodeid_map.m_locations.insert(m_bnodeid_map.m_locations.end(), loc_array.begin(), loc_array.end());
    }
    m_bnodeid_map.m_updates_since_last_flush.store(0); // Reset the count, as we just flushed the full map
}

void COWBtree::flush_sb(COWBtreeCPContext* cp_ctx) {
    CPSession* session = cp_session(cp_ctx->id());
    bt.mutable_superblk().write();
    session->finish();
}

void COWBtree::update_bnode_map(CompactNodeId nodeid, CompactBlkId cblkid, bool in_recovery) {
    auto do_update = [this](CompactNodeId nodeid, CompactBlkId cblkid) {
        auto it = m_bnode_map.m_map.find(nodeid);
        if (it != m_bnode_map.m_map.end()) {
            m_vdev->free_blk(it->second.to_blkid());
            it->second = cblkid;
        } else {
            m_bnode_map.m_map.emplace(nodeid, cblkid);
        }
    };

    if (in_recovery) {
        do_update();
        m_nodeid_generator.reserve(nodeid);
        m_vdev->commit_blk(cblkid.to_blkid());
    } else {
        std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.m_mtx);
        do_update();
    }
}

void COWBtree::delete_from_bnode_map(CompactNodeId nodeid, bool in_recovery) {
    auto do_delete = [this](CompactNodeId nodeid) {
        m_vdev->free_blk(lookup_bnode_map(nodeid));
        m_bnode_map.m_map.erase(nodeid);
        m_nodeid_generator.unreserve(nodeid);
    };

    if (in_recovery) {
        std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.m_mtx);
        do_delete();
    } else {
        do_delete();
    }
}

BlkId COWBtree::lookup_bnode_map(CompactNodeId nodeid) const {
    std::shared_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.m_mtx);
    auto const it = m_bnode_map.m_map.find(nodeid);
    return (it == m_bnode_map.m_map.cend()) ? BlkId{} : it->second.to_blkid();
}

void COWBtree::recover_bnode_map(BlkId const& map_loc) {
    BT_LOG(INFO, "Recovering NodeID to blkid from location=[{}]", map_loc);

    BlkId next_loc = map_loc;
    do {
        auto [ec, buf] = m_vdev->sync_read(next_loc);
        HS_REL_ASSERT(!ec, "Error while reading bnodeid map, cannot proceed further");

        m_vdev->commit_blk(next_loc);
        m_bnodeid_map.m_locations.push_back(next_loc);

        BNodeMapWriteUnit munit(m_vdev.get(), std::move(buf), next_loc);
        for (uint32_t i{0}; i < munit.n_entries; ++i) {
            BNodeMapWriteUnit::MapEntry* e = munit.next_entry();
            for (uint32_t n{0}; n < e->nodes_count; ++n) {
                update_bnode_map(e->nodeid_start + n, e->node_locations[n], true /* in_recovery */);
            }
        }
        next_loc = munit.header()->next_unit_location.to_blkid();
    } while (next_loc.is_valid());
}

void COWBtree::used_size() const {
    std::shared_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.m_mtx);
    return m_bnodeid_map.m_map.size() * m_vdev->block_size();
}

void COWBtree::apply_incremental_map(sisl::byte_view const& journal_buf) {
    auto jhdr = r_cast< Journal::Header* >(journal_buf.bytes());
    HS_REL_ASSERT_EQ(jhdr->ordinal, m_btree_ordinal, "Btree Ordinal mismatch between journal and in-memory");

    uint8_t* cur_ptr = jhdr + sizeof(Journal::Header);
    for (uint32_t i{0}; i < jhdr->num_flush_units; ++i) {
        NodeFlushUnit::JournalEntry* nf_jentry = r_cast< NodeFlushUnit::JournalEntry* >(cur_ptr);
        for (uint16_t n{0}; n < nf_jentry->n_nodes; ++n) {
            update_bnode_map(nodes[i], CompactBlkId{nodes_location, n}, true /* in_recovery */);
        }
        cur_ptr += nf_jentry->size();
    }

    auto* deleted_nodes = r_cast< CompactNodeId* >(cur_ptr);
    for (uint32_t i{0}; i < jhdr->num_delete_units; ++i) {
        delete_from_bnode_map(deleted_nodes[i], true /* in_recovery */);
    }
}

////////////////////////////////////////////// CPSession Section //////////////////////////////////////////////
bool COWBtree::CPSession::prepare_to_flush_nodes(COWBtreeCPContext* cp_ctx) {
    std::lock_guard lg{m_flush_mtx};

    if (m_state == FlushState::NODES_FLUSHED) {
        return false; // Bail out if we have already flushed this session
    } else if (m_state == FlushState::NODES_FLUSHING) {
        ++m_flushing_req_count;
        return true; // Everything is prepared already, join the flush
    }

    m_modified_count = m_modified_nodes.size();
    m_deleted_count = m_deleted_nodes.size();

    if ((m_modified_count == 0) & (m_deleted_count == 0)) {
        // Nothing has been dirtied in this btree in this session to flush.
        m_state = FlushState::NODES_FLUSHED;
        return false;
    }

    auto const status = m_bt.m_vdev->alloc_blks(
        mod_node_count, blk_alloc_hints{.min_blks_per_piece = std::min(mod_node_count, min_blks_per_write_unit)},
        m_node_locations);
    if ((status != BlkAllocStatus::SUCCESS) || (status != BlkAllocStatus::PARTIAL)) {
        HS_REL_ASSERT(false, "Blk allocation to persist btree pages failed, we are crashing for now");
    }

    // Setup all the iterators
    m_next_location_idx = 0;
    m_modified_it = m_modified_nodes.begin();
    m_deleted_it = m_deleted_nodes.begin();

    if (!cp_ctx->need_full_map_flush()) {
        // Setup the journal buffers
        // Size deterimination:
        // One location which is a blkid corresponds to 1 flush unit, so total journal size would be
        // Journal Header + (Number of flush units * Flush unit header) + Number of nodes + Number of deleted nodes
        auto const journal_size = sizeof(Journal::Header) +
            (NodeFlushUnit::JournalEntry::size(0) * m_node_locations.size()) +
            ((node_count + m_deleted_count) * sizeof(CompactNodeId));
        m_journal = std::make_unique< Journal >(m_bt.ordinal(), journal_size);
        m_journal->header()->num_flush_units = mod_node_count;
        m_journal->header()->num_delete_units = m_deleted_count;
    }

    m_state = FlushState::NODES_FLUSHING;
    ++m_flushing_req_count;
    return true;
}

std::tuple< BlkId, DirtyNodeList::iterator, sisl::blob > COWBtree::CPSession::next_dirty() {
    std::lock_guard lg{m_flush_mtx};
    HS_DBG_ASSERT_EQ(m_state, FlushState::FLUSHING,
                     "Unexpected state while pulling a dirty nodes, we expect all fibers have drained the iterator "
                     "before moving to flushed or collecting state");

    sisl::blob ret_blob;
    if (m_next_location_idx == m_node_locations.size()) {
        // We have reached the end of all nodes location's, which means there should be no more dirty
        HS_DBG_ASSERT(m_modified_it == m_modified_nodes.end(),
                      "Mismatch between number of blks allocated for node and dirty node iterator");
        return std::make_tuple(BlkId{}, m_modified_it, ret_blob);
    } else {
        HS_DBG_ASSERT(
            m_modified_it != m_modified_nodes.end(),
            "There are more blks allocated for nodes, but the dirty list doesn't have anymore node to fill it in");
        return std::make_tuple(BlkId{}, m_modified_it, ret_blob);
    }

    BlkId ret_loc = m_node_locations[m_next_location_idx++];
    auto ret_it = m_modified_it;
    m_modified_it += ret_loc.blk_count(); // Move the iterator past the blk_count().

    if (m_journal) {
        auto junit_size = NodeFlushUnit::JournalEntry::size(ret_loc.blk_count());
        ret_blob = sisl::blob{m_journal->allocate(junit_size), junit_size};
    }
    return std::make_tuple(ret_loc, ret_it, ret_blob);
}

std::tuple< DeletedNodeList::iterator, uint32_t, sisl::blob > COWBtree::CPSession::next_deleted() {
    std::lock_guard lg{m_flush_mtx};
    HS_DBG_ASSERT_EQ(m_state, FlushState::FLUSHING,
                     "Unexpected state while pulling a dirty nodes, we expect all fibers have drained the iterator "
                     "before moving to flushed or collecting state");
    auto ret_it = m_deleted_it;
    m_deleted_it = m_deleted_nodes.end(); // Set to end, so that any subsequent requests will get ret_it as end iterator

    sisl::blob ret_blob;
    if (m_journal) {
        auto const jdel_size = m_deleted_count * sizeof(CompactNodeId);
        ret_blob = sisl::blob{m_journal->allocate(jdel_size), jdel_size};
    }
    return std::make_tuple(std::move(ret_it), m_deleted_nodes.end(), std::move(ret_blob));
}

bnodeid_t COWBtree::CPSession::new_root() { return session->new_root_id.exchange(empty_bnodeid); }

bool COWBtree::CPSession::done_flushing_nodes() {
    std::lock_guard lg{m_flush_mtx};
    HS_DBG_ASSERT_EQ(m_state, FlushState::NODES_FLUSHING,
                     "Received a flush done while state was not in flushing, some race condition?");
    if (--m_flushing_req_count == 0) {
        m_state = FlushState::NODES_FLUSHED;
        return true;
    }
    return false;
}

std::pair< BNodeIDMap::iterator, uint32_t > COWBtree::CPSession::prepare_to_flush_map(COWBtreeCPContext* cp_ctx) {
    std::lock_guard lg{m_flush_mtx};
    if (m_state == FlushState::MAP_FLUSHING) {
        // Some other fiber has started the flushing, get the next range of maps and iterate over and start flushing
        ++m_flushing_req_count;
        auto ret_it = m_next_full_map_it;
        m_next_full_map_it += m_parallel_flush_range;
        return std::pair(ret_it, m_parallel_flush_range);
    } else if (m_state != FlushState::NODE_FLUSHED) {
        // The nodes themselves have not been flushed or we have already finished flushing map, so we don't need to
        // anything now
        return (std::pair(m_next_full_map_it, 0));
    } else {
        // First fiber to start flushing, prepare the iterator. First flusher wll also get reminder of range also
        HS_DBG_ASSERT_EQ(m_flushing_req_count, 0, "In NODE_FLUSHED state, but outstanding count is non zero");
        auto& m = m_bt.m_bnodeid_map.m_map;

        // First fiber to flush the full map in this session. All fibers get equal portion to flush except the first
        // one which gets additional
        m_parallel_flush_range = m.size() / cp_ctx->m_parallel_flushers_count;
        auto this_count = m_parallel_flush_range + (m.size() % cp_ctx->m_parallel_flushers_count);
        m_next_full_map_it = m.begin() + this_count;

        m_state = FlushState::MAP_FLUSHING;
        return (m.begin(), this_count);
    }
}

std::pair< bool, std::vector< std::vector< BlkId > > >
COWBtree::CPSession::done_flushing_map(std::vector< BlkId > map_locations) {
    std::lock_guard lg{m_flush_mtx};
    HS_DBG_ASSERT_EQ(m_state, FlushState::MAP_FLUSHING,
                     "Received a flush done while state was not in flushing, some race condition?");

    m_location_chains.emplace_back(std::move(map_locations));
    if (--m_flushing_req_count != 0) { return std::pair(false, {}); }

    m_state = FlushState::MAP_FLUSHED;
    return std::pair(true, std::move(m_location_chains));
}

void COWBtree::CPSession::finish() {
    std::lock_guard lg{m_flush_mtx};
    m_modified_nodes.clear();
    m_deleted_nodes.clear();
    m_new_root_id.store(empty_bnodeid);
    m_state = FlushState::ALL_DONE;
    m_flushing_req_count = 0;

    m_node_locations.reset();
    m_next_location_idx = 0;
    m_modified_it = m_modified_nodes.end();
    m_deleted_it = m_deleted_nodes.end();
    m_modified_count = 0;
    m_deleted_count = 0;
    m_journal.reset();

    m_next_full_map_it = m_bt.m_bnodeid_map.m_map.end();
    m_parallel_flush_range = 0;
    m_location_chains.clear();
}
} // namespace homestore