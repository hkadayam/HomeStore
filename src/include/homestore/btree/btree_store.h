#pragma once

#include <homestore/btree/btree.hpp>

namespace homestore {

class BtreeStore {
public:
    virtual std::string store_type() const = 0;

    // All Btree related operations
    virtual void on_btree_created();

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

    virtual void on_node_freed(BtreeNode* node) = 0;
};
} // namespace homestore