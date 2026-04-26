#pragma once
#include "homestore/index/btree/btree.h"

namespace homestore {

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::do_query(BtreeQueryRequest< K >& qreq,
                                                 std::vector< std::pair< K, V > >& out_values) {
    COUNTER_INCREMENT(metrics_, btree_query_ops_count, 1);

    if (qreq.batch_size() == 0) {
        CO_RETURN BtreeStatus::success;
    }

    BT_LOG(TRACE, "query: batch_size={} root_node_id={}", qreq.batch_size(), root_node_id_);
    BtreeStatus ret{BtreeStatus::success};
    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, LockType::Read));
        if (!root_result.hasValue()) {
            ret = root_result.error();
            goto out;
        }

        switch (qreq.query_type()) {
        case BtreeQueryType::Sweep:
            ret = CO_AWAIT(do_sweep_query(std::move(root_result.value()), qreq, out_values));
            break;
        case BtreeQueryType::Traversal:
            ret = CO_AWAIT(do_traversal_query(std::move(root_result.value()), qreq, out_values));
            break;
        default:
            LOGERROR("Query type {} is not supported yet", qreq.query_type());
            break;
        }

        if (out_values.size()) {
            K out_last_key = out_values.back().first;
            if (qreq.reverse_order()) {
                if (out_last_key.compare(qreq.input_range().start_key()) <= 0) {
                    ret = BtreeStatus::success;
                }
                qreq.trim_working_range(std::move(out_last_key), false);
            } else {
                if (out_last_key.compare(qreq.input_range().end_key()) >= 0) {
                    ret = BtreeStatus::success;
                }
                qreq.next_working_range_from(std::move(out_last_key), false);
            }
        } else {
            DEBUG_ASSERT_NE(ret, BtreeStatus::has_more, "Query returned has_more, but no values added");
        }
    }

out:
    if ((ret != BtreeStatus::success) && (ret != BtreeStatus::has_more)) {
        BT_LOG(ERROR, "btree query failed {}", ret);
        COUNTER_INCREMENT(metrics_, query_err_cnt, 1);
    }
    CO_RETURN ret;
}

// ── Sweep query: descend to first leaf, then walk sibling links ──────────────────────────────────────────────────
template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::do_sweep_query(Node my_node, BtreeQueryRequest< K >& qreq,
                                                       std::vector< std::pair< K, V > >& out_values) {
    BtreeStatus ret{BtreeStatus::success};

    BT_NODE_LOG(TRACE, my_node, "do_sweep_query: entries={}", my_node->total_entries());
    if (my_node->is_leaf()) {
        do {
            auto cur_count = CO_AWAIT query_leaf_entries(my_node, qreq, out_values);
            BT_NODE_LOG(TRACE, my_node,
                        "query_leaf_entries returned cur_count={}, out_values.size={}, batch_size={},ext_node={}",
                        cur_count, out_values.size(), qreq.batch_size(), my_node->next_node());
            if (qreq.route_tracing_) {
                append_route_trace(qreq, my_node, BtreeEvent::READ);
            }

            if (out_values.size() >= qreq.batch_size()) {
                ret = BtreeStatus::has_more;
                break;
            }

            {
                auto last_key = my_node->get_last_key< K >();
                auto cmp = last_key.compare(qreq.input_range().end_key());
                if (cmp >= 0) {
                    break;
                }
            }
            if (my_node->next_node() == empty_bnodeid) {
                BT_NODE_LOG(TRACE, my_node, "do_sweep_query: next_node is empty, reached edge of tree");
                break;
            }

            auto next_result = CO_AWAIT(underlying_->read_node(my_node->next_node(), LockType::Read));
            if (!next_result.hasValue()) {
                ret = next_result.error();
                break;
            }
            my_node = std::move(next_result.value());
        } while (true);

        CO_RETURN ret;
    }

    // Interior: find child and descend.
    auto const [found, idx] = my_node->find(qreq.first_key());
    ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node.operator->());
    if (qreq.route_tracing_) {
        append_route_trace(qreq, my_node, BtreeEvent::READ, idx, idx);
    }

    auto child_result = CO_AWAIT(get_child_node(my_node, idx, LockType::Read));
    my_node.release();
    if (!child_result.hasValue()) {
        CO_RETURN child_result.error();
    }
    CO_RETURN CO_AWAIT(do_sweep_query(std::move(child_result.value()), qreq, out_values));
}

