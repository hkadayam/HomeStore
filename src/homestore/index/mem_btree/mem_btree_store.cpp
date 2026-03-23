#include "index/mem_btree/mem_btree_store.h"
#include <homestore/index/btree/detail/btree_node.h>
#include <homestore/index/btree/btree_base.h>

namespace homestore {

unique< UnderlyingBtree > MemBtreeStore::create_underlying_btree(BtreeBase& btree, bool load_existing) {
    return std::make_unique< MemBtree >(btree);
}

MemBtree::MemBtree(BtreeBase& btree) : m_base_btree{btree} {}

Node MemBtree::create_node(bool is_leaf) {
    NodeCore* core = m_base_btree.alloc_node_core(bnodeid_t{0}, is_leaf);
    core->set_node_id(bnodeid_t{r_cast< std::uintptr_t >(core)});
    MemNodeHandle handle{core};
    return Node{handle, LockType::None};
}

Node MemBtree::read_node(bnodeid_t id) const {
    MemNodeHandle handle{r_cast< NodeCore* >(id)};
    return Node{handle, LockType::None};
}

btree_status_t MemBtree::write_node(Node const& node, CPContext*) { return btree_status_t::success; }

btree_status_t MemBtree::refresh_node(Node const& node, bool for_read_modify_write, CPContext*) {
    return btree_status_t::success;
}

void MemBtree::remove_node(Node const& node, CPContext*) {
    // MemBtree owns node memory; free via alloc_node_core's reciprocal.
    // The NodeCore was allocated by BtreeBase::alloc_node_core (placement-new into
    // allocator-managed buffer). Destroy it the same way the allocator expects.
    delete node.operator->();
}

btree_status_t MemBtree::transact_nodes(NodeList const& new_nodes, NodeList const& freed_nodes,
                                        Node const& left_child_node, Node const& parent_node,
                                        CPContext* context) {
    for (auto const& node : new_nodes) {
        m_base_btree.write_node(node, context);
    }
    m_base_btree.write_node(left_child_node, context);
    m_base_btree.write_node(parent_node, context);

    for (auto const& node : freed_nodes) {
        m_base_btree.remove_node(node, context);
    }
    return btree_status_t::success;
}

BtreeLinkInfo MemBtree::load_root_node_id() { return BtreeLinkInfo{empty_bnodeid, 0}; }

btree_status_t MemBtree::on_root_changed(Node const&, CPContext*) { return btree_status_t::success; }

} // namespace homestore
