#pragma once

#include <memory>

#include <homestore/btree/cow_btree_store.h>

namespace homestore {
COWBtreeNode* to_cow_btree_node(BtreeNodePtr const& n) {
    return r_cast< COWBtreeNode* >(uintptr_cast(n.get()) - sizeof(COWBtreeNode));
}

void cow_btree_node_constructor(BtreeNodePtr const& n) {
    new (uintptr_cast(n.get()) - sizeof(COWBtreeNode)) COWBtreeNode();
}

void cow_btree_node_destructor(BtreeNode* n) {
    r_cast< COWBtreeNode* >(uintptr_cast(n) - sizeof(COWBtreeNode))->~COWBtreeNode();
}

COWBtreeStore::COWBtreeStore(shared< VirtualDev > vdev, std::pair< meta_blk*, sisl::byte_view > sb,
                             shared_ptr< sisl::Evictor > evictor, uint32_t node_size) :
        m_vdev{std::move(vdev)},
        m_cache{std::move(evictor), 100000, node_size,
                [](const BtreeNodePtr& node) -> bnodeid_t { return node->node_id(); },
                [](const sisl::CacheRecord& rec) -> bool {
                    const auto& hnode = (sisl::SingleEntryHashNode< BtreeNodePtr >&)rec;
                    return (hnode.m_value->m_refcount.test_le(1));
                }},
        m_node_size{node_size},
        m_bnode_map_base_mblk{sb.first},
        m_vdev_blks_per_node{node_size / m_vdev->block_size()},
        m_max_nodes_per_flush{
            ((HS_DYNAMIC_CONFIG(btree->max_btree_write_size_per_io) - 1) / node_size) + 1,
        } {}

BtreeNodePtr COWBtreeStore::create_node(BtreeBase& btree, bool is_leaf) override {
    auto buf = hs_utils::iobuf_alloc(m_node_size, sisl::buftag::btree_node, m_vdev->align_size());
    auto n = BtreeNodePtr{init_node(buf, generate_node_id(), true /* init_buf */, is_leaf, sizeof(COWBtreeNode))};
    new (uintptr_cast(n.get()) - sizeof(COWBtreeNode)) COWBtreeNode();

    // Add the node to the cache
    bool done = m_cache.insert(n);
    HS_REL_ASSERT_EQ(done, true, "Unable to add alloc'd node to cache, low memory or duplicate inserts?");

    return n;
}

btree_status_t COWBtreeStore::write_node(BtreeBase&, BtreeNodePtr const& node, void* context) {
    // All the required actions are performed during refresh_node with read_modify_write=true
    return btree_status_t::success;
}

btree_status_t COWBtreeStore::read_node(BtreeBase& btree, bnodeid_t node_id, BtreeNodePtr& node) const override {
retry:
    // Attempt to locate the node in the cache
    if (m_cache.get(node_id, node)) { return btree_status_success; }

    // Need to read from the blk, so check that in the map
    auto const it = m_bnode_map.find(node_id);
    if (it == m_bnode_map.cend()) {
        LOGERRORMOD(index, "Unable to locate node_id={} in the map, has the node been removed?");
        return btree_status_not_found;
    }

    btree_blk_id const bbid = it->second;
    BlkId blkid = BlkId{bbid.blk_num, m_vdev_blks_per_node, bbid.chunk_num};

    auto raw_buf = hs_utils::iobuf_alloc(buf_size, sisl::buftag::btree_node, m_vdev->align_size());
    m_vdev->sync_read(r_cast< char* >(raw_buf), m_node_size, blkid);

    // Initialize the node
    node = BtreeNodePtr{btree.init_node(raw_buf, node_id, false /* init_buf*/, BtreeNode::identify_leaf_node(raw_buf),
                                        sizeof(COWBtreeNode))};
    cow_btree_node_constructor(node);

    // Add the node to the cache
    if (!m_cache.insert(node)) {
        // There is a race between 2 concurrent reads of same node, Re-read from cache again
        cow_btree_node_destructor(node.get());
        goto retry;
    }

    return btree_status_t::success;
}

btree_status_t COWBtreeStore::refresh_node(BtreeBase& bt, BtreeNodePtr const& node, bool for_read_modify_write,
                                           void* context) const {
    if (context == nullptr || !for_read_modify_write) { return btree_status_t::success; }

    COWBtreeCPContext* cp_ctx = r_cast< COWBtreeCPContext* >(context);
    auto const mod_cp_id = node->get_modified_cp_id();
    auto const cur_cp_id = cp_ctx->id();
    if (mod_cp_id == cur_cp_id) {
        // For same cp, we don't need a copy, we can rewrite on the same buffer
        return btree_status_t::success;
    } else if (mod_cp_id > cp_ctx->id()) {
        return btree_status_t::cp_mismatch; // We are asked to provide the buffer of an older CP, which is not possible
    } else {
        COWBtree& cow_bt = to_cow_btree(bt);
        to_cow_btree_node(node)->copy_buf_if_needed(cow_bt, cp_ctx);
        cow_bt.add_to_dirty_list(node, cp_ctx);
        return btree_status_t::success;
    }
}

void COWBtreeStore::on_node_freed(BtreeNode* node) { cow_btree_node_destructor(node); }

void MemBtreeStore::remove_node(const BtreeNodePtr& node, void* context) override { intrusive_ptr_release(node.get()); }

btree_status_t MemBtreeStore::transact_nodes(const BtreeNodeList& new_nodes, const BtreeNodeList& freed_nodes,
                                             const BtreeNodePtr& left_child_node, const BtreeNodePtr& parent_node,
                                             void* context) override {
    for (const auto& node : new_nodes) {
        this->write_node(node, context);
    }
    this->write_node(left_child_node, context);
    this->write_node(parent_node, context);

    for (const auto& node : freed_nodes) {
        this->remove_node(node, locktype_t::WRITE, context);
    }
    return btree_status_t::success;
}

btree_status_t MemBtreeStore::on_root_changed(BtreeNodePtr const&, void*) override { return btree_status_t::success; }

bnodeid_t COWBtreeStore::generate_node_id() { return m_next_bnode_id.fetch_add(1); }

void COWBtreeStore::update_bnode_map(bnodeid_t node_id, BlkId const& blkid) {
    auto [it, happened] = m_bnode_map.insert_or_assign(node_id, btree_blk_id(blkid));
    if (!happened) { HS_LOG_ASSERT(!happened, "Adding node_id {} to bnode map failed"); }
}

struct FlushUnit {
    std::vector< BtreeNodePtr > nodes;
    std::vector< const iovec* > iovs;
    BlkId alloced_blkid;

