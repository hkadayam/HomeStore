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
    auto const num_map_sb_heads = cow_bt_super_blk().num_map_head;
    for (uint32_t i{0}; i < num_map_sb_heads; ++i) {
        recover_bnode_map(cow_bt_super_blk().map_heads[i]);
    }

    // Apply all incremental journal entries containing map updates/removes. Each journal_buf listed here corresponding
    // to a journal written as part of cps, sorted by the cp_id
    for (auto const& journal_buf : journal_bufs) {
        apply_incremental_map(journal_buf);
    }
}

bnodeid_t COWBtree::generate_node_id() { return (m_ordinal_shifted | m_nodeid_generator.reserve()); }

void COWBtree::add_to_dirty_list(BtreeNodePtr const& node, cp_id_t cp_id) {
    dirty_list(cp_ctx).added().insert(node);
    cp_ctx->dirty_buf_count_.increment(1);
}

void COWBtree::add_to_remove_list(bnodeid_t node_id, cp_id_t cp_id) {
    dirty_list(cp_ctx).deleted().insert(node);
    cp_ctx->removed_node_count_.increment(1);
}

void COWBtree::node_count_update(int64_t changes, cp_id_t cp_id) {
    dirty_list(cp_ctx).count_changes().fetch_add(changes);
}

void COWBtree::update_bnode_map(CompactNodeId nodeid, CompactBlkId blkid) {
    std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.mtx_);
    auto const [it, happened] = m_bnode_map.map_.insert_or_assign(nodeid, blkid);
    if (!happened) { HS_LOG_ASSERT(!happened, "Updating node_id {} to bnode map failed", nodeid); }
    updates_since_last_flush_.fetch_add(1);
}

void COWBtree::delete_from_bnode_map(CompactNodeId nodeid) {
    std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.mtx_);
    m_bnode_map.map_.erase(nodeid);
    updates_since_last_flush_.fetch_add(1);
}

// FlushUnit represents one contiguous block where all btree nodes that can be packed are done and written at once
struct NodeFlushUnit {
#pragma pack(1)
    struct JournalEntry {
        BlkId nodes_location;   // Location where nodes from this unit are written
        uint16_t n_nodes{0};    // Total number of nodes written
        CompactNodeId nodes[1]; // Array of nodes
    };
#pragma pack()

    COWBtreeCPContext* cp_ctx_;
    JournalEntry* jentry_{nullptr};
    std::vector< const iovec* > iovs_;
    BlkId locations_;
    uint32_t nodes_count{0};

    NodeFlushUnit(COWBtreeCPContext* cp_ctx, BlkId location, sisl::blob const& journal_area) :
            cp_ctx_{cp_ctx},
            jentry_{r_cast< JournalEntry* >(journal_area.bytes_)},
            iovs_.reserve(location.blk_count()),
            locations_{location} {
        if (jentry) { jentry->nodes_location = location; }
    }

    void add(COWBtreeNode* cow_node) {
        HS_DBG_ASSERT_LT(nodes_count, locations_.blk_count(), "Adding more nodes than node allocated for");
        iovs_.emplace_back(iovec{.iov_base = cow_node->get_flush_version_buf(cp_ctx_->cp_id()),
                                 .iov_len = cow_node->to_btree_node()->node_size()});
        ++nodes_count;
        if (jentry_) { jentry_->nodes[jentry_->n_nodes++] = get_compact_nodeid(cow_node); }
    }

