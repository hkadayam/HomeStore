#pragma once

#include <vector>
#include <memory>

#include <homestore/btree/btree_store.h>
#include <homestore/btree/btree_base.hpp>

namespace homestore {
class MemBtreeStore : public BtreeStore {
private:
    std::vector< std::shared_ptr< uint8_t[] > > node_buf_ptr_vec;

public:
    MemBtreeStore() = default;
    virtual ~MemBtreeStore() = default;

    std::string store_type() const override { return "MEM_BTREE"; }

    unique< UnderlyingBtree > on_btree_created(BtreeBase& btree, bool load_existing) override;
    virtual void on_btree_destroyed(BtreeBase&) override {}
    void on_recovery_completed() override {}

    BtreeNodePtr create_node(BtreeBase& btree, bool is_leaf, void* context) override;
    btree_status_t write_node(BtreeBase&, BtreeNodePtr const& node, void* context) override;
    btree_status_t read_node(BtreeBase&, bnodeid_t id, BtreeNodePtr& node) override;
    btree_status_t refresh_node(BtreeBase&, BtreeNodePtr const& node, bool for_read_modify_write,
                                void* context) override;
    void remove_node(BtreeBase&, BtreeNodePtr const& node, void* context) override;
    btree_status_t transact_nodes(BtreeBase&, BtreeNodeList const& new_nodes, BtreeNodeList const& freed_nodes,
                                  BtreeNodePtr const& left_child_node, BtreeNodePtr const& parent_node,
                                  void* context) override;
    btree_status_t on_root_changed(BtreeBase&, BtreeNodePtr const&, void*) override;
    void on_node_freed(BtreeNode* node) override;

    bool is_fast_destroy_supported() const override { return true; }

    uint64_t used_size(BtreeBase const& btree) const override { return 0; }
};

} // namespace homestore