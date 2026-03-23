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
template < typename ReqT >
BtreeTask< btree_status_t > Btree< K, V >::put(ReqT& put_req) {
    static_assert(std::is_same_v< ReqT, BtreeSinglePutRequest > || std::is_same_v< ReqT, BtreeRangePutRequest< K > > ||
                      std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > > ||
                      std::is_same_v< ReqT, BtreeScanPutRequest< K > >,
                  "put api is called with non put request type");
    COUNTER_INCREMENT(m_metrics, btree_write_ops_count, 1);

    for (;;) {
        bool need_split{false};
        {
            auto tree_lock = CO_AWAIT(lock_tree_shared());
            auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.id(), LockType::ReadInteriorWriteLeaf));
            if (read_ret != btree_status_t::success) { CO_RETURN read_ret; }

            if (is_split_needed(root, put_req)) {
                unlock_node(root);
                need_split = true;
                // tree_lock released at end of scope before check_split_root takes excl lock
            } else {
                auto ret = CO_AWAIT(do_put(std::move(root), put_req));
                if (ret != btree_status_t::retry) {
                    if (ret != btree_status_t::success) {
                        BT_LOG(ERROR, "btree put failed {}", ret);
                        COUNTER_INCREMENT(m_metrics, write_err_cnt, 1);
                    }
                    CO_RETURN ret;
                }
                BT_LOG(TRACE, "retrying put operation because btree reported retriable status {}", ret);
            }
        }

        if (need_split) {
            auto split_ret = CO_AWAIT(check_split_root(put_req));
            if (split_ret != btree_status_t::success) {
                LOGERROR("root split failed btree name {}", m_bt_cfg.name());
                CO_RETURN split_ret;
            }
        }
    }
}

/*
 * Takes my_node by value — owns the lock. RAII unlocks on return.
 * Expects: node is not full (split check done by caller).
 */
template < typename K, typename V >
template < typename ReqT >
BtreeTask< btree_status_t > Btree< K, V >::do_put(Node my_node, ReqT& req) {
    btree_status_t ret = btree_status_t::success;

    if (my_node->is_leaf()) {
        ret = mutate_write_leaf_node(my_node, req);
        CO_RETURN ret; // RAII: my_node unlocked when function returns
    }

retry:
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    uint32_t curr_idx;

    if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > > ||
                  std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
        const auto matched = my_node->match_range(req.working_range(), start_idx, end_idx);
        if (!matched) {
            BT_NODE_LOG_ASSERT(false, my_node.operator->(), "match_range returns 0 entries for interior node");
            ret = btree_status_t::put_failed;
            goto out;
        }
    } else if constexpr (std::is_same_v< ReqT, BtreeSinglePutRequest > ||
                         std::is_same_v< ReqT, BtreeScanPutRequest< K > >) {
        auto const [found, idx] = my_node->find(req.key(), nullptr, true);
        ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node.operator->());
        end_idx = start_idx = idx;
    }

    BT_NODE_DBG_ASSERT((my_node.lock_type() == LockType::Read || my_node.lock_type() == LockType::Write),
                       my_node.operator->(), "unexpected locktype {}", my_node.lock_type());

    if (req.m_route_tracing) { append_route_trace(req, my_node, btree_event_t::READ, start_idx, end_idx); }

    curr_idx = start_idx;
    while (curr_idx <= end_idx) {
        NodeId child_id;
        auto [child_ret, child_node] =
            CO_AWAIT(get_child_node(my_node, curr_idx, child_id, LockType::ReadInteriorWriteLeaf));
        if (child_ret != btree_status_t::success) {
            ret = (child_ret == btree_status_t::not_found) ? btree_status_t::retry : child_ret;
            goto out;
        }

        if (is_split_needed(child_node, req)) {
            ret = CO_AWAIT(upgrade_node_locks(my_node, child_node));
            if (ret != btree_status_t::success) {
                BT_NODE_LOG(DEBUG, my_node.operator->(), "Upgrade of node lock failed, retrying from root");
                goto out;
            }

            K split_key;
            BT_NODE_LOG(TRACE, my_node.operator->(), "Split node needed");
            ret = split_node(my_node, child_node, curr_idx, &split_key);
            // child_node goes out of scope here → RAII unlocks
            if (ret != btree_status_t::success) { goto out; }

            if (req.m_route_tracing) { append_route_trace(req, child_node, btree_event_t::SPLIT); }
            COUNTER_INCREMENT(m_metrics, btree_split_count, 1);
            goto retry;
        }

        if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > > ||
                      std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
            if (child_node->is_leaf()) {
                if (curr_idx < my_node->total_entries()) {
                    K child_end_key = my_node->get_nth_key< K >(curr_idx, true);
                    if (child_end_key.compare(req.working_range().end_key()) < 0) {
                        req.trim_working_range(std::move(child_end_key), true);
                    }
                    BT_NODE_LOG(DEBUG, my_node.operator->(), "Subrange:idx=[{}-{}],c={},working={}", start_idx, end_idx,
                                curr_idx, req.working_range().to_string());
                }
            }
        }