    static uint32_t journal_entry_size(uint32_t num_nodes) {
        return sizeof(JournalEntry) + (num_nodes * sizeof(CompactNodeId)) - sizeof(CompactNodeId);
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
    struct Entry {
        CompactNodeId nodeid_start{0};
        uint16_t nodes_count{0};
        CompactBlkId node_locations[1];

        static size_t size(uint32_t count) { return sizeof(Entry) + (count ? (count - 1) * sizeof(CompactBlkId) : 0); }
        size_t size() const { return size(nodes_count); }

        bool merge_if_possible(CompactNodeId n, CompactBlkId b) {
            if (nodes_count == 0) {
                nodeid_start = n;
                node_locations[nodes_count++] = b;
                return true;
            } else if ((nodeid_start + nodes_count) == n) {
                node_locations[nodes_count++] = b;
                return true;
            }
            return false;
        }
    };
#pragma pack()

private:
    VirtualDev* vdev_;
    sisl::io_blob_safe buf_;
    uint32_t available_space_{0};
    BlkId location_;
    Entry* cur_entry_{nullptr};

public:
    // Guess the size expecting 64 nodes packed together.
    static constexpr const uint32_t expected_nodes_packed_per_entry = 64;

    static uint32_t size_guess(uint32_t num_nodes) { return Entry::size(num_nodes / expected_nodes_packed_per_entry); }

    static constexpr uint32_t const min_blks_per_write_unit = 128;

    BNodeMapWriteUnit(VirtualDev* vdev, uint32_t nodes_count) : vdev_{vdev} {
        available_space_ = sisl::round_up(size_guess(nodes_count), vdev_->block_size());
        auto const reqd_blks = (available_space_ - 1) / vdev_->block_size() + 1;

        // First allocate the blks and adjust the available space to how much ever we were able to allocate
        // contiguously.

        blk_alloc_hints hints = {.partial_alloc_ok = true,
                                 .min_blks_per_piece = std::min(reqd_blks, min_blks_per_write_unit)};
        location_ = alloc_blks_or_fail(vdev_, available_space_, hints);
        available_space_ = location_.blk_count() * vdev_->block_size();

        // Allocate buffer to hold up that much disk space we allocated.
        buf_ = sisl::io_blob_safe(available_space_, vdev->align_size(), sisl::buftag::metablk);
        memset(buf_.bytes, 0, available_space_);

        // Initialize the in-memory pointers
        auto s = new (buf_.bytes) Header();
        cur_entry_ = r_cast< Entry* >(buf_.bytes + sizeof(Header));
    }

    bool has_room() const { return (available_space_ > sizeof(Entry)); }

    bool is_empty() const { return (header()->size == sizeof(Header)); }

    void add_entry(CompactNodeId n, CompactBlkId b) {
        HS_REL_ASSERT_EQ(has_room(), true, "Calling add_entry without any room");
        if (cur_entry_->merge_if_possible(n, b)) {
            header()->size += sizeof(CompactBlkId);
            available_space_ -= sizeof(CompactBlkId);
        } else {
            ++(header()->n_entries);
            cur_entry_ = r_cast< Entry* >(uintptr_cast(cur_entry_) + cur_entry_->size());
            cur_entry_->merge_if_possible(n, b);

            available_space_ -= Entry::size(1u);
            header()->size += Entry::size(1u);
        }
    }

    void link(BNodeMapWriteUnit& next) { header()->next_unit_location = CompactBlkId{next->location_}; }

    void finialize() {
        ++(header()->n_entries); // We increment as the last entry would be open until we finalize

        // Trim down the alloc size and actual blks (if we alloced them)
        auto const occupied_blks = location_.blk_count() - (available_space_ / vdev_->block_size());
        auto const [valid, freeable] = location_.split(occupied_blks);
        vdev_->free_blks(freeable);
        location_ = valid;
        available_space_ = 0;

        // Write the checksum
        auto s = r_cast< Serialized* >(buf_.bytes);
        auto const crc = crc32_ieee(init_crc32, s_cast< const uint8_t* >(header()) + sizeof(Header),
                                    header()->size - sizeof(Header));
        s->checksum = crc;
    }

private:
    Header* header() { return r_cast< Header* >(buf_.bytes); }
};

struct BNodeMapWriteUnit {
private:
#pragma pack(1)
    struct Serialized {
        static constexpr const uint32_t expected_nodes_packed_per_entry = 64;

        struct Entry {
            CompactNodeId nodeid_start;
            uint16_t nodes_count{0};
            CompactBlkId node_locations[1];

            static size_t size(uint32_t count) { return sizeof(Entry) + (count ? count * sizeof(CompactBlkId) : 0); }
            size_t size() const { return size(nodes_count); }

            bool merge_if_possible(compact_node_id n, btree_blk_id b) {
                if ((nodes_count == 0) || ((nodeid_start + nodes_count) == n)) {
                    node_locations[nodes_count] = CompactBlkId{b};
                    ++nodes_count;
                    return true;
                }
                return false;
            }
        };

        // Guess the size expecting 64 nodes packed together.
        static uint32_t size_guess(uint32_t num_nodes) const {
            return Entry::size(num_nodes / expected_nodes_packed_per_entry);
        }

        static uint32_t header_size() { return sizeof(Serialized) - sizeof(Entry); }

        CompactBlkId next_unit_location; // Location of where the next meta (for map) is present
        uint16_t n_unit_blks;            // Total number of blocks in this unit
        uint32_t n_entries{0};           // Total number of entries in this unit
        uint32_t checksum{0};            // Checksum excluding this header
        Entry entries[1];                // Contiguous open number of entries, each entry contains node info
    };
#pragma pack()

private:
    sisl::io_blob_safe buf_;
    shared< VirtualDev > vdev_;
    uint32_t available_space_{0};
    BlkId location_;
    bool pre_alloced_{true};
    Serialized::Entry* cur_entry_{nullptr};

    static constexpr uint32_t const min_blks_per_write_unit = 128;

