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
#include "homestore/index/btree/btree.h"

namespace homestore {

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::put(ReqT& put_req) {
    static_assert(std::is_same_v< ReqT, BtreeSinglePutRequest > || std::is_same_v< ReqT, BtreeRangePutRequest< K > > ||
                      std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > > ||
                      std::is_same_v< ReqT, BtreeScanPutRequest< K > >,
                  "put api is called with non put request type");
    COUNTER_INCREMENT(metrics_, btree_write_ops_count, 1);

retry:
    auto op_guard = underlying_->enter_op();
    BtreeStatus ret{BtreeStatus::success};
    bool need_split{false};

    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, LockType::ReadInteriorWriteLeaf));
        if (!root_result.hasValue()) {
            ret = root_result.error();
            goto handle;
        }
        auto root = std::move(root_result.value());

        if (is_split_needed(root, put_req)) {
            need_split = true;
        } else {
            ret = CO_AWAIT(do_put(std::move(root), put_req));
        }
    }

    if (need_split) {
        ret = CO_AWAIT(check_split_root(put_req));
        if (ret == BtreeStatus::success) {
            goto retry;
        }
        BT_LOG(ERROR, "Root split failed");
    }

handle:
    if (ret == BtreeStatus::success) {
        CO_RETURN ret;
    } else if (ret == BtreeStatus::retry || ret == BtreeStatus::has_more) {
        // has_more bubbles up only when root is a leaf; interior descent handles it via its own retry.
        // Either way we re-descend so is_split_needed / check_split_root can split the full node.
        COUNTER_INCREMENT(metrics_, btree_retry_count, 1);
        goto retry;
    } else {
        BT_LOG(ERROR, "btree put failed {}", ret);
        COUNTER_INCREMENT(metrics_, write_err_cnt, 1);
        CO_RETURN ret;
    }
}

/*
 * Takes my_node by value — owns the lock. RAII unlocks on return. Expects: node is not full (split check done by
 * caller).
 */
template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::do_put(Node my_node, ReqT& req) {
    BtreeStatus ret = BtreeStatus::success;

    if (my_node->is_leaf()) {
        // RAII: my_node unlocked when function returns
        CO_RETURN CO_AWAIT(mutate_write_leaf_node(std::move(my_node), req));
    }

retry:
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    uint32_t curr_idx;

    if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > > ||
                  std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
        const auto matched = my_node->match_range(req.working_range(), start_idx, end_idx);
        if (!matched) {
            BT_NODE_LOG_ASSERT(false, my_node, "working_range()={} did not match node={}",
                               req.working_range().to_string(), my_node->to_string());
            ret = BtreeStatus::interior_entry_corrupted;
            goto out;
        }
    } else if constexpr (std::is_same_v< ReqT, BtreeSinglePutRequest > ||
                         std::is_same_v< ReqT, BtreeScanPutRequest< K > >) {
        auto const [found, idx] = my_node->find(req.key());
        ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node);
        end_idx = start_idx = idx;
    }

    if (req.route_tracing_) {
        append_route_trace(req, my_node, BtreeEvent::READ, start_idx, end_idx);
    }

    curr_idx = start_idx;
    while (curr_idx <= end_idx) {
        auto child_result = CO_AWAIT(get_child_node(my_node, curr_idx, LockType::ReadInteriorWriteLeaf));
        if (!child_result.hasValue()) {
            ret = child_result.error();
            goto out;
        }
        auto child_node = std::move(child_result.value());

        if (is_split_needed(child_node, req)) {
            ret = CO_AWAIT(upgrade_node_locks(my_node, child_node));
            if (ret != BtreeStatus::success) {
                BT_NODE_LOG(DEBUG, my_node.operator->(), "Upgrade of node lock failed, retrying from root");
                goto out;
            }

            K split_key;
            BT_NODE_LOG(TRACE, my_node.operator->(), "Split node needed");
            ret = split_node(my_node, child_node, curr_idx, &split_key);
            if (ret != BtreeStatus::success) {
                // child_node goes out of scope here → RAII unlocks
                goto out;
            }

            if (req.route_tracing_) {
                append_route_trace(req, child_node, BtreeEvent::SPLIT);
            }
            COUNTER_INCREMENT(metrics_, btree_split_count, 1);
            goto retry;
        }

        if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > > ||
                      std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
            if (child_node->is_leaf() && (curr_idx < my_node->total_entries())) {
                K child_end_key = my_node->get_nth_key< K >(curr_idx, true);
                if (child_end_key.compare(req.working_range().end_key()) < 0) {
                    req.trim_working_range(std::move(child_end_key), true);
                }
                BT_NODE_LOG(DEBUG, my_node.operator->(), "Subrange:idx=[{}-{}],c={},working={}", start_idx, end_idx,
                            curr_idx, req.working_range().to_string());
            }
        }

