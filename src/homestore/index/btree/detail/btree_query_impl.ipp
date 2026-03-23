/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once
#include <homestore/index/btree/btree.h>

namespace homestore {

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::query(BtreeQueryRequest< K >& qreq,
                                                  std::vector< std::pair< K, V > >& out_values) {
    COUNTER_INCREMENT(m_metrics, btree_query_ops_count, 1);

    if (qreq.batch_size() == 0) { CO_RETURN btree_status_t::success; }

    btree_status_t ret{btree_status_t::success};
    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());

        auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.bnode_id(), LockType::Read));
        if (read_ret != btree_status_t::success) {
            ret = read_ret;
        } else {
            switch (qreq.query_type()) {
            case BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY:
                ret = CO_AWAIT(do_sweep_query(std::move(root), qreq, out_values));
                break;

            case BtreeQueryType::TREE_TRAVERSAL_QUERY:
                ret = CO_AWAIT(do_traversal_query(std::move(root), qreq, out_values));
                break;

            default:
                // root RAII unlocks via destructor
                LOGERROR("Query type {} is not supported yet", qreq.query_type());
                break;
            }

            if ((qreq.query_type() == BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY ||
                 qreq.query_type() == BtreeQueryType::TREE_TRAVERSAL_QUERY)) {
                if (out_values.size()) {
                    K out_last_key = out_values.back().first;
                    if (out_last_key.compare(qreq.input_range().end_key()) >= 0) {
                        ret = btree_status_t::success;
                    }
                    qreq.shift_working_range(std::move(out_last_key), false /* non inclusive*/);
                } else {
                    DEBUG_ASSERT_NE(ret, btree_status_t::has_more, "Query returned has_more, but no values added")
                }
            }
        }
    }

#ifndef NDEBUG
    check_lock_debug();
#endif
    if ((ret != btree_status_t::success) && (ret != btree_status_t::has_more)) {
        BT_LOG(ERROR, "btree query failed {}", ret);
        COUNTER_INCREMENT(m_metrics, query_err_cnt, 1);
    }
    CO_RETURN ret;
}

// do_sweep_query takes my_node by value — owns the RAII lock.
// In the leaf loop, explicit unlock_node + move to reassign my_node to the next sibling.
template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::do_sweep_query(Node my_node, BtreeQueryRequest< K >& qreq,
                                                          std::vector< std::pair< K, V > >& out_values) {
    btree_status_t ret{btree_status_t::success};

    if (my_node->is_leaf()) {
        BT_NODE_DBG_ASSERT_GT(qreq.batch_size(), 0, my_node.operator->());

        auto count = 0U;
        do {
            uint32_t start_ind{0};
            uint32_t end_ind{0};
            auto cur_count = to_variant_node(my_node)->multi_get(qreq.working_range(), qreq.batch_size() - count,
                                                                 start_ind, end_ind, &out_values, qreq.filter());
            count += cur_count;

            if (qreq.m_route_tracing) {
                append_route_trace(qreq, my_node, btree_event_t::READ, start_ind, start_ind + cur_count);
            }

            if (count < qreq.batch_size()) {
                // Before reading a sibling node, validate if the current node last key is already same as end key.
                if (my_node->get_last_key< K >().compare(qreq.input_range().end_key()) >= 0) { break; }
                if (my_node->next_node() == empty_bnodeid) { break; }

                auto [next_ret, next_node] = CO_AWAIT(read_node(my_node->next_node(), LockType::Read));
                if (next_ret != btree_status_t::success) {
                    ret = next_ret;
                    break;
                }
                // Explicitly unlock current node before advancing to sibling.
                unlock_node(my_node);
                my_node = std::move(next_node);
            } else {
                ret = btree_status_t::has_more;
                break;
            }
        } while (true);

        CO_RETURN ret;
        // RAII: my_node destructor unlocks current node (if not already unlocked by unlock_node above)
    }

    BtreeLinkInfo start_child_info;
    [[maybe_unused]] const auto [isfound, idx] = my_node->find(qreq.first_key(), &start_child_info, false);
    ASSERT_IS_VALID_INTERIOR_CHILD_INDX(isfound, idx, my_node.operator->());
    if (qreq.m_route_tracing) { append_route_trace(qreq, my_node, btree_event_t::READ, idx, idx); }

    auto [child_ret, child] = CO_AWAIT(read_node(start_child_info.bnode_id(), LockType::Read));
    // Unlock parent before recursing — child now owns the lock.
    unlock_node(my_node);
    if (child_ret != btree_status_t::success) { CO_RETURN child_ret; }
    CO_RETURN CO_AWAIT(do_sweep_query(std::move(child), qreq, out_values));
}