    BNodeMapWriteUnit(shared< VirtualDev > vdev, uint32_t nodes_count) : vdev_{std::move(vdev)}, pre_alloced_{false} {
        available_space_ = sisl::round_up(Serialized::size_guess(nodes_count), vdev_->block_size());
        auto const reqd_blks = (available_space_ - 1) / vdev_->block_size() + 1;

        // First allocate the blks and adjust the available space to how much ever we were able to allocate
        // contiguously.
        blk_alloc_hints hints = {.partial_alloc_ok = true,
                                 .min_blks_per_piece = std::min(reqd_blks, min_blks_per_write_unit)};
        BlkAllocStatus status = vdev_->alloc_contiguous_blks(available_space_, hints, location_);
        HS_REL_ASSERT_EQ(
            status, BlkAllocStatus::SUCCESS,
            "No space to write the bnode map, which cannot be proceeded further, crashing the system for now");
        available_space_ = location_.blk_count() * vdev_->block_size();

        // Now allocate this space and initialize the serialized structure
        buf_ = sisl::io_blob_safe(available_space_, vdev_->align_size(), sisl::buftag::metablk);
        memset(buf_.bytes, 0, available_space_);
        auto s = new (buf_.bytes) Serialized();
        cur_entry_ = &s->entries[0];
    }

    BNodeMapWriteUnit(shared< VirtualDev > vdev, BlkId const& loc) :
            vdev_{std::move(vdev)}, location_{loc}, pre_alloced_{true} {
        available_space_ = location_.blk_count() * vdev_->block_size();
        buf_ = sisl::io_blob_safe(available_space_, vdev_->align_size(), sisl::buftag::metablk);
        memset(buf_.bytes, 0, available_space_);
        auto s = new (buf_.bytes) Serialized();
        cur_entry_ = &s->entries[0];
    }

    bool has_room() const { return (available_space_ > sizeof(Serialized::Entry)); }

    void add_entry(CompactNodeId n, CompactBlkId b) {
        HS_REL_ASSERT_EQ(has_room(), true, "Calling add_entry without any room");
        if (cur_entry_->merge_if_possible(n, b)) {
            available_space_ -= sizeof(CompactBlkId);
        } else {
            auto s = r_cast< Serialized* >(buf_.bytes);
            ++s->n_entries;
            available_space_ -= Serialized::Entry::size(1u);
            cur_entry_ = r_cast< Serialized::Entry >(uintptr_cast(cur_entry_) + cur_entry_->size());
            cur_entry_->merge_if_possible(n, b);
        }
    }

    void link(BNodeMapWriteUnit& next) {
        r_cast< Serialized* >(buf_)->next_unit_location = CompactBlkId{next->location_};
    }

    void finialize() {
        if (!pre_alloced_) {
            // Trim down the alloc size and actual blks (if we alloced them)
            auto const occupied_blks = location_.blk_count() - (available_space_ / vdev_->block_size());
            auto const [valid, freeable] = location_.split(occupied_blks);
            vdev_->free_blks(freeable);
            location_ = valid;
            available_space_ = 0;
        }

        // Write the checksum
        auto s = r_cast< Serialized* >(buf_.bytes);
        auto const crc = crc32_ieee(init_crc32, s_cast< const uint8_t* >(s) + Serialized::header_size(),
                                    alloced_size() - Serialized::header_size());
        s->checksum = crc;
    }

    uint32_t alloced_size() const { return location_.blk_count() * vdev_->block_size(); }
};

struct Flusher {
    COWBtreeCPContext* cp_ctx_;
    unique< Journal > journal_; // Journal for flushing the maps. Valid only if we are doing incr map flush

    // Dirty node flush section. FlushUnit related
    unique< NodeFlushUnit > nfunit_;
    DirtyList& dirty_list_;
    sisl::ConcurrentInsertVector< BtreeNodePtr >::iterator dirty_it_;
    uint32_t remaining_modified_;

    // Deleted node info flush section. DeleteUnit related
    sisl::ConcurrentInsertVector< BtreeNodePtr >::iterator deleted_it_;
    uint32_t deleted_count_{0};

    // Full Map writing related sections. Will be invoked only if we are doing full map flush in CP
    COWBtree::FullBNodeIdMap& full_map_;
    std::map< CompactNodeId, CompactBlkId >::iterator full_map_it_;
    bool full_map_it_valid_{false};
    uint32_t remaining_entries_{0};
    std::vector< CompactBlkId > prev_map_locations_;
    unique< BNodeMapWriteUnit > mwunit_;
    bool sb_persist_needed{false};

    Flusher(COWBtree* cbtree, COWBtreeCPContext* cp_ctx) :
            cp_ctx_{cp_ctx}, dirty_list_{cbtree->dirty_list(cp_ctx)}, full_map_{cbtree->m_bnodeid_map} {
        dirty_it_ = dirty_list_.modified().begin();
        remaining_modified_ = dirty_list_.size();
        deleted_it_ = dirty_list_.deleted().begin();

        // We need to create journal for every incremental flush
        if (cp_ctx->full_bnode_map_flush()) {
            mwunit_ = std::make_unique< BNodeMapWriteUnit >(cbtree->m_vdev, full_map_.map_.size());
        } else {
            journal_ = std::make_unique< COWBtree::Journal >(cbtree->m_btree_ordinal, 1 * 1024 * 1024);
        }

        nfunit_ = std::make_unique< NodeFlushUnit >(cbtree->m_vdev.get(), cp_ctx, journal_.get(), remaining_modified_);
    }