#ifndef NDEBUG
        K ckey, pkey;
        if (curr_idx != my_node->total_entries()) {
            pkey = my_node->get_nth_key< K >(curr_idx, true);
            if (child_node->total_entries() != 0) {
                ckey = child_node->template get_last_key< K >();
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
                ckey = child_node->template get_first_key< K >();
                BT_NODE_DBG_ASSERT_GE(ckey.compare(pkey), 0, child_node.operator->());
            }
        }
#endif

        if (curr_idx == end_idx) {
            // Unlock parent before descending into last child — no longer needed for range decisions.
            my_node.release();
        }

        ret = CO_AWAIT(do_put(std::move(child_node), req));
        if (ret == BtreeStatus::has_more) {
            // Child leaf couldn't fit the rest of the range and shifted working_range to where it stopped.
            // Re-descend from this node — is_split_needed will split the full leaf before we enter it again.
            goto retry;
        }
        if (ret != BtreeStatus::success) {
            goto out;
        }

        ++curr_idx;
    }

out:
    // If my_node is still locked (wasn't unlocked in the last-child branch), RAII handles it.
    CO_RETURN ret;
}

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::mutate_write_leaf_node(Node const& my_node, ReqT& req) {
    BtreeStatus ret = BtreeStatus::success;

    if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > >) {
        ret = CO_AWAIT(put_range_in_leaf(my_node, req));
    } else if constexpr (std::is_same_v< ReqT, BtreeSinglePutRequest >) {
        ret = CO_AWAIT(put_one_in_leaf(my_node, req));
    } else if constexpr (std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
        ret = CO_AWAIT(put_batch_in_leaf(my_node, req));
    } else if constexpr (std::is_same_v< ReqT, BtreeScanPutRequest< K > >) {
        ret = CO_AWAIT(put_scan_in_leaf(my_node, req));
    }

    if (ret == BtreeStatus::success) {
        if (req.route_tracing_) {
            append_route_trace(req, my_node, BtreeEvent::MUTATE);
        }
        write_node(my_node);
    }
    CO_RETURN ret;
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::put_one_in_leaf(Node const& node, BtreeSinglePutRequest& req) {
    auto const [found, idx] = node->find(req.key());
    auto decision = PutFilterDecision::Replace;

    if (found) {
        if (req.put_type_ == BtreePutType::INSERT) {
            CO_RETURN BtreeStatus::key_already_exists;
        }

        if (req.existing_val_) {
            auto status = CO_AWAIT node_ops_.leaf_read_value(node, idx, *req.existing_val_);
            if (status != BtreeStatus::success) {
                CO_RETURN status;
            }
        }

        decision = CO_AWAIT apply_put_filter(node, idx, req.filter_);
        if (decision == PutFilterDecision::Keep) {
            CO_RETURN BtreeStatus::success;
        } else if (decision == PutFilterDecision::Remove) {
            node_ops_.leaf_remove_kv(node, idx);
            COUNTER_DECREMENT(metrics_, btree_obj_count, 1);
            ++req.stats_.removed;
            CO_RETURN BtreeStatus::success;
        }
    } else if (req.put_type_ == BtreePutType::UPDATE) {
        CO_RETURN BtreeStatus::key_not_found;
    }

    auto const& val = (decision == PutFilterDecision::ReplaceWith) ? *req.filter_->replacement_value() : req.value();
    if (found) {
        auto ret = CO_AWAIT node_ops_.leaf_update_value(node, idx, val);
        if (ret != BtreeStatus::success) {
            CO_RETURN ret;
        }
        ++req.stats_.updated;
    } else {
        auto ret = CO_AWAIT node_ops_.leaf_insert_kv(node, idx, req.key(), val);
        if (ret != BtreeStatus::success) {
            CO_RETURN ret;
        }
        ++req.stats_.inserted;
        COUNTER_INCREMENT(metrics_, btree_obj_count, 1);
    }
    CO_RETURN BtreeStatus::success;
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::put_range_in_leaf(Node const& node, BtreeRangePutRequest< K >& req) {
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    if (!node->match_range(req.working_range(), start_idx, end_idx)) {
        CO_RETURN BtreeStatus::key_not_found;
    }

    K last_failed_key;
    bool has_more{false};
    auto const new_val_size = req.newval_->serialized_size();

    uint32_t idx = start_idx;
    while (idx <= end_idx) {
        if (!node_ops_.has_room(node, req.put_type_, node->get_nth_key_size(idx), new_val_size)) {
            node->read_nth_key(idx, last_failed_key, true);
            has_more = true;
            break;
        }

        auto decision = CO_AWAIT apply_put_filter(node, idx, req.filter_);
        switch (decision) {
        case PutFilterDecision::Keep:
            ++idx;
            break;
        case PutFilterDecision::ReplaceWith:
        case PutFilterDecision::Replace: {
            auto const& val =
                (decision == PutFilterDecision::ReplaceWith) ? *req.filter_->replacement_value() : *req.newval_;
            auto ret = CO_AWAIT node_ops_.leaf_update_value(node, idx, val);
            if (ret != BtreeStatus::success) {
                node->read_nth_key(idx, last_failed_key, true);
                has_more = true;
                goto done;
            }
            ++req.stats_.updated;
            ++idx;
            break;
        }
        case PutFilterDecision::Remove:
            node_ops_.leaf_remove_kv(node, idx);
            --end_idx;
            ++req.stats_.removed;
            COUNTER_DECREMENT(metrics_, btree_obj_count, 1);
            break;
        default:
            ++idx;
            break;
        }
    }

done:
    if (has_more) {
        // When has_more is set, the leaf ran out of room mid-range — tell the caller so it can split the leaf
        // and re-descend for the remaining working_range.
        req.next_working_range_from(std::move(last_failed_key), true);
        CO_RETURN BtreeStatus::has_more;
    } else {
        req.next_working_range();
        CO_RETURN BtreeStatus::success;
    }
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::put_batch_in_leaf(Node const& node, BtreeBatchPutRequest< K, V >& req) {
    while (!req.done()) {
        auto const& [key, val] = req.current();
        if (key.compare(req.end_key_) > 0) {
            break;
        }

        auto const [found, idx] = node->find(key);
        if (found) {
            if (req.put_type_ == BtreePutType::INSERT) {
                req.advance();
                continue;
            }

            auto decision = CO_AWAIT apply_put_filter(node, idx, req.filter_);
            if (decision == PutFilterDecision::Keep) {
                req.advance();
                continue;
            } else if (decision == PutFilterDecision::Remove) {
                node_ops_.leaf_remove_kv(node, idx);
                COUNTER_DECREMENT(metrics_, btree_obj_count, 1);
                ++req.stats_.removed;
                req.advance();
                continue;
            }

            auto const& update_val =
                (decision == PutFilterDecision::ReplaceWith) ? *req.filter_->replacement_value() : val;
            auto ret = CO_AWAIT node_ops_.leaf_update_value(node, idx, update_val);
            if (ret != BtreeStatus::success) {
                CO_RETURN ret;
            }
            ++req.stats_.updated;
        } else {
            if (req.put_type_ == BtreePutType::UPDATE) {
                req.advance();
                continue;
            }
            if (!node_ops_.has_room(node, req.put_type_, key.serialized_size(), val.serialized_size())) {
                CO_RETURN BtreeStatus::retry;
            }
            auto ret = CO_AWAIT node_ops_.leaf_insert_kv(node, idx, key, val);
            if (ret != BtreeStatus::success) {
                CO_RETURN ret;
            }
            ++req.stats_.inserted;
            COUNTER_INCREMENT(metrics_, btree_obj_count, 1);
        }
        req.advance();
    }

    req.next_working_range();
    CO_RETURN BtreeStatus::success;
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::put_scan_in_leaf(Node const& node, BtreeScanPutRequest< K >& req) {
    // Step 1: Scan entries in scan_range, apply filter to each — remove eligible old entries.
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    auto matched = node->match_range(req.scan_range_, start_idx, end_idx);

    if (matched) {
        size_t scanned = 0;
        uint32_t idx = start_idx;
        while (idx <= end_idx && scanned < req.max_scan_) {
            auto decision = CO_AWAIT apply_put_filter(node, idx, req.filter_);
            if (decision == PutFilterDecision::Remove) {
                node_ops_.leaf_remove_kv(node, idx);
                if (end_idx == 0) {
                    break;
                }
                --end_idx;
                COUNTER_DECREMENT(metrics_, btree_obj_count, 1);
            } else {
                ++idx;
            }
            ++scanned;
        }
    }

    // Step 2: Stamp the insert key via filter.
    if (req.filter_) {
        req.filter_->mutate_key(req.insert_key_);
    }

    // Step 3: Insert the stamped key.
    auto const [found, ins_idx] = node->find(req.insert_key_);
    BtreeStatus ret;
    if (found) {
        ret = CO_AWAIT node_ops_.leaf_update_value(node, ins_idx, req.value_);
        if (ret == BtreeStatus::success) {
            req.inserted_ = false;
        }
    } else {
        if (!node_ops_.has_room(node, BtreePutType::UPSERT, req.insert_key_.serialized_size(),
                                req.value_.serialized_size())) {
            ret = BtreeStatus::node_full;
        } else {
            ret = CO_AWAIT node_ops_.leaf_insert_kv(node, ins_idx, req.insert_key_, req.value_);
            if (ret == BtreeStatus::success) {
                req.inserted_ = true;
                COUNTER_INCREMENT(metrics_, btree_obj_count, 1);
            }
        }
    }

    CO_RETURN ret;
}

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::check_split_root(ReqT& req) {
    K split_key;
    BtreeStatus ret = BtreeStatus::success;

    auto tree_lock = CO_AWAIT(lock_tree_excl());
    auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, LockType::Write));
    if (!root_result.hasValue()) {
        CO_RETURN root_result.error();
    }
    auto root = std::move(root_result.value());

    if (!is_split_needed(root, req)) {
        CO_RETURN BtreeStatus::success;
    }

    {
        Node new_root = create_interior_node();
        new_root->set_level(root->level() + 1);

        BT_NODE_LOG(DEBUG, root, "Root is full, creating new root node={}", new_root->node_id());

        Node child_node = std::move(root);
        root = std::move(new_root);

        underlying_->on_root_changed(root);

        ret = split_node(root, child_node, root->total_entries(), &split_key);
        if (ret != BtreeStatus::success) {
            remove_node(std::move(root));
            root = std::move(child_node);
            underlying_->on_root_changed(root);
        } else {
            if (req.route_tracing_) {
                append_route_trace(req, child_node, BtreeEvent::SPLIT);
            }
            root_node_id_ = root->node_id();
            COUNTER_INCREMENT(metrics_, btree_depth, 1);
        }
    }
    CO_RETURN ret;
}

template < typename K, typename V >
BtreeStatus Btree< K, V >::split_node(Node const& parent_node, Node const& child_node, uint32_t parent_ind,
                                      K* out_split_key) {
    Node child_node2 = child_node->is_leaf() ? create_leaf_node() : create_interior_node();

    child_node2->set_next_node(child_node->next_node());
    child_node->set_next_node(child_node2->node_id());
    child_node2->set_level(child_node->level());

    uint32_t child1_filled_size = child_node->node_data_size() - child_node->available_size();
    auto split_size = bt_cfg_.split_size(child1_filled_size);
    uint32_t res = child_node->move_out_to_right_by_size(*child_node2, split_size);

    BT_NODE_REL_ASSERT_GT(res, 0, child_node.operator->(), "Unable to split entries in the child node");
    BT_NODE_DBG_ASSERT_GT(child_node->total_entries(), 0, child_node.operator->());
    BT_NODE_LOG(TRACE, parent_node.operator->(), "Available space for split entry={}", parent_node->available_size());

    *out_split_key = child_node->get_last_key< K >();

    node_ops_.update_child(parent_node, parent_ind, child_node2->link_info());
    node_ops_.insert_child(parent_node, parent_ind, *out_split_key, child_node->link_info());

    BT_NODE_DBG_ASSERT_GT(child_node2->get_first_key< K >().compare(*out_split_key), 0, child_node2.operator->());
    BT_NODE_LOG(DEBUG, parent_node.operator->(), "Split child={} new_child={}, split_key={}", child_node->node_id(),
                child_node2->node_id(), out_split_key->to_string());

    write_node(child_node2);
    write_node(child_node);
    write_node(parent_node);
    return BtreeStatus::success;
}

template < typename K, typename V >
template < typename ReqT >
bool Btree< K, V >::is_split_needed(Node const& node, ReqT& req) const {
    if (!node->is_leaf()) {
        return !node_ops_.has_room(node, BtreePutType::UPSERT, K::get_max_size(), NodeLink::get_fixed_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeRangePutRequest< K > >) {
        return !node_ops_.has_room(node, req.put_type_, req.first_key_size(), req.newval_->serialized_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeSinglePutRequest >) {
        return !node_ops_.has_room(node, req.put_type_, req.key().serialized_size(), req.value().serialized_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeBatchPutRequest< K, V > >) {
        const auto& [k, v] = req.current();
        return !node_ops_.has_room(node, req.put_type_, k.serialized_size(), v.serialized_size());
    } else if constexpr (std::is_same_v< ReqT, BtreeScanPutRequest< K > >) {
        return !node_ops_.has_room(node, BtreePutType::UPSERT, req.insert_key_.serialized_size(),
                                   req.value_.serialized_size());
    } else {
        return false;
    }
}

// Apply put filter, resolving overflow for the old value if the filter needs it.
template < typename K, typename V >
BtreeTask< PutFilterDecision > Btree< K, V >::apply_put_filter(Node const& node, uint32_t idx,
                                                               PutFilter* filter) const {
    if (!filter) {
        CO_RETURN PutFilterDecision::Replace;
    }

    K key = node->get_nth_key< K >(idx, false);
    auto decision = filter->check_key(key);
    if (decision != PutFilterDecision::NeedOldValue) {
        CO_RETURN decision;
    }

    V old_val;
    auto status = CO_AWAIT node_ops_.leaf_read_value(node, idx, old_val);
    if (status != BtreeStatus::success) {
        BT_LOG(ERROR, "apply_put_filter: read_from_node failed status={}", status);
        DEBUG_ASSERT(false, "apply_put_filter: failed to read value for filter");
        CO_RETURN PutFilterDecision::Keep;
    }
    CO_RETURN filter->check_kv(key, old_val);
}

} // namespace homestore
