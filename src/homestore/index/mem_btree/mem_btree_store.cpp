#include "index/mem_btree/mem_btree_store.h"
#include "homestore/index/btree/detail/btree_node.h"
#include "homestore/index/btree/btree_base.h"

namespace homestore {

unique< UnderlyingBtree > MemBtreeStore::create_underlying_btree(BtreeBase& btree, bool load_existing) {
    return std::make_unique< MemBtree >();
}

Node MemBtree::create_node(bool is_leaf) {
    NodeCore* core = base_btree_->alloc_node_core(bnodeid_t{0}, is_leaf);
    core->set_node_id(bnodeid_t{r_cast< std::uintptr_t >(core)});
    MemNodeHandle handle{core};
    return Node{handle, LockType::None};
}

BtreeResult< Node > MemBtree::read_node(bnodeid_t id, LockType lock_type) const {
    MemNodeHandle handle{r_cast< NodeCore* >(id)};
    CO_RETURN CO_AWAIT Node::async_construct(handle, lock_type);
}

void MemBtree::write_node(const Node&) {
}

BtreeStatus MemBtree::prepare_for_write(const Node&) {
    return BtreeStatus::success;
}

void MemBtree::remove_node(const Node& node) {
    delete node.operator->();
}

void MemBtree::on_root_changed(const Node&) {
}

} // namespace homestore