    ~Flusher() {}

    void build_node_flush_units(auto&& cb) {
        while (dirty_it_ != dirty_list_.end()) {
            BtreeNodePtr node = *dirty_it_;
            ++dirty_it_;
            --remaining_modified_;

            // Its possible node was dirtied, but subsequently delete, in that case no need to flush them
            if (node->is_deleted()) { continue; }

            // Add the current node to the flush unit, it should tell us it has more room for us to add more nodes
            auto const [filled, node_loc] = nfunit_->add(to_cow_btree_node(node));

            // Do a callback for each node inside flush unit
            cb(node, node_loc, *nfunit_, filled);

            if (filled || (remaining_modified_ == 0)) {
                if (journal_) {
                    journal_->header()->size += nfunit_->jentry_->size;
                    ++(journal_->header()->num_flush_units);
                }

                nfunit_->realloc(remaining_modified_);
            }
        }
        HS_DBG_ASSERT_EQ(remaining_modified_, 0,
                         "Remaining count to flush is non zero, but dirty node iterator has come to an end");
    }

    void build_delete_units(auto&& cb) {
        CompactNodeId* delete_journal_entries{nullptr};
        if (journal_) {
            delete_journal_entries =
                r_cast< CompactNodeId* >(journal_->make_room(delete_list_.size() * sizeof(CompactNodeId)));
        }

        while (delete_it_ != delete_list_.end()) {
            auto nodeid = *delete_it_;
            cb(nodeid);
            if (journal_) { delete_journal_entries[deleted_count_++] = nodeid; }
            ++delete_it;
        }
        if (journal_) {
            journal_->header()->size += (deleted_count * sizeof(CompactNodeId));
            journal_->header()->num_delete_units = deleted_count;
        }
    }

    void build_map_write_units(auto&& cb) {
        if (!full_map_it_valid_) {
            full_map_it_ = full_map_.begin();
            remaining_entries_ = full_map_.size();

            // Store the previous blkids/location where chain of full maps are stored. Required to free these blks once
            // full map has been written to the new location
            prev_map_locations_ = std::move(full_map_.chain_locations_);
            full_map_it_valid_ = true;
        }

        while (full_map_it_ != full_map_.end()) {
            if (!mwunit_->has_room()) {
                auto new_unit = std::make_unique< BNodeMapWriteUnit >(m_dev, remaining_entries_);
                mwunit_->link(*new_unit);
                mwunit_->finalize();
                full_map_.chain_locations_.emplace_back(mwunit_->location_);
                cb(*mwunit_);

                mwunit_ = std::move(new_unit);
            }
            mwunit_->add_entry(full_map_it_->first, full_map_it_->second);
            ++full_map_it_;
            --remaining_entries_;
        }

        if (mwunit_->buf.size != 0) {
            full_map_.chain_locations_.emplace_back(mwunit->location_);
            cb(*mwunit_);
        }

        for (auto const& loc : prev_map_locations_) {
            m_vdev->free_blks(loc);
        }
    }
};

bool COWBtree::flush_nodes(COWBtreeCPContext* cp_ctx) {
    CPSession* session = cp_session(cp_ctx->cp_id());
    if (!session->prepare_to_flush_nodes(*this, cp_ctx)) {
        // Already flushed the cp and moved on.
        return false;
    }

    // 5 steps on per btree CP flush
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
            update_bnode_map(get_compact_nodeid(to_cow_btree_node(node)), CompactBlkId{location, i});
        }

        auto err = m_vdev->sync_writev(nfunit.iovs_.data(), nfunit.iovs_.size(), nfunit.location_);
        HS_REL_ASSERT(!err, "Flush of nodes failed during cp, best is to crash the system and retry on reboot");
    } while (true);

    //
    // Step 2: During cp io phase, all deleted nodes are tracked, we delete them from in-memory map and also
    // build the journal with this delete operation.
    //
    auto [it, end_it, journal_area] = session->next_deleted();
    uint8_t* delete_jentries = r_cast< CompactNodeId* >(journal_area.bytes_);
    uint32_t deleted_count = 0;
    while (it != end_it) {
        auto nodeid = *it;
        delete_from_bnode_map(nodeid);
        if (delete_jentries) { delete_jentries[deleted_count_++] = nodeid; }
        ++it;
    }

    //
    // Step 3: Check if there are any changes that needs changes in superblock of the btree. If so modify them. We do
    // not persist them yet, because we need all fibers to finish flushing, write journal etc before committing to
    // superblock to disk. In this step, we just updated the superblk based on what has been dirtied earlier.
    //
    auto [new_root, new_node_counts] = session->next_sb_updates();
    if (new_root != empty_bnodeid) { bt->mutable_super_blk()->btree_sb.root_node = new_root; }
    if (new_node_counts != 0) {
        bt->mutable_super_blk()->btree_sb.index_size += (vdev->block_size() * new_node_counts);
    }

    if (session->done_flushing_nodes()) {
        CP_PERIODIC_LOG(DEBUG, cp_ctx->id(), "Btree={} has flushed {} dirty_nodes and deleted {} nodes", ordinal(),
                        session->modified_node_count(), session->deleted_node_count());
    }
    return true;
}

