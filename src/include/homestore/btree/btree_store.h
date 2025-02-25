#pragma once

#include <homestore/btree/btree.hpp>

namespace homestore {

class BtreeStore : public IndexStore {
public:
    // All Btree related operations
    virtual unique< UnderlyingBtree > on_btree_created(BtreeBase& btree, bool load_existing) = 0;
    virtual void on_btree_destroyed(BtreeBase& btree) = 0;

    // All individual node specific operations
    virtual BtreeNodePtr create_node(BtreeBase& btree, bool is_leaf) = 0;
    virtual btree_status_t write_node(BtreeBase& btree, BtreeNodePtr const& node, void* context) = 0;
    virtual btree_status_t read_node(BtreeBase& btree, bnodeid_t id, BtreeNodePtr& node) = 0;
    virtual btree_status_t refresh_node(BtreeBase& btree, BtreeNodePtr const& node, bool for_read_modify_write,
                                        void* context) = 0;
    virtual void remove_node(BtreeBase& btree, BtreeNodePtr const& node, void* context) = 0;
    virtual btree_status_t transact_nodes(BtreeBase& btree, const BtreeNodeList& new_nodes,
                                          const BtreeNodeList& freed_nodes, const BtreeNodePtr& left_child_node,
                                          const BtreeNodePtr& parent_node, void* context) = 0;
    virtual btree_status_t on_root_changed(BtreeBase& btree, BtreeNodePtr const& root, void* context) = 0;

    // Called whenever a particular btree node has been freed. The underlying implementation could use this oppurtunity
    // to free any contexts stored for this node.
    virtual void on_node_freed(BtreeNode* node) = 0;
};
} // namespace homestore