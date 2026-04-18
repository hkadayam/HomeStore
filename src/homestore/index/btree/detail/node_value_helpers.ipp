#pragma once
#include "homestore/index/btree/btree.h"

namespace homestore {

// Read the value at idx into out_val, resolving overflow transparently.
template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::read_from_node(Node const& node, uint32_t idx, V& out_val) const {
    if (node->is_nth_value_overflow(idx)) {
        ValueOrOverflow< V > vref;
        node->get_nth_value(idx, &vref, /*copy=*/false);
        if (vref.is_overflow()) {
            sisl::ByteArray buf;
            auto status = CO_AWAIT underlying_->read_overflow(vref.blkid(), buf);
            if (status != BtreeStatus::success) {
                CO_RETURN status;
            }
            out_val.deserialize(*buf, /*is_overflow=*/true);
        } else {
            out_val = vref.inline_value();
        }
    } else {
        node->get_nth_value(idx, &out_val, /*copy=*/true);
    }
    CO_RETURN BtreeStatus::success;
}

// Free the overflow block at idx if it is an overflow entry. Lightweight — only deserializes when the bit is set.
template < typename K, typename V >
void Btree< K, V >::free_if_overflow(Node const& node, uint32_t idx) {
    if (node->is_nth_value_overflow(idx)) {
        ValueOrOverflow< V > vref;
        node->get_nth_value(idx, &vref, false);
        underlying_->delete_overflow(vref.blkid());
    }
}

// Update the value at idx. Frees old overflow if present, writes new value inline or overflow based on threshold.
template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::update_in_node(Node const& node, uint32_t idx, BtreeValue const& val) {
    free_if_overflow(node, idx);

    if (val.serialized_size() > bt_cfg_.inline_value_size()) {
        BlkId bid;
        auto status = underlying_->write_overflow(val.serialize_to_byte_array(), bid);
        if (status != BtreeStatus::success) {
            CO_RETURN status;
        }
        CO_RETURN node->update(idx, ValueOrOverflow< V >::make_overflow(bid));
    }
    CO_RETURN node->update(idx, val);
}

// Insert a value at idx. Writes inline or overflow based on threshold.
template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::insert_in_node(Node const& node, uint32_t idx, BtreeKey const& key,
                                                       BtreeValue const& val) {
    if (val.serialized_size() > bt_cfg_.inline_value_size()) {
        BlkId bid;
        auto status = underlying_->write_overflow(val.serialize_to_byte_array(), bid);
        if (status != BtreeStatus::success) {
            CO_RETURN status;
        }
        node->insert(idx, key, ValueOrOverflow< V >::make_overflow(bid));
    } else {
        node->insert(idx, key, val);
    }
    CO_RETURN BtreeStatus::success;
}

// Remove entry at idx. Frees overflow block if present, then removes the entry.
template < typename K, typename V >
void Btree< K, V >::remove_from_node(Node const& node, uint32_t idx) {
    free_if_overflow(node, idx);
    node->remove(idx);
}

// Check if the node has room for a put, accounting for overflow (value stored as BlkId if above threshold).
template < typename K, typename V >
bool Btree< K, V >::has_room_in_node(Node const& node, BtreePutType put_type, uint32_t key_size,
                                     uint32_t val_size) const {
    return node->has_room_for_put(put_type, key_size,
                                  (val_size > bt_cfg_.inline_value_size()) ? to_u32(sizeof(BlkId)) : val_size);
}

} // namespace homestore