void COWBtree::flush_map_and_sb(COWBtreeCPContext* cp_ctx) {
    HS_DBG_ASSERT(cp_ctx->need_full_map_flush(), "Flush map called on a cp which doesn't need full map flush");

    CPSession* session = cp_session(cp_ctx->cp_id());
    auto const [it, count] = session->prepare_to_flush_map(*this, cp_ctx);

    std::vector< BlkId > full_map_locations;
    // Worst Estimate of 1 entry per count packed in a single blk
    full_map_locations.reserve((Entry::size(1) * count) / m_vdev->block_size());

    while (count > 0) {
        BNodeMapWriteUnit munit = std::make_unique< BNodeMapWriteUnit >(m_dev.get(), count);
        if (!munit->has_room()) {
            auto new_unit = std::make_unique< BNodeMapWriteUnit >(m_dev, count);
            munit->link(*new_unit);
            munit->finalize();
            full_map_locations.emplace_back(mwunit->location_);
            auto err = m_vdev->sync_write(mwunit->buf_.bytes, mwunit->buf_.size, mwunit->location_);
            HS_REL_ASSERT(!err, "Flush of full map failed with err={}. best is to crash the system and replay", err);

            munit = std::move(new_unit);
        }
        munit->add_entry(it->first, it->second);
        ++it;
        --count;
    }

    if (!munit->is_empty()) {
        munit->finalize();
        full_map_locations.emplace_back(mwunit->location_);
        auto err = m_vdev->sync_write(mwunit->buf_.bytes, mwunit->buf_.size, mwunit->location_);
        HS_REL_ASSERT(!err, "Flush of full map failed with err={}. best is to crash the system and replay", err);
    }

    auto const [done, loc_array_list] = session->done_flushing_map(std::move(full_map_locations));
    if (!done) { return; }

    // Full map has been written, now we write the multiple chains in an indirect block into the super block
    BlkId cur_indirect_location = m_base_btree->mutable_super_blk()->btree_sb.u.cow_sb.full_map_location;

    m_base_btree->mutable_super_blk()->btree_sb.u.cow_sb.full_map_location =
        write_indirect_location_blk(loc_array_list);

    // Persist the superblk now
    flush_sb(cp_ctx);

    // We have completed the flush of map and now we can free up the old location blks.
    auto& map_locations = m_bnodeid_map.map_locations_;
    for (auto const& loc : map_locations) {
        m_vdev->free_blks(loc);
    }
    m_vdev->free_blks(cur_indirect_location);

    // We need to update the current map_locations with this new set.
    map_locations.clear();
    for (auto const& loc_array : loc_array_list) {
        map_locations.insert(map_locations.end(), loc_array.begin(), loc_array.end());
    }
}

void COWBtree::flush_sb(COWBtreeCPContext* cp_ctx) {
    CPSession* session = cp_session(cp_ctx->cp_id());
    session->flush_sb(cp_ctx);
}

BlkId COWBtree::write_indirect_location_blk(std::vector< std::vector< BlkId > > const& loc_array_list) {
    auto indirect_map_location = alloc_blks_or_fail(m_vdev.get(), m_vdev->block_size(), blk_alloc_hints{});
    sisl::io_blob_safe buf{m_vdev->block_size(), m_vdev->align_size(), sisl::buftag::meta};
    memset(buf.bytes_, 0, buf.size_);

    auto sb = r_cast< FullBNodeIdMap::IndirectLocationSB* >(buf.bytes_);
    for (auto const& loc_array : loc_array_list) {
        locations_head[sb->num_locations_head++] = loc_array[0];
    }
    sb->checksum =
        crc32_ieee(init_crc32, s_cast< const uint8_t* >(buf.bytes_ + FullBNodeIdMap::IndirectLocationSB::header_size()),
                   (buf.size_ - FullBNodeIdMap::IndirectLocationSB::header_size()));
    auto err = m_vdev->sync_write(buf.bytes_, buf.size_, indirect_map_location);
    HS_REL_ASSERT(!err, "Flush of full map failed with err={}. best is to crash the system and replay", err);
    return indirect_map_location;
}

