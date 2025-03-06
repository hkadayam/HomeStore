#pragma once

#include <homestore/btree/detail/btree_internal.hpp>

namespace homestore {

class BtreeBase;
class UnderlyingBtree;

class BtreeStore : public IndexStore {
public:
    // All Btree related operations
    virtual unique< UnderlyingBtree > on_btree_created(BtreeBase& btree, bool load_existing) = 0;
    virtual void on_btree_destroyed(BtreeBase& btree) = 0;

    // All individual node specific operations
    virtual BtreeNodePtr create_node(BtreeBase& btree, bool is_leaf, void* context) = 0;
    virtual btree_status_t write_node(BtreeBase& btree, BtreeNodePtr const& node, void* context) = 0;
    virtual btree_status_t read_node(BtreeBase& btree, bnodeid_t id, BtreeNodePtr& node) = 0;
    virtual btree_status_t refresh_node(BtreeBase& btree, BtreeNodePtr const& node, bool for_read_modify_write,
                                        void* context) = 0;
    virtual void remove_node(BtreeBase& btree, BtreeNodePtr const& node, void* context) = 0;
    virtual btree_status_t transact_nodes(BtreeBase& btree, const BtreeNodeList& new_nodes,
                                          const BtreeNodeList& removed_nodes, const BtreeNodePtr& left_child_node,
                                          const BtreeNodePtr& parent_node, void* context) = 0;
    virtual btree_status_t on_root_changed(BtreeBase& btree, BtreeNodePtr const& root, void* context) = 0;

    // Called whenever a particular btree node has been freed. The underlying implementation could use this oppurtunity
    // to free any contexts stored for this node.
    virtual void on_node_freed(BtreeNode* node) = 0;

    // When a particular btree is to be destroyed, some stores can support fast destroy mechanism, where all the btree
    // nodes can be freed in one go (in a single Checkpoint) without merging the tree and collapsing the tree. This
    // saves lots of IOs while destroying a btree. The requirement from the store is that it should be able to destroy
    // and free all nodes within single checkpoint. If store doesn't support, then btree library itself will keep
    // merging entities and collapsing the tree.
    virtual bool is_fast_destroy_supported() const = 0;

    virtual uint64_t used_size(BtreeBase const& btree) const = 0;
};
} // namespace homestore