#pragma once

#include <vector>
#include <memory>

#ifdef StoreSpecificBtreeNode
#undef StoreSpecificBtreeNode
#endif

#define StoreSpecificBtreeNode BtreeNode

#include <homestore/btree/btree.hpp>

namespace homestore {
class MemBtreeStore : public BtreeStore {
private:
    std::vector< std::shared_ptr< uint8_t[] > > node_buf_ptr_vec;

public:
    MemBtreeStore(uint32_t node_size);
    virtual ~MemBtreeStore() = default;

    std::string store_type() const override { return "MEM_BTREE"; }

    BtreeNodePtr create_node(BtreeBase& btree, bool is_leaf) override;

    btree_status_t write_node(BtreeBase&, BtreeNodePtr& node, void* context) override;

    btree_status_t read_node(BtreeBase&, bnodeid_t id, BtreeNodePtr& node) const override;

    btree_status_t refresh_node(BtreeBase&, BtreeNodePtr const& node, bool for_read_modify_write,
                                void* context) const override;

    void remove_node(BtreeBase&, BtreeNodePtr const& node, void* context) override;

    btree_status_t transact_nodes(BtreeBase&, BtreeNodeList const& new_nodes, BtreeNodeList const& freed_nodes,
                                  BtreeNodePtr const& left_child_node, BtreeNodePtr const& parent_node,
                                  void* context) override;

    btree_status_t on_root_changed(BtreeBase&, BtreeNodePtr const&, void*) override;

    void on_node_freed(BtreeNode* node) override;
};

} // namespace homestore