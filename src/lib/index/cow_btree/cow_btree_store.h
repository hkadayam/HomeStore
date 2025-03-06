#pragma once

#include <vector>
#include <atomic>
#include <unordered_map>
#include <sisl/cache/simple_cache.hpp>

#include <homestore/blk.h>
#include <homestore/btree/btree_store.h>
#include <homestore/btree/detail/btree_internal.hpp>
#include <homestore/superblk_handler.hpp>
#include <homestore/checkpoint/cp_mgr.hpp>

#include "common/homestore_utils.hpp"

namespace homestore {
class COWBtreeCPContext;
class VirtualDev;

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
    shared< VirtualDev > m_vdev;
    uint32_t const m_node_size;
    uint32_t const m_vdev_blks_per_node;

    // List of fibers to flush (note that this could be on multiple threads)
    std::vector< iomgr::io_fiber_t > m_cp_flush_fibers;

    // All loaded journals arranged by the btree ordinals
    std::unordered_map< uint32_t, std::vector< sisl::byte_view > > m_journals_by_btree;

    // All journals maintained (sorted) by its cp_id
    std::vector< superblk< IndexStoreSuperBlock > > m_journals_by_cpid;

public:
    COWBtreeStore(shared< VirtualDev > vdev, std::vector< superblk< IndexStoreSuperBlock > > store_sbs,
                  shared< sisl::Evictor > evictor, uint32_t node_size);
    virtual ~COWBtreeStore() = default;

    //////////////////////// Override of IndexStore Interfaces //////////////////////////
    std::string store_type() const override { return "COW_BTREE"; }
    void on_recovery_completed() override;

    ////////////////// Override Implementation of underlying store requirements //////////////////
    unique< UnderlyingBtree > on_btree_created(BtreeBase& btree, bool load_existing) override;
    void on_btree_destroyed(BtreeBase& bt) override;

    BtreeNodePtr create_node(BtreeBase& btree, bool is_leaf, void* context) override;

    btree_status_t write_node(BtreeBase& btree, const BtreeNodePtr& node, void* context) override;

    btree_status_t read_node(BtreeBase& btree, bnodeid_t id, BtreeNodePtr& node) override;

    btree_status_t refresh_node(BtreeBase& btree, const BtreeNodePtr& node, bool for_read_modify_write,
                                void* context) override;

    void remove_node(BtreeBase& btree, const BtreeNodePtr& node, void* context) override;

    btree_status_t transact_nodes(BtreeBase& btree, const BtreeNodeList& new_nodes, const BtreeNodeList& removed_nodes,
                                  const BtreeNodePtr& left_child_node, const BtreeNodePtr& parent_node,
                                  void* context) override;

    btree_status_t on_root_changed(BtreeBase& btree, BtreeNodePtr const& root, void* context) override;

    void on_node_freed(BtreeNode* node) override;

    bool is_fast_destroy_supported() const override { return true; }

    uint64_t used_size(BtreeBase const& btree) const override;

    folly::Future< bool > async_cp_flush(COWBtreeCPContext* cp_ctx);

    uint32_t parallel_map_flushers_count() const;

private:
    void flush_map(COWBtreeCPContext* cp_ctx);
    void load_journal(superblk< IndexStoreSuperBlock >& store_journal);
};
} // namespace homestore
