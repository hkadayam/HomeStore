#pragma once
#include "homestore/index/btree/btree.h"

namespace homestore {

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::get(ReqT& greq) {
    static_assert(std::is_same_v< BtreeSingleGetRequest, ReqT > || std::is_same_v< BtreeGetAnyRequest< K >, ReqT >,
                  "get api is called with non get request type");

    auto tree_lock = CO_AWAIT(lock_tree_shared());
    auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, LockType::Read));
    if (!root_result.hasValue()) {
        CO_RETURN root_result.error();
    }
    CO_RETURN CO_AWAIT(do_get(std::move(root_result.value()), greq));
}

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::do_get(Node my_node, ReqT& greq) {
    if (my_node->is_leaf()) {
        if constexpr (std::is_same_v< BtreeSingleGetRequest, ReqT >) {
            auto const [found, idx] = my_node->find(greq.key(), nullptr, false);
            if (!found) {
                CO_RETURN BtreeStatus::key_not_found;
            }

            auto status = CO_AWAIT read_from_node(my_node, idx, *s_cast< V* >(greq.outval_));
            if (status != BtreeStatus::success) {
                CO_RETURN status;
            }

            if (greq.route_tracing_) {
                append_route_trace(greq, my_node, BtreeEvent::READ, idx, idx);
            }
            CO_RETURN BtreeStatus::success;

        } else if constexpr (std::is_same_v< BtreeGetAnyRequest< K >, ReqT >) {
            uint32_t start_idx{0};
            uint32_t end_idx{0};
            if (!my_node->match_range(greq.range_, start_idx, end_idx)) {
                CO_RETURN BtreeStatus::key_not_found;
            }

            uint32_t idx;
            switch (greq.range_.multi_option()) {
            case MultiMatchOption::RIGHT_MOST:
                idx = end_idx;
                break;
            case MultiMatchOption::MID:
                idx = start_idx + (end_idx - start_idx) / 2;
                break;
            default:
                idx = start_idx;
                break;
            }
            if (greq.outkey_) {
                my_node->read_nth_key(idx, *greq.outkey_, true);
            }

            auto status = CO_AWAIT read_from_node(my_node, idx, *s_cast< V* >(greq.outval_));
            if (status != BtreeStatus::success) {
                CO_RETURN status;
            }

            if (greq.route_tracing_) {
                append_route_trace(greq, my_node, BtreeEvent::READ, idx, idx);
            }
            CO_RETURN BtreeStatus::success;
        }
    }

    // Interior node: find child and descend.
    NodeLink child_id;
    if constexpr (std::is_same_v< BtreeGetAnyRequest< K >, ReqT >) {
        auto const [found, idx] = my_node->find(greq.range_.start_key(), &child_id, false);
        ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node.operator->());
        if (greq.route_tracing_) {
            append_route_trace(greq, my_node, BtreeEvent::READ, idx, idx);
        }
    } else if constexpr (std::is_same_v< BtreeSingleGetRequest, ReqT >) {
        auto const [found, idx] = my_node->find(greq.key(), &child_id, false);
        ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node.operator->());
        if (greq.route_tracing_) {
            append_route_trace(greq, my_node, BtreeEvent::READ, idx, idx);
        }
    }

    auto child_result = CO_AWAIT(underlying_->read_node(child_id.id(), LockType::Read));
    if (!child_result.hasValue()) {
        CO_RETURN child_result.error();
    }

    my_node.release();
    CO_RETURN CO_AWAIT(do_get(std::move(child_result.value()), greq));
}

} // namespace homestore
