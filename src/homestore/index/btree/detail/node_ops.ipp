#pragma once

#include "homestore/index/btree/btree_base.h"
#include "homestore/index/btree/btree_kv.h"
#include "homestore/index/btree/detail/btree_internal.h"
#include "homestore/index/btree/detail/btree_node.h"

namespace homestore {

/// @brief Node-level operations for a Btree<K, V>.
///
/// All V-sensitive (overflow-aware) and role-sensitive (leaf vs interior) node operations live here.
/// Btree core code accesses node mutations only through this class — the raw NodeCore APIs
/// (insert/update/remove/get_nth_value/...) are reserved for the leaf_* and interior_* wrappers below.
///
/// Held as an inline member of Btree<K, V>; lifetime is bounded by the owning Btree.
template < typename K, typename V >
class NodeOps {
public:
    NodeOps(BtreeConfig const& cfg, UnderlyingBtree& underlying) : cfg_(cfg), underlying_(underlying) {}

    NodeOps(NodeOps const&) = delete;
    NodeOps& operator=(NodeOps const&) = delete;

    // ── Leaf ops: overflow-transparent (values are V, may spill to overflow blocks) ───────────────
    // out_val is BtreeValue& so callers can pass type-erased BtreeValue* (from request structs) by
    // dereference, without a downcast at the call site.  The runtime type must be V — NodeOps casts
    // internally; this is sound because NodeOps< K, V > is only ever instantiated by Btree< K, V >.
    BtreeTask< BtreeStatus > leaf_read_value(Node const& node, uint32_t idx, BtreeValue& out_val) const;
    BtreeTask< BtreeStatus > leaf_update_value(Node const& node, uint32_t idx, BtreeValue const& val);
    BtreeTask< BtreeStatus > leaf_insert_kv(Node const& node, uint32_t idx, BtreeKey const& key, BtreeValue const& val);
    void leaf_remove_kv(Node const& node, uint32_t idx);

    // ── Interior ops: values are always NodeLink (child pointers, fixed-size inline) ───────────────
    void insert_child(Node const& node, uint32_t idx, BtreeKey const& key, NodeLink const& link);
    void update_child(Node const& node, uint32_t idx, NodeLink const& link);
    void update_child(Node const& node, uint32_t idx, BtreeKey const& key, NodeLink const& link);
    void update_key(Node const& node, uint32_t idx, BtreeKey const& key);
    void remove_child(Node const& node, uint32_t idx);
    void remove_children(Node const& node, uint32_t start_idx, uint32_t end_idx);
    void set_edge_link(Node const& node, NodeLink const& link);
    NodeLink get_edge_link(Node const& node) const;

    // Room check.  Collapses an overflow-bound value down to BlkId size before asking the node, so the
    // caller passes the user-facing value size and doesn't need to know about overflow.  For interior
    // (NodeLink) the value is always below inline_value_size, so the collapse is a no-op.
    bool has_room(Node const& node, BtreePutType put_type, uint32_t key_size, uint32_t value_size) const;

private:
    // Free the overflow block at idx if it is an overflow entry. Internal helper used by leaf_update_value
    // and leaf_remove_kv; never called from Btree core.
    void leaf_free_overflow(Node const& node, uint32_t idx);