sisl::io_blob_safe COWBtree::cp_flush(COWBtreeCPContext* cp_ctx) {
    m_flush_mtx.lock();
    CPSession* cp_session = current_cp_session(cp_ctx->cp_id());
    if (cp_session == nullptr) {
        // Already flushed the cp and moved on.
        m_flush_mtx.unlock();
        return sisl::io_blob_safe{};
    }

    // 5 steps on per btree CP flush
    //
    // Step 1: Flush all the nodes by building flush units (with each unit consists of 1 contiguous blk worth) and
    // while doing so, keep updating the in-memory map as well as adding to incremental journal with the map
    // updates.
    //
    // Step 2: During cp io phase, all deleted nodes are tracked, we delete them from in-memory map now and also
    // build the journal with this delete operation.
    //
    // Step 3: If it incremental map only session, this method only has to be provide the journal to btreestore
    // layer and done. It is the store layer which collects journal from all the btree and write collected journal
    // at the end.
    //
    // Step 4: If it is full map flush cp, then build the map write units (with each unit consits of map entries
    // that can accomodate in 1 contiguous blk worth of space). All map write units are written on new locations, so
    // this step also frees up old location where previous maps were written.
    //
    // Step 5: Map blks are written as chain and thus first blk has to be persisted in the superblock. This step
    // does that.
    cp_session->flusher()->build_node_flush_units(
        [this](BtreeNodePtr const& node, CompactBlkId node_loc, NodeFlushUnit& nfunit, bool do_write_now) {
            // Keep updating the full inmemory map of nodeid and blkid.
            // IMPORTANT NODE: We do that before actually writing the data. It is ok to do so, under the assumption that
            // there will be no reads into the bnode map while this is being flushed because nodes are cached until
            // flush is completed. If for any reason we need to support skipping cache, then we should update this bnode
            // map after it has been written. We are doing this here as an optimization to avoid looping for every node
            // and then update.
            update_bnode_map(get_compact_nodeid(to_cow_btree_node(node)), node_loc);

            if (do_write_now) {       // Unit is completely filled for alloced blks, flush them
                m_flush_mtx.unlock(); // Release the lock for other fibers to work on while we persiste them

                auto err = m_vdev->sync_writev(nfunit.iovs_.data(), nfunit.iovs_.size(), nfunit.location_);
                HS_REL_ASSERT(!err, "Flush of nodes failed during cp, best is to crash the system and retry on reboot");

                m_flush_mtx.lock();
            }
        });

    cp_session->flusher()->build_delete_units([this](CompactNodeId nodeid) { delete_from_bnode_map(nodeid); });

    auto cur_root = cp_session->new_root().exchange(empty_bnodeid);
    auto cur_node_counts = cp_session()->count_changes().exchange(0);

    if (!cp_ctx->full_bnode_map_flush()) {
        auto ret_buf = std::move(cp_session->flusher()->journal_.base_buf_);
        m_flush_mtx.unlock();
        return ret_buf;
    }

    cp_session->flusher()->build_map_write_units([this](BNodeMapWriteUnit& mwunit) {
        m_flush_mtx.unlock();
        auto err = m_vdev->sync_write(mwunit.buf_.bytes, mwunit.buf_.size, mwunit.location_, false /* part_of_batch*/);
        HS_REL_ASSERT(!err, "Flush of full map failed with err={}. best is to crash the system and replay", err);
        m_flush_mtx.lock();
    });

    if (cp_session->flusher()->sb_persist_needed) {
        auto& sb = bt->mutable_super_blk();
        sb->btree_sb.u.cow_sb.full_map_location = m_bnodeid_map.chain_locations_[0];
        sb.write();
        m_flusher->sb_persist_needed = false;
    }
    m_flush_mtx.unlock();

    return sisl::io_blob_safe{}; // No incremental journal needed
}

void COWBtree::recover_bnode_map(BlkId const& map_loc) {
    BT_LOG(INFO, "Recovering NodeID to blkid from location=[{}]", map_loc);

    BlkId next_loc = map_loc;
    do {
        auto [ec, buf] = m_vdev->alloc_buf_and_read(next_loc);
        HS_REL_ASSERT(!ec, "Error while reading bnodeid map, cannot proceed further");

        m_vdev->commit_blk(next_loc);
        m_bnodeid_map.chain_locations_.push_back(next_loc);

        auto s = r_cast< BNodeMapWriteUnit::Serialized* >(buf->bytes);
        auto ptr = uintptr_cast(&s->entries[0]);

        for (uint32_t i{0}; i < s->n_entries; ++i) {
            auto e = r_cast< BNodeMapWriteUnit::Serialized::Entry* >(ptr);
            for (uint32_t n{0}; n < e->nodes_count; ++n) {
                CompactBlkId cb = CompactBlkId{e->node_locations[n].blk_num, 1, e->node_locations[n].chunk_num};
                m_bnodeid_map.map_.insert(e->nodeid_start + n, cb);
                m_nodeid_generator.reserve(e->nodeid_start + n);
                m_vdev->commit_blk(BlkId{cb.blk_num, cb.chunk_num});
            }
            ptr += e->size();
        }
        next_loc = s->next_unit_location.to_blkid();
    } while (next_loc.is_valid());
}

