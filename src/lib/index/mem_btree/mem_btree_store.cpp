#pragma once

#include <memory>

#include <homestore/btree/mem_btree_store.h>

namespace homestore {
MemBtreeStore::MemBtreeStore(uint32_t node_size) : m_node_size{node_size} {}

BtreeNodePtr MemBtreeStore::create_node(BtreeBase& btree, bool is_leaf) override {
    std::shared_ptr< uint8_t[] > ptr(new uint8_t[m_node_size()]);
    node_buf_ptr_vec.emplace_back(ptr);

    auto new_node = btree.init_node(ptr.get(), bnodeid_t{0}, true, is_leaf, 0 /* context_size */);
    new_node->set_node_id(bnodeid_t{r_cast< std::uintptr_t >(new_node)});
    new_node->m_refcount.increment();
    return BtreeNodePtr{new_node};
}

btree_status_t MemBtreeStore::write_node(BtreeBase&, BtreeNodePtr const& node, void* context) {
    return btree_status_t::success;
}

btree_status_t MemBtreeStore::read_node(BtreeBase&, bnodeid_t id, BtreeNodePtr& node) const {
    node.reset(r_cast< BtreeNode* >(id));
    return btree_status_t::success;
}

btree_status_t MemBtreeStore::refresh_node(BtreeBase&, BtreeNodePtr const& node, bool for_read_modify_write,
                                           void* context) const {
    return btree_status_t::success;
}

void MemBtreeStore::remove_node(BtreeBase&, BtreeNodePtr const& node, void* context) {
    intrusive_ptr_release(node.get());
}

btree_status_t MemBtreeStore::transact_nodes(BtreeBase& btree, BtreeNodeList const& new_nodes,
                                             BtreeNodeList const& freed_nodes, BtreeNodePtr const& left_child_node,
                                             BtreeNodePtr const& parent_node, void* context) {
    for (auto const& node : new_nodes) {
        this->write_node(btree, node, context);
    }
    this->write_node(btree, left_child_node, context);
    this->write_node(btree, parent_node, context);

    for (auto const& node : freed_nodes) {
        this->remove_node(btree, node, locktype_t::WRITE, context);
    }
    return btree_status_t::success;
}

btree_status_t MemBtreeStore::on_root_changed(BtreeBase&, BtreeNodePtr const&, void*) {
    return btree_status_t::success;
}

void MemBtreeStore::on_node_freed(BtreeNode* node) {}

} // namespace homestore