// ── Traversal query: descend through all matching children ───────────────────────────────────────────────────────
template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::do_traversal_query(Node my_node, BtreeQueryRequest< K >& qreq,
                                                           std::vector< std::pair< K, V > >& out_values) {
    BtreeStatus ret{BtreeStatus::success};

    if (my_node->is_leaf()) {
        auto cur_count = CO_AWAIT query_leaf_entries(my_node, qreq, out_values);
        if (qreq.route_tracing_) {
            append_route_trace(qreq, my_node, BtreeEvent::READ);
        }
        if (out_values.size() >= qreq.batch_size()) {
            ret = BtreeStatus::has_more;
        }
        CO_RETURN ret;
    }

    auto const [start_found, start_idx] = my_node->find(qreq.first_key());
    auto [end_found, end_idx] = my_node->find(qreq.input_range().end_key());

    if (start_idx == my_node->total_entries() && !my_node->has_valid_edge()) {
        CO_RETURN ret;
    }
    if (end_idx == my_node->total_entries() && !my_node->has_valid_edge()) {
        --end_idx;
    }

    DEBUG_ASSERT_LE(start_idx, end_idx);
    if (qreq.route_tracing_) {
        append_route_trace(qreq, my_node, BtreeEvent::READ, start_idx, end_idx);
    }

    bool const reverse = qreq.reverse_order();
    auto idx = reverse ? end_idx : start_idx;
    for (;;) {
        auto child_result = CO_AWAIT(get_child_node(my_node, idx, LockType::Read));
        if (!child_result.hasValue()) {
            ret = child_result.error();
            break;
        }

        bool const is_last = reverse ? (idx == start_idx) : (idx == end_idx);
        if (is_last) {
            my_node.release();
        }

        ret = CO_AWAIT(do_traversal_query(std::move(child_result.value()), qreq, out_values));
        if (ret == BtreeStatus::has_more || is_last) {
            break;
        }

        reverse ? --idx : ++idx;
    }
    CO_RETURN ret;
}

// ── Leaf helper: read entries in range into out_values, applying filter, resolving overflow ────────────────────────
template < typename K, typename V >
BtreeTask< uint32_t > Btree< K, V >::query_leaf_entries(Node const& node, BtreeQueryRequest< K >& qreq,
                                                        std::vector< std::pair< K, V > >& out_values) {
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    if (!node->match_range(qreq.working_range(), start_idx, end_idx)) {
        CO_RETURN 0;
    }

    uint32_t count{0};
    auto const max_count = qreq.batch_size() - to_u32(out_values.size());
    bool const reverse = qreq.reverse_order();

    auto idx = reverse ? end_idx : start_idx;
    for (;;) {
        if (count >= max_count) {
            break;
        }

        V val;
        auto status = CO_AWAIT node_ops_.leaf_read_value(node, idx, val);
        if (status == BtreeStatus::success) {
            K key = node->get_nth_key< K >(idx, true);
            if (!qreq.filter() || qreq.filter()->check_kv(key, val) == GetFilterDecision::Include) {
                out_values.emplace_back(std::move(key), std::move(val));
                ++count;
            }
        }

        if (reverse) {
            if (idx == start_idx) {
                break;
            }
            --idx;
        } else {
            if (idx == end_idx) {
                break;
            }
            ++idx;
        }
    }
    CO_RETURN count;
}

} // namespace homestore
