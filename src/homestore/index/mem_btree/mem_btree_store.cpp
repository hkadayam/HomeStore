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

btree_status_t MemBtree::write_node(Node const&) { return btree_status_t::success; }

btree_status_t MemBtree::prepare_for_write(Node const&) { return btree_status_t::success; }

void MemBtree::remove_node(Node const& node) {
    // MemBtree owns node memory; NodeCore was allocated by BtreeBase::alloc_node_core.
    delete node.operator->();
}

NodeId MemBtree::load_root_node_id() { return NodeId{empty_bnodeid}; }

btree_status_t MemBtree::on_root_changed(Node const&) { return btree_status_t::success; }

} // namespace homestore