#ifndef NDEBUG
        K ckey, pkey;
        if (curr_idx != my_node->total_entries()) {
            pkey = my_node->get_nth_key< K >(curr_idx, true);
            if (child_node->total_entries() != 0) {
                ckey = child_node->get_last_key< K >();
                if (!child_node->is_leaf()) {
                    BT_NODE_DBG_ASSERT_EQ(ckey.compare(pkey), 0, my_node.operator->());
                } else {
                    BT_NODE_DBG_ASSERT_LE(ckey.compare(pkey), 0, my_node.operator->());
                }
            }
        }
        if (curr_idx > 0) {
            pkey = my_node->get_nth_key< K >(curr_idx - 1, true);
            if (child_node->total_entries() != 0) {
                ckey = child_node->get_first_key< K >();
                BT_NODE_DBG_ASSERT_GE(ckey.compare(pkey), 0, child_node.operator->());
            }
        }
#endif

        if (curr_idx == end_idx) {
            // Unlock parent before descending into last child — no longer needed for range decisions.
            unlock_node(my_node);
        }

        ret = CO_AWAIT(do_put(std::move(child_node), req));
        if (ret != btree_status_t::success) { goto out; }

        ++curr_idx;
    }

out:
    // If my_node is still locked (wasn't unlocked in the last-child branch), RAII handles it.
    CO_RETURN ret;
}

template < typename K, typename V >
template < typename ReqT >
btree_status_t Btree< K, V >::mutate_write_leaf_node(Node const& my_node, ReqT& req) {
    btree_status_t ret = btree_status_t::success;

    if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > >) {
        K last_failed_key;
        ret = to_variant_node(my_node)->multi_put(req.working_range(), req.input_range().start_key(), *req.m_newval,
                                                  req.m_put_type, &last_failed_key, req.m_filter);
        if (ret == btree_status_t::has_more) {
            req.shift_working_range(std::move(last_failed_key), true);
            ret = btree_status_t::success; // internally consumed — no pagination exposed to caller
        } else if (ret == btree_status_t::success) {
            req.shift_working_range();
        }
    } else if constexpr (std::is_same_v< ReqT, BtreeSinglePutRequest >) {
        if (!to_variant_node(my_node)->put(req.key(), req.value(), req.m_put_type, req.m_existing_val, req.m_filter)) {
            ret = btree_status_t::put_failed;
        }
        COUNTER_INCREMENT(m_metrics, btree_obj_count, 1);
    } else if constexpr (std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
        while (!req.done()) {
            const auto& [key, val] = req.current();
            if (key.compare(req.m_end_key) > 0) break;

            auto const [found, idx] = my_node->find(key, nullptr, false);
            if (found) {
                switch (req.m_put_type) {
                case btree_put_type::REPLACE_ONLY_IF_EXISTS:
                case btree_put_type::UPSERT:
                    my_node->update(idx, val);
                    ++req.m_stats.updated;
                    break;
                default:
                    break; // INSERT_ONLY_IF_NOT_EXISTS: skip duplicates
                }
            } else {
                if (req.m_put_type != btree_put_type::REPLACE_ONLY_IF_EXISTS) {
                    if (!my_node->has_room_for_put(req.m_put_type, key.serialized_size(), val.serialized_size())) {
                        ret = btree_status_t::retry;
                        break;
                    }
                    my_node->insert(idx, key, val);
                    ++req.m_stats.inserted;
                    COUNTER_INCREMENT(m_metrics, btree_obj_count, 1);
                }
            }
            req.advance();
        }
        if (ret == btree_status_t::success) { req.shift_working_range(); }
    } else if constexpr (std::is_same_v< ReqT, BtreeScanPutRequest< K > >) {
        uint32_t start_idx = 0, end_idx = 0;
        my_node->match_range(req.m_scan_range, start_idx, end_idx);

        size_t scanned = 0;
        uint32_t idx = start_idx;
        while (idx <= end_idx && scanned < req.m_max_scan) {
            auto decision = apply_put_filter(my_node, idx, req.m_filter);
            if (decision == PutFilterDecision::Remove) {
                my_node->remove(idx);
                --end_idx;
                COUNTER_DECREMENT(m_metrics, btree_obj_count, 1);
            } else {
                ++idx;
            }
            ++scanned;
        }

        if (req.m_filter) req.m_filter->mutate_key(req.m_insert_key);

        auto const [found, ins_idx] = my_node->find(req.m_insert_key, nullptr, false);
        if (found) {
            my_node->update(ins_idx, req.m_value);
            req.m_inserted = false;
        } else {
            if (!my_node->has_room_for_put(btree_put_type::UPSERT, req.m_insert_key.serialized_size(),
                                           req.m_value.serialized_size())) {
                ret = btree_status_t::retry;
            } else {
                my_node->insert(ins_idx, req.m_insert_key, req.m_value);
                req.m_inserted = true;
                COUNTER_INCREMENT(m_metrics, btree_obj_count, 1);
            }
        }
    }

    if (ret == btree_status_t::success) {
        if (req.m_route_tracing) { append_route_trace(req, my_node, btree_event_t::MUTATE); }
        write_node(my_node);
    }
    return ret;
}