// do_traversal_query takes my_node by value — owns the RAII lock.
// Unlocks parent explicitly at the last child index (standard lock-crabbing).
template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::do_traversal_query(Node my_node, BtreeQueryRequest< K >& qreq,
                                                              std::vector< std::pair< K, V > >& out_values) {
    btree_status_t ret{btree_status_t::success};

    if (my_node->is_leaf()) {
        BT_NODE_LOG_ASSERT_GT(qreq.batch_size(), 0, my_node.operator->());

        uint32_t start_ind{0};
        uint32_t end_ind{0};
        auto cur_count = to_variant_node(my_node)->multi_get(qreq.working_range(),
                                                             qreq.batch_size() - uint32_cast(out_values.size()),
                                                             start_ind, end_ind, &out_values, qreq.filter());
        if (qreq.m_route_tracing) {
            append_route_trace(qreq, my_node, btree_event_t::READ, start_ind, start_ind + cur_count);
        }
        if (out_values.size() >= qreq.batch_size()) { ret = btree_status_t::has_more; }
        CO_RETURN ret;
        // RAII: my_node destructor unlocks
    }

    const auto [start_isfound, start_idx] = my_node->find(qreq.first_key(), nullptr, false);
    auto [end_is_found, end_idx] = my_node->find(qreq.input_range().end_key(), nullptr, false);

    if (start_idx == my_node->total_entries() && !(my_node->has_valid_edge())) {
        CO_RETURN ret; // no results; RAII unlocks my_node
    }
    if (end_idx == my_node->total_entries() && !(my_node->has_valid_edge())) {
        --end_idx; // end is not valid
    }

    BT_NODE_LOG_ASSERT_LE(start_idx, end_idx, my_node.operator->());
    if (qreq.m_route_tracing) { append_route_trace(qreq, my_node, btree_event_t::READ, start_idx, end_idx); }

    for (uint32_t idx = start_idx; idx <= end_idx; ++idx) {
        BtreeLinkInfo child_info;
        my_node->get_nth_value(idx, &child_info, false);

        auto [child_ret, child] = CO_AWAIT(read_node(child_info.bnode_id(), LockType::Read));
        if (child_ret != btree_status_t::success) {
            ret = child_ret;
            break;
        }

        if (idx == end_idx) {
            // Last child: release parent lock now (we hold child lock).
            unlock_node(my_node);
        }
        // TODO: pass sub-range if child is leaf
        ret = CO_AWAIT(do_traversal_query(std::move(child), qreq, out_values));
        if (ret == btree_status_t::has_more) { break; }
    }
    // RAII: my_node destructor unlocks (no-op if unlock_node was already called above)
    CO_RETURN ret;
}

///////////////////////////////////////////////////////////////////////////////
// query_traversal — start a paginated query, returning a QueryResultHandle.
///////////////////////////////////////////////////////////////////////////////
template < typename K, typename V >
BtreeTask< QueryResultHandle< K, V > >
Btree< K, V >::query_traversal(BtreeKeyRange< K >&& inp_range, uint32_t batch_size, GetFilter* filter,
                                bool reverse_order, BtreeQueryType query_type) {
    QueryResultHandle< K, V > handle;
    handle.btree_ = this;
    handle.next_range_ = inp_range; // copy for continuation
    handle.query_type_ = query_type;
    handle.batch_size_ = batch_size;
    handle.filter_ = filter;
    handle.reverse_order_ = reverse_order;

    BtreeQueryRequest< K > qreq{*this, std::move(inp_range), query_type, batch_size, filter, reverse_order};

    auto ret = CO_AWAIT(query(qreq, handle.results));
    handle.has_more_ = (ret == btree_status_t::has_more);

    if (handle.has_more_) {
        // Save the shifted working range for the next batch.
        handle.next_range_ = qreq.working_range();
    }
    CO_RETURN handle;
}

///////////////////////////////////////////////////////////////////////////////
// query_next_batch — continue a paginated query from a QueryResultHandle.
///////////////////////////////////////////////////////////////////////////////
template < typename K, typename V >
BtreeTask< QueryResultHandle< K, V > > Btree< K, V >::query_next_batch(QueryResultHandle< K, V >&& handle) {
    QueryResultHandle< K, V > next;
    next.btree_ = handle.btree_;
    next.query_type_ = handle.query_type_;
    next.batch_size_ = handle.batch_size_;
    next.filter_ = handle.filter_;
    next.reverse_order_ = handle.reverse_order_;

    BtreeKeyRange< K > range = std::move(handle.next_range_);
    BtreeQueryRequest< K > qreq{*this, std::move(range), handle.query_type_, handle.batch_size_, handle.filter_,
                                handle.reverse_order_};

    auto ret = CO_AWAIT(query(qreq, next.results));
    next.has_more_ = (ret == btree_status_t::has_more);

    if (next.has_more_) { next.next_range_ = qreq.working_range(); }
    CO_RETURN next;
}

} // namespace homestore
