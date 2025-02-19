#include "index/cow_btree.h"

namespace homestore {
static constexpr uint64_t btree_nodeid_bits = sizeof(uint16_t) * 8;
static constexpr uint64_t btree_ordinal_bits = 64 - btree_nodeid_bits;
static constexpr uint64_t btree_nodeid_mask = ((1ull << btree_nodeid_bits) - 1);
static constexpr uint64_t btree_ordinal_mask = ((1ull << btree_ordinal_bits) - 1) << btree_nodeid_bits;

static constexpr uint32_t initial_bnodeid_map_persistent_size = 512 * 1024;

COWBtree::COWBtree(BtreeBase* bt, shared< VirtualDev > vdev, superblk< index_table_sb >&& sb) :
        m_base_btree{bt},
        m_bt_cfg{bt->cfg},
        m_nodeid_generator(std::numeric_limits< uint32_t >::max()),
        m_sb{std::move(sb)},
        m_vdev{std::move(vdev)},
        m_btree_ordinal{m_sb->btree_sb.ordinal},
        m_ordinal_shifted{m_btree_ordinal << btree_nodeid_bits} {
    if (m_sb->btree_sb.full_map_location.is_valid()) { recover_full_map(); }
}

bnodeid_t COWBtree::gen_node_id() { return (m_ordinal_shifted | m_nodeid_generator.reserve()); }

void COWBtree::update_bnode_map(bnodeid_t nodeid, BlkId const& blkid, cp_id_t cpid) {
    HS_DBG_ASSERT_EQ(nodeid & btree_ordinal_mask, m_base_ordinal, "Ordinal number on btree and node is different");

    std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lg(m_bnodeid_map.mtx_);
    auto [it, happened] = m_bnode_map.map_.insert_or_assign(nodeid & btree_nodeid_mask, btree_blk_id(blkid, cpid));
    if (!happened) { HS_LOG_ASSERT(!happened, "Updating node_id {} to bnode map failed", nodeid); }
}

void COWBtree::add_to_dirty_list(BtreeNodePtr const& node, COWBtreeCPContext* cp_ctx) {
    m_dirty_list[cp_ctx->cp_id % MAX_CONCURRENT_CPS].insert(node);
    cp_ctx->dirty_buf_count.increment(1);
}

void COWBtree::async_cp_flush(COWBtreeCPContext* cp_ctx) {
    cp_ctx->dirty_nodes_it = m_dirty_list[cp_ctx->cp_id % MAX_CONCURRENT_CPS].begin();
}

struct BNodeMapWriteUnit {
private:
#pragma pack(1)
    struct Serialized {
        static constexpr const uint32_t expected_nodes_packed_per_entry = 64;

        struct Entry {
            compact_nodeid_t nodeid_start;
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

    void add_entry(compact_nodeid_t n, btree_blk_id b) {
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

folly::Future< folly::Unit > COWBtree::persist_bnode_map() {
    std::vector< folly::Future< std::error_condition > > iovs;
    size_t remaining_nodes = m_bnodeid_map.map_.size();

    std::shared_lock< iomgr::FiberManagerLib::shared_mutex >(m_bnodeid_map.mtx_);
    auto it = m_bnode_map.begin();

    // Move the current bnodeid_map_locations to full_bnodeid_map
    m_bnodeid_map.freeable_locations_ = std::move(m_bnodeid_map.chain_locations_);

    unique< BNodeMapWriteUnit > unit = std::make_unique< BNodeMapWriteUnit >(m_dev, remaining_nodes);
    for (auto const& [n, b] : m_bnode_map) {
        if (!unit->has_room()) {
            auto new_unit = std::make_unique< BNodeMapWriteUnit >(m_dev, remaining_nodes);
            unit->link(*new_unit);
            unit->finalize();
            m_bnodeid_map.chain_locations_.emplace_back(unit->location_);
            futs.emplace_back(
                m_vdev->async_write(unit->buf_.bytes, unit->buf_.size, unit->location_, false /* part_of_batch*/)
                    .thenValue([unit = std::move(unit)] {}));
            unit = std::move(new_unit);
        }

        unit->add_entry(n, b);
        --remaining_nodes;
    }

    if (unit->buf.size != 0) {
        m_bnodeid_map.chain_locations_.emplace_back(unit->location_);
        futs.emplace_back(m_vdev->async_write(unit->buf.bytes, unit->buf.size, unit->location, false /* part_of_batch*/)
                              .thenValue([unit = std::move(unit)](auto) {}));
    }

    return folly::collectAllUnsafe(futs).thenValue([this](auto&& e) {
        for (auto const& loc : m_bnodeid_map.freeable_locations_) {
            m_vdev->free_blks(loc);
        }
        m_bnodeid_map.freeable_locations_.clear();
        return folly::makeFuture< folly::Unit >(folly::Unit{});
    });
}

void COWBtree::recover_full_map() {
    BT_LOG(INFO, "Recovering NodeID to blkid from location=[{}]", m_sb->btree_sb.full_map_location);

    BlkId next_loc = m_sb->btree_sb.full_map_location;
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
} // namespace homestore