void COWBtree::apply_incremental_map(sisl::byte_view const& journal_buf) {
    auto jhdr = r_cast< Journal::Header* >(journal_buf.bytes());
    HS_REL_ASSERT_EQ(jhdr->ordinal, m_btree_ordinal, "Btree Ordinal mismatch between journal and in-memory");

    uint8_t* cur_ptr = jhdr + sizeof(Journal::Header);
    for (uint32_t i{0}; i < jhdr->num_flush_units; ++i) {
        NodeFlushUnit::JournalEntry* nfunit = r_cast< NodeFlushUnit::JournalEntry* >(cur_ptr);
        for (uint16_t n{0}; n < nfunit->n_nodes; ++n) {
            update_bnode_map(nodes[i] + n, CompactBlkId{nodes_location, n});
        }
        cur_ptr += NodeFlushUnit::journal_entry_size(nfunit->n_nodes);
    }

    auto* deleted_nodes = r_cast< CompactNodeId* >(cur_ptr);
    for (uint32_t i{0}; i < jhdr->num_delete_units; ++i) {
        delete_from_bnode_map(deleted_nodes[i]);
    }
}

////////////////////////////////////////////// CPSession Section //////////////////////////////////////////////
bool COWBtree::CPSession::prepare_to_flush_nodes(COWBtreeCPContext* cp_ctx) {
    std::lock_guard lg{flush_mtx_};

    if (state_ == FlushState::NODES_FLUSHED) {
        return false; // Bail out if we have already flushed this session
    } else if (state_ == FlushState::NODES_FLUSHING) {
        ++flushing_req_count_;
        return true; // Everything is prepared already, join the flush
    }

    modified_count_ = modified_nodes_.size();
    deleted_count_ = deleted_nodes_.size();

    if ((modified_count_ == 0) & (deleted_count_ == 0)) {
        // Nothing has been dirtied in this btree in this session to flush.
        state_ = FlushState::NODES_FLUSHED;
        return false;
    }

    auto const status = bt_.m_vdev->alloc_blks(
        mod_node_count, blk_alloc_hints{.min_blks_per_piece = std::min(mod_node_count, min_blks_per_write_unit)},
        node_locations_);
    if ((status != BlkAllocStatus::SUCCESS) || (status != BlkAllocStatus::PARTIAL)) {
        HS_REL_ASSERT(false, "Blk allocation to persist btree pages failed, we are crashing for now");
    }

    // Setup all the iterators
    next_location_idx_ = 0;
    modified_it_ = modified_nodes_.begin();
    deleted_it_ = deleted_nodes_.begin();

    if (!cp_ctx->full_bnode_map_flush()) {
        // Setup the journal buffers
        // Size deterimination:
        // One location which is a blkid corresponds to 1 flush unit, so total journal size would be
        // Journal Header + (Number of flush units * Flush unit header) + Number of nodes + Number of deleted nodes
        auto const journal_size = sizeof(Journal::Header) +
            (NodeFlushUnit::Entry::journal_entry_size(0) * node_locations_.size()) +
            ((node_count + deleted_count_) * sizeof(CompactNodeId));
        journal_ = std::make_unique< Journal >(bt_.ordinal(), journal_size);
        journal_->header()->num_flush_units = mod_node_count;
        journal_->header()->num_delete_units = deleted_count_;
    }

    state_ = FlushState::NODES_FLUSHING;
    ++flushing_req_count_;
    return true;
}

bool COWBtree::CPSession::done_flushing_nodes() {
    std::lock_guard lg{flush_mtx_};
    HS_DBG_ASSERT_EQ(state_, FlushState::NODES_FLUSHING,
                     "Received a flush done while state was not in flushing, some race condition?");
    if (--flushing_req_count_ == 0) {
        state_ = FlushState::NODES_FLUSHED;
        return true;
    }
    return false;
}