    BtreeConfig const& cfg_;
    UnderlyingBtree& underlying_;
};

// ─────────────────────────────────────── Implementations ──────────────────────────────────────────

template < typename K, typename V >
BtreeTask< BtreeStatus > NodeOps< K, V >::leaf_read_value(Node const& node, uint32_t idx, BtreeValue& out_val) const {
    V& out = s_cast< V& >(out_val);
    if (node->is_nth_value_overflow(idx)) {
        ValueOrOverflow< V > vref;
        node->get_nth_value(idx, &vref, /*copy=*/false);
        if (vref.is_overflow()) {
            sisl::ByteArray buf;
            auto status = CO_AWAIT underlying_.read_overflow(vref.blkid(), buf);
            if (status != BtreeStatus::success) {
                CO_RETURN status;
            }
            out.deserialize(*buf, /*is_overflow=*/true);
        } else {
            out = vref.inline_value();
        }
    } else {
        node->get_nth_value(idx, &out, /*copy=*/true);
    }
    CO_RETURN BtreeStatus::success;
}

template < typename K, typename V >
void NodeOps< K, V >::leaf_free_overflow(Node const& node, uint32_t idx) {
    if (node->is_nth_value_overflow(idx)) {
        ValueOrOverflow< V > vref;
        node->get_nth_value(idx, &vref, false);
        underlying_.delete_overflow(vref.blkid());
    }
}

template < typename K, typename V >
BtreeTask< BtreeStatus > NodeOps< K, V >::leaf_update_value(Node const& node, uint32_t idx, BtreeValue const& val) {
    leaf_free_overflow(node, idx);

    if (val.serialized_size() > cfg_.inline_value_size()) {
        BlkId bid;
        auto status = underlying_.write_overflow(val.serialize_to_byte_array(), bid);
        if (status != BtreeStatus::success) {
            CO_RETURN status;
        }
        CO_RETURN node->update(idx, ValueOrOverflow< V >::make_overflow(bid));
    }
    CO_RETURN node->update(idx, val);
}

template < typename K, typename V >
BtreeTask< BtreeStatus > NodeOps< K, V >::leaf_insert_kv(Node const& node, uint32_t idx, BtreeKey const& key,
                                                         BtreeValue const& val) {
    if (val.serialized_size() > cfg_.inline_value_size()) {
        BlkId bid;
        auto status = underlying_.write_overflow(val.serialize_to_byte_array(), bid);
        if (status != BtreeStatus::success) {
            CO_RETURN status;
        }
        node->insert(idx, key, ValueOrOverflow< V >::make_overflow(bid));
    } else {
        node->insert(idx, key, val);
    }
    CO_RETURN BtreeStatus::success;
}

template < typename K, typename V >
void NodeOps< K, V >::leaf_remove_kv(Node const& node, uint32_t idx) {
    leaf_free_overflow(node, idx);
    node->remove(idx);
}

template < typename K, typename V >
bool NodeOps< K, V >::has_room(Node const& node, BtreePutType put_type, uint32_t key_size,
                                uint32_t value_size) const {
    return node->has_room_for_put(put_type, key_size,
                                  (value_size > cfg_.inline_value_size()) ? to_u32(sizeof(BlkId)) : value_size);
}

template < typename K, typename V >
void NodeOps< K, V >::insert_child(Node const& node, uint32_t idx, BtreeKey const& key, NodeLink const& link) {
    node->insert(idx, key, link);
}

template < typename K, typename V >
void NodeOps< K, V >::update_child(Node const& node, uint32_t idx, NodeLink const& link) {
    node->update(idx, link);
}

template < typename K, typename V >
void NodeOps< K, V >::update_key(Node const& node, uint32_t idx, BtreeKey const& key) {
    node->update(idx, key);
}

template < typename K, typename V >
void NodeOps< K, V >::update_child(Node const& node, uint32_t idx, BtreeKey const& key, NodeLink const& link) {
    node->update(idx, key, link);
}

template < typename K, typename V >
void NodeOps< K, V >::remove_child(Node const& node, uint32_t idx) {
    node->remove(idx);
}

template < typename K, typename V >
void NodeOps< K, V >::remove_children(Node const& node, uint32_t start_idx, uint32_t end_idx) {
    node->remove(start_idx, end_idx);
}

template < typename K, typename V >
void NodeOps< K, V >::set_edge_link(Node const& node, NodeLink const& link) {
    node->set_edge_value(link);
}

template < typename K, typename V >
NodeLink NodeOps< K, V >::get_edge_link(Node const& node) const {
    return node->get_edge_value();
}

} // namespace homestore