    FlushUnit(uint32_t max_nodes_flushable) : nodes{max_nodes_flushable}, iovs{max_nodes_flushable} {}

    void reset() {
        nodes.clear();
        iovs.clear();
        alloced_blkid = BlkId{};
    }

    uint32_t fill_upto_alloced_blks(COWBtreeCPContext* cp_ctx) {
        uint32_t const max_count = alloced_blkid.blk_count();
        for (uint32_t i{0}; i < max_count; ++i) {
            auto node = next_dirty();
            if (!node) { return max_count - i; }

            iovs.emplace_back(
                iovec{.iov_base = node->get_flush_version_buf(cp_ctx->cp_id()), .iov_len = node->node_size()});
            nodes.emplace_back(std::move(node));
        }
        return 0;
    }
};

folly::Future< bool > COWBtreeStore::async_cp_flush(COWBtreeCPContext* cp_ctx) {
    LOGTRACEMOD(cowbtreestore, "Starting Index CP Flush with cp context={}", cp_ctx->to_string());
    if (!cp_ctx->any_dirty_buffers()) {
        if (cp_ctx->id() == 0) {
            // For the first CP, we need to flush the journal buffer to the meta blk
            LOGINFO("First time boot cp, we shall flush the vdev to ensure all cp information is created");
            m_vdev->cp_flush(cp_ctx);
        } else {
            CP_PERIODIC_LOG(DEBUG, cp_ctx->id(), "Btree does not have any dirty buffers to flush");
        }
        return folly::makeFuture< bool >(true); // nothing to flush
    }

#ifdef _PRERELEASE
    if (hs()->crash_simulator().is_crashed()) {
        LOGINFOMOD(wbcache, "crash simulation is ongoing, so skip the cp flush");
        return folly::makeFuture< bool >(true);
    }
#endif

    // First thing is to flush the new_blks created as part of the CP.
    cp_ctx->prepare_flush_iteration();

    for (auto& fiber : m_cp_flush_fibers) {
        iomanager.run_on_forget(fiber, [this, cp_ctx]() {
            for (i = 0; i < resource_mgr().get_dirty_buf_qd(); ++i) {
                fill_and_flush_one_unit(std::make_unique< FlushUnit >(cp_ctx, m_max_nodes_per_flush),
                                        true /* part_of_batch*/);
            }
            m_vdev->submit_batch();
        });
    }
    return std::move(cp_ctx->get_future());
}

void COWBtreeStore::fill_and_flush_one_unit(unique< FlushUnit > funit, bool part_of_batch) {
    funit->reset();

    // Allocate max blks and try to pull as much dirty pages into the blks allocated.
    blk_alloc_hints hints{.partial_alloc_ok = true};
    BlkAllocStatus status = m_vdev->alloc_contiguous_blks(m_max_nodes_per_flush, hints, funit->blkid);
    if ((status != BlkAllocStatus::SUCCESS) || (status != BlkAllocStatus::PARTIAL)) {
        HS_REL_ASSERT(false, "Blk allocation to persist btree pages failed, we are crashing for now");
    }
    funit->fill_upto_alloced_blks(ctx);

    m_vdev->async_write(funit->iovs.data(), funit->iovs.size(), funit->blkid, false)
        .thenValue([funit = std::move(funit), this](auto) {
            blk_num_t b = funit->blkid.blk_num();
            for (auto const& n : funit->nodes) {
                process_write_completion(n, BlkId{b++, funit->blkid.chunk_num(), 1u});
            }

            fill_and_flush_one_unit(std::move(funit), false /* part_of_batch */);
        });

    if (!part_of_batch) { m_vdev->submit_batch(); }
}

} // namespace homestore