std::tuple< BlkId, DirtyNodeList::iterator, sisl::blob > COWBtree::CPSession::next_dirty() {
    std::lock_guard lg{flush_mtx_};
    HS_DBG_ASSERT_EQ(state_, FlushState::FLUSHING,
                     "Unexpected state while pulling a dirty nodes, we expect all fibers have drained the iterator "
                     "before moving to flushed or collecting state");

    sisl::blob ret_blob;
    if (next_location_idx_ == node_locations_.size()) {
        // We have reached the end of all nodes location's, which means there should be no more dirty
        HS_DBG_ASSERT(modified_it_ == modified_nodes_.end(),
                      "Mismatch between number of blks allocated for node and dirty node iterator");
        return std::make_tuple(BlkId{}, modified_it_, ret_blob);
    } else {
        HS_DBG_ASSERT(
            modified_it_ != modified_nodes_.end(),
            "There are more blks allocated for nodes, but the dirty list doesn't have anymore node to fill it in");
        return std::make_tuple(BlkId{}, modified_it_, ret_blob);
    }

    BlkId ret_loc = node_locations_[next_location_idx_++];
    auto ret_it = modified_it_;
    modified_it_ += ret_loc.blk_count(); // Move the iterator past the blk_count().

    if (journal_) {
        auto junit_size = NodeFlushUnit::Entry::journal_entry_size(ret_loc.blk_count());
        ret_blob = sisl::blob{journal_->allocate(junit_size), junit_size};
    }
    return std::make_tuple(ret_loc, ret_it, ret_blob);
}

std::tuple< DeletedNodeList::iterator, uint32_t, sisl::blob > COWBtree::CPSession::next_deleted() {
    std::lock_guard lg{flush_mtx_};
    HS_DBG_ASSERT_EQ(state_, FlushState::FLUSHING,
                     "Unexpected state while pulling a dirty nodes, we expect all fibers have drained the iterator "
                     "before moving to flushed or collecting state");
    auto ret_it = deleted_it_;
    deleted_it_ = deleted_nodes_.end(); // Set to end, so that any subsequent requests will get ret_it as end iterator

    sisl::blob ret_blob;
    if (journal_) {
        auto const jdel_size = deleted_count_ * sizeof(CompactNodeId);
        ret_blob = sisl::blob{journal_->allocate(jdel_size), jdel_size};
    }
    return std::make_tuple(std::move(ret_it), deleted_nodes_.end(), std::move(ret_blob));
}

std::pair< bnodeid_t, int64_t > COWBtree::CPSession::next_sb_updates() {
    std::lock_guard lg{flush_mtx_};
    auto new_root = session->new_root_id.exchange(empty_bnodeid);
    if (new_root != empty_bnodeid) { sb_persist_needed_ = true; }

    auto new_count = node_count_changes.exchange(0));
    if (new_count != 0) { sb_persist_needed_ = true; }

    return std::pair(new_root, new_count);
}

std::pair< BNodeIDMap::iterator, uint32_t > COWBtree::CPSession::prepare_to_flush_map(COWBtreeCPContext* cp_ctx) {
    std::lock_guard lg{flush_mtx_};
    if (state_ == FlushState::MAP_FLUSHING) {
        // Some other fiber has started the flushing, get the next range of maps and iterate over and start flushing
        ++flushing_req_count_;
        auto ret_it = next_full_map_it_;
        next_full_map_it_ += parallel_flush_range_;
        return std::pair(ret_it, parallel_flush_range_);
    } else if (state_ != FlushState::NODE_FLUSHED) {
        // The nodes themselves have not been flushed or we have already finished flushing map, so we don't need to
        // anything now
        return (std::pair(next_full_map_it_, 0));
    } else {
        // First fiber to start flushing, prepare the iterator. First flusher wll also get reminder of range also
        HS_DBG_ASSERT_EQ(flushing_req_count_, 0, "In NODE_FLUSHED state, but outstanding count is non zero");
        auto& m = bt_.m_bnodeid_map.map_;

        // First fiber to flush the full map in this session. All fibers get equal portion to flush except the first one
        // which gets additional
        parellel_flush_range_ = m.size() / cp_ctx->parallel_flushers_count();
        auto this_count = parallel_flush_range_ + (m.size() % cp_ctx->parallel_flushers_count());
        next_full_map_it_ = m.begin() + this_count;

        state_ = FlushState::MAP_FLUSHING;
        return (m.begin(), this_count);
    }
}

std::pair< bool, std::vector< std::vector< BlkId > > >
COWBtree::CPSession::done_flushing_map(std::vector< BlkId > map_locations) {
    std::lock_guard lg{flush_mtx_};
    HS_DBG_ASSERT_EQ(state_, FlushState::MAP_FLUSHING,
                     "Received a flush done while state was not in flushing, some race condition?");

    loc_array_list_.emplace_back(std::move(map_locations));
    if (--flushing_req_count_ != 0) { return std::pair(false, {}); }
    state_ = FlushState::MAP_FLUSHED;
    return std::pair(true, std::move(loc_array_list_));
}

bool COWBtree::CPSession::flush_sb(COWBtreeCPContext* cp_ctx) {
    std::lock_guard lg{flush_mtx_};
    if (cp_ctx->need_full_map_flush() || sb_persist_needed_) {
        bt.mutable_superblk().write();
        sb_persist_needed_ = false;
    }
    state_ = FlushState::ALL_DONE;
}
} // namespace homestore