template < typename K, typename V >
template < typename ReqT >
BtreeTask< btree_status_t > Btree< K, V >::check_split_root(ReqT& req) {
    K split_key;
    btree_status_t ret = btree_status_t::success;

    auto tree_lock = CO_AWAIT(lock_tree_excl());
    auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.id(), LockType::Write));
    if (read_ret != btree_status_t::success) { CO_RETURN read_ret; }

    if (!is_split_needed(root, req)) { CO_RETURN btree_status_t::success; }

    {
        Node new_root = create_interior_node();
        if (!new_root.valid()) { CO_RETURN btree_status_t::space_not_avail; }
        new_root->set_level(root->level() + 1);

        BT_NODE_LOG(DEBUG, root.operator->(), "Root node={} is full, creating new root node={}", root->node_id(),
                    new_root->node_id());

        Node child_node = std::move(root);
        root = std::move(new_root);

        ret = m_underlying->on_root_changed(root);
        if (ret != btree_status_t::success) {
            remove_node(std::move(root));
            CO_RETURN ret;
        }

        ret = split_node(root, child_node, root->total_entries(), &split_key);
        if (ret != btree_status_t::success) {
            remove_node(std::move(root));
            root = std::move(child_node);
            m_underlying->on_root_changed(root); // revert
        } else {
            if (req.m_route_tracing) { append_route_trace(req, child_node, btree_event_t::SPLIT); }
            m_root_node_info = NodeId{root->node_id()};
            COUNTER_INCREMENT(m_metrics, btree_depth, 1);
        }
    }
    CO_RETURN ret;
}

template < typename K, typename V >
btree_status_t Btree< K, V >::split_node(Node const& parent_node, Node const& child_node, uint32_t parent_ind,
                                         K* out_split_key) {
    Node child_node2 = child_node->is_leaf() ? create_leaf_node() : create_interior_node();
    if (!child_node2.valid()) { return btree_status_t::space_not_avail; }

    child_node2->set_next_node(child_node->next_node());
    child_node->set_next_node(child_node2->node_id());
    child_node2->set_level(child_node->level());

    uint32_t child1_filled_size = child_node->node_data_size() - child_node->available_size();
    auto split_size = m_bt_cfg.split_size(child1_filled_size);
    uint32_t res = child_node->move_out_to_right_by_size(*child_node2, split_size);

    BT_NODE_REL_ASSERT_GT(res, 0, child_node.operator->(), "Unable to split entries in the child node");
    BT_NODE_DBG_ASSERT_GT(child_node->total_entries(), 0, child_node.operator->());
    BT_NODE_LOG(TRACE, parent_node.operator->(), "Available space for split entry={}", parent_node->available_size());

    *out_split_key = child_node->get_last_key< K >();

    parent_node->update(parent_ind, child_node2->link_info());
    parent_node->insert(parent_ind, *out_split_key, child_node->link_info());

    BT_NODE_DBG_ASSERT_GT(child_node2->get_first_key< K >().compare(*out_split_key), 0, child_node2.operator->());
    BT_NODE_LOG(DEBUG, parent_node.operator->(), "Split child={} new_child={}, split_key={}", child_node->node_id(),
                child_node2->node_id(), out_split_key->to_string());

    auto ret = write_node(child_node2);
    if (ret != btree_status_t::success) { return ret; }

    ret = write_node(child_node);
    if (ret != btree_status_t::success) { return ret; }

    return write_node(parent_node);
}

template < typename K, typename V >
template < typename ReqT >
bool Btree< K, V >::is_split_needed(Node const& node, ReqT& req) const {
    if (!node->is_leaf()) {
        return !node->has_room_for_put(btree_put_type::UPSERT, K::get_max_size(), NodeId::get_fixed_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > >) {
        return !node->has_room_for_put(req.m_put_type, req.first_key_size(), req.m_newval->serialized_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeSinglePutRequest >) {
        return !node->has_room_for_put(req.m_put_type, req.key().serialized_size(), req.value().serialized_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
        const auto& [k, v] = req.current();
        return !node->has_room_for_put(req.m_put_type, k.serialized_size(), v.serialized_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeScanPutRequest< K > >) {
        return !node->has_room_for_put(btree_put_type::UPSERT, req.m_insert_key.serialized_size(),
                                       req.m_value.serialized_size());
    } else {
        return false;
    }
}

template < typename K, typename V >
PutFilterDecision Btree< K, V >::apply_put_filter(Node const& node, uint32_t idx, PutFilter* filter) const {
    if (!filter) return PutFilterDecision::Replace;

    K key = node->get_nth_key< K >(idx, false);
    auto decision = filter->check_key(key);
    if (decision != PutFilterDecision::NeedOldValue) return decision;

    V old_val;
    node->get_nth_value(idx, &old_val, false);
    return filter->check_kv(key, old_val);
}

} // namespace homestore
