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
BtreeTask< BtreeStatus > Btree< K, V >::remove(ReqT& req) {
    static_assert(std::is_same_v< ReqT, BtreeSingleRemoveRequest > ||
                      std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > > ||
                      std::is_same_v< ReqT, BtreeRemoveAnyRequest< K > >,
                  "remove api is called with non remove request type");

    for (;;) {
        bool need_collapse = false;
        auto op_guard = underlying_->enter_op();
        BtreeStatus ret{BtreeStatus::success};
        {
            auto tree_lock = CO_AWAIT(lock_tree_shared());
            auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, LockType::ReadInteriorWriteLeaf));
            if (!root_result.hasValue()) {
                ret = root_result.error();
                BT_LOG(ERROR, "remove: failed to read root ret={}", ret);
                goto handle;
            }

            auto root = std::move(root_result.value());
            BT_NODE_LOG(TRACE, root, "remove: root entries={} is_leaf={}", root->total_entries(), root->is_leaf());
            if (root->total_entries() == 0) {
                if (root->is_leaf()) {
                    BT_NODE_LOG(TRACE, root, "remove: empty leaf root, nothing to remove");
                    goto handle;
                }
                BT_NODE_LOG_ASSERT_EQ(root->has_valid_edge(), true, root.operator->(),
                                      "Orphaned root with no entries and no edge");
                BT_NODE_LOG(DEBUG, root, "remove: empty interior root, need collapse");
                root.release();
                need_collapse = true;
            } else {
                ret = CO_AWAIT(do_remove(std::move(root), req));
            }
        }

        if (need_collapse) {
            ret = CO_AWAIT(check_collapse_root(req));
            if (ret != BtreeStatus::success && ret != BtreeStatus::merge_not_required) {
                BT_LOG(ERROR, "check_collapse_root failed ret={}", ret);
                goto handle;
            }
            BT_LOG(DEBUG, "remove: root collapsed, retrying");
            continue;
        }

    handle:
        if (ret == BtreeStatus::success || ret == BtreeStatus::key_not_found) {
            CO_RETURN ret;
        } else if (ret == BtreeStatus::retry) {
            COUNTER_INCREMENT(metrics_, btree_retry_count, 1);
            BT_LOG(TRACE, "remove: retrying");
            continue;
        } else {
            BT_LOG(ERROR, "btree remove failed ret={}", ret);
            COUNTER_INCREMENT(metrics_, remove_err_cnt, 1);
            CO_RETURN ret;
        }
    }
}

// do_remove takes my_node by value — RAII owns the lock.
// On interior nodes, child locks are acquired and released by RAII or explicit unlock.
template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::do_remove(Node my_node, ReqT& req) {
    BtreeStatus ret = BtreeStatus::success;

    BT_NODE_LOG(TRACE, my_node, "do_remove: is_leaf={} lock_type={} entries={}", my_node->is_leaf(),
                enum_name(my_node.lock_type()), my_node->total_entries());
    if (my_node->is_leaf()) {
        BT_NODE_DBG_ASSERT_EQ(my_node.lock_type(), LockType::Write, my_node.operator->());

        uint32_t removed_count{0};
        bool modified{false};
#ifndef NDEBUG
        my_node->validate_key_order< K >();
#endif

        if constexpr (std::is_same_v< ReqT, BtreeSingleRemoveRequest >) {
            auto const [found, idx] = my_node->find(req.key());
            BT_NODE_LOG(TRACE, my_node, "do_remove leaf single: key={} found={} idx={}", req.key().to_string(), found,
                        idx);
            if (found) {
                if (req.outval_) {
                    auto status = CO_AWAIT node_ops_.leaf_read_value(my_node, idx, *req.outval_);
                    if (status != BtreeStatus::success) {
                        BT_NODE_LOG(ERROR, my_node, "do_remove leaf: read_from_node failed at idx={}", idx);
                        CO_RETURN status;
                    }
                }
                node_ops_.leaf_remove_kv(my_node, idx);
                ++removed_count;
                modified = true;
            }
        } else if constexpr (std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > >) {
            uint32_t start_idx{0};
            uint32_t end_idx{0};
            if (my_node->match_range(req.working_range(), start_idx, end_idx)) {
                BT_NODE_LOG(TRACE, my_node, "do_remove leaf range: matched [{}-{}]", start_idx, end_idx);

                uint32_t idx = start_idx;
                while (idx <= end_idx) {
                    auto decision = CO_AWAIT apply_remove_filter(my_node, idx, req.filter_);
                    if (decision == RemoveFilterDecision::Remove) {
                        node_ops_.leaf_remove_kv(my_node, idx);
                        if (end_idx == 0) {
                            break;
                        }
                        --end_idx;
                        ++removed_count;
                    } else {
                        ++idx;
                    }
                }
                modified = (removed_count > 0);
            } else {
                BT_NODE_LOG(TRACE, my_node, "do_remove leaf range: no match in working range");
            }
            req.removed_count_ += removed_count;
            req.next_working_range();
        } else if constexpr (std::is_same_v< ReqT, BtreeRemoveAnyRequest< K > >) {
            uint32_t start_idx{0};
            uint32_t end_idx{0};
            if (my_node->match_range(req.range_, start_idx, end_idx)) {
                auto idx = (start_idx + end_idx) / 2; // pick middle
                BT_NODE_LOG(TRACE, my_node, "do_remove leaf any: matched [{}-{}] picking idx={}", start_idx, end_idx,
                            idx);
                if (req.outkey_) {
                    my_node->read_nth_key(idx, *req.outkey_, true);
                }
                if (req.outval_) {
                    auto status = CO_AWAIT node_ops_.leaf_read_value(my_node, idx, *req.outval_);
                    if (status != BtreeStatus::success) {
                        BT_NODE_LOG(ERROR, my_node, "do_remove leaf any: read_from_node failed at idx={}", idx);
                        CO_RETURN status;
                    }
                }
                node_ops_.leaf_remove_kv(my_node, idx);
                ++removed_count;
                modified = true;
            } else {
                BT_NODE_LOG(TRACE, my_node, "do_remove leaf any: no match in range");
            }
        }
#ifndef NDEBUG
        my_node->validate_key_order< K >();
#endif
        if (modified) {
            write_node(my_node);
            COUNTER_DECREMENT(metrics_, btree_obj_count, removed_count);
            BT_NODE_LOG(TRACE, my_node, "do_remove leaf: removed {} entries, remaining={}", removed_count,
                        my_node->total_entries());
            if (req.route_tracing_) {
                append_route_trace(req, my_node, BtreeEvent::REMOVE);
            }
        } else {
            BT_NODE_LOG(TRACE, my_node, "do_remove leaf: nothing removed");
        }
        // Range removes are idempotent: a leaf with no matches in its working range is "already clean", not an error,
        // so the interior loop continues to the next sibling that may still hold matches.
        if constexpr (std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > >) {
            CO_RETURN BtreeStatus::success;
        } else {
            CO_RETURN modified ? BtreeStatus::success : BtreeStatus::key_not_found;
        }
        // RAII: my_node destructor unlocks
    }

    // Interior node: use a retry loop for merge-after-merge scenarios.
    // (goto is not allowed in coroutines when jumping over co_await points.)
retry_inner:
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    uint32_t curr_idx{0};

    // Determine child index range to visit.
    if constexpr (std::is_same_v< ReqT, BtreeSingleRemoveRequest >) {
        auto const [found, idx] = my_node->find(req.key());
        ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node.operator->());
        end_idx = start_idx = idx;
    } else if constexpr (std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > >) {
        auto const matched = my_node->match_range< K >(req.working_range(), start_idx, end_idx);
        if (!matched) {
            BT_NODE_LOG_ASSERT(false, my_node, "working_range()={} did not match node={}",
                               req.working_range().to_string(), my_node->to_string());
            CO_RETURN(BtreeStatus::interior_entry_corrupted);
        }
    } else if constexpr (std::is_same_v< ReqT, BtreeRemoveAnyRequest< K > >) {
        auto const matched = my_node->match_range< K >(req.range_, start_idx, end_idx);
        if (!matched) {
            BT_NODE_LOG_ASSERT(false, my_node, "working_range()={} did not match node={}",
                               req.working_range().to_string(), my_node->to_string());
            CO_RETURN(BtreeStatus::interior_entry_corrupted);
        }
        end_idx = start_idx = (end_idx - start_idx) / 2; // pick middle
    }

    BT_NODE_LOG(TRACE, my_node, "do_remove interior: child range=[{}-{}] entries={}", start_idx, end_idx,
                my_node->total_entries());
    if (req.route_tracing_) {
        append_route_trace(req, my_node, BtreeEvent::READ, start_idx, end_idx);
    }
    curr_idx = start_idx;

    while (curr_idx <= end_idx) {
        auto child_result = CO_AWAIT(get_child_node(my_node, curr_idx, LockType::ReadInteriorWriteLeaf));
        if (!child_result.hasValue()) {
            CO_RETURN(child_result.error());
        }
        auto child = std::move(child_result.value());

        BT_NODE_LOG(TRACE, child, "do_remove: visiting child[{}] entries={} merge_needed={}", curr_idx,
                    child->total_entries(), child->is_merge_needed(bt_cfg_));

        if (child->is_merge_needed(bt_cfg_)) {
            uint32_t node_end_idx = my_node->total_entries();
            if (!my_node->has_valid_edge()) {
                --node_end_idx;
            }
            if (node_end_idx > (curr_idx + bt_cfg_.max_merge_nodes_ - 1)) {
                node_end_idx = curr_idx + bt_cfg_.max_merge_nodes_ - 1;
            }

            BT_NODE_LOG(DEBUG, my_node, "do_remove: merge check child[{}] node_end_idx={}", curr_idx, node_end_idx);

            if (node_end_idx > curr_idx) {
                // Upgrade parent + child to WRITE; on failure, caller must retry.
                ret = CO_AWAIT(upgrade_node_locks(my_node, child));
                if (ret != BtreeStatus::success) {
                    BT_NODE_LOG(DEBUG, my_node, "do_remove: lock upgrade failed ret={}, will retry", ret);
                    CO_RETURN(ret);
                }

                ret = CO_AWAIT(merge_nodes(my_node, child, curr_idx, node_end_idx));
                if ((ret != BtreeStatus::success) && (ret != BtreeStatus::merge_not_required)) {
                    BT_NODE_LOG(ERROR, my_node, "do_remove: merge_nodes failed ret={}", ret);
                    CO_RETURN(ret);
                } else if (ret == BtreeStatus::success) {
                    if (req.route_tracing_) {
                        append_route_trace(req, child, BtreeEvent::MERGE);
                    }
                    BT_NODE_LOG(DEBUG, my_node, "do_remove: merge succeeded, retrying from interior top");
                    goto retry_inner;
                } else {
                    BT_NODE_LOG(DEBUG, my_node, "do_remove: merge not required for child[{}]", curr_idx);
                }
            }
        }

        // Trim working range for range removes at leaf children.
        if constexpr (std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > >) {
            if (child->is_leaf()) {
                if (curr_idx < my_node->total_entries()) {
                    K child_end_key = my_node->get_nth_key< K >(curr_idx, true);
                    if (child_end_key.compare(req.working_range().end_key()) < 0) {
                        req.trim_working_range(std::move(child_end_key), true /* inclusive */);
                    }
                    BT_NODE_LOG(DEBUG, my_node.operator->(), "Subrange:idx=[{}-{}],c={},working={}", start_idx, end_idx,
                                curr_idx, req.working_range().to_string());
                }
            }
        }

#ifndef NDEBUG
        if (child->total_entries()) {
            if (curr_idx != my_node->total_entries()) {
                BT_NODE_DBG_ASSERT_LE(
                    child->template get_last_key< K >().compare(my_node->template get_nth_key< K >(curr_idx, false)), 0,
                    my_node.operator->());
            }
            if (curr_idx > 0) {
                BT_NODE_DBG_ASSERT_GT(
                    child->template get_first_key< K >().compare(my_node->template get_nth_key< K >(curr_idx - 1, false)),
                    0, my_node.operator->());
            }
        }
#endif

        if (curr_idx == end_idx) {
            // Last child: release parent lock now (child lock still held).
            my_node.release();
        }

        ret = CO_AWAIT(do_remove(std::move(child), req));
        if (ret != BtreeStatus::success) {
            break;
        }
        ++curr_idx;
    }

    // RAII: my_node destructor unlocks (no-op if already unlocked via unlock_node above)
    CO_RETURN(ret);
}

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::check_collapse_root(ReqT& req) {
    if (!bt_cfg_.merge_turned_on_) {
        CO_RETURN BtreeStatus::merge_not_required;
    }

    auto tree_lock = CO_AWAIT(lock_tree_excl());
    auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, LockType::Write));
    if (!root_result.hasValue()) {
        CO_RETURN root_result.error();
    }
    auto root = std::move(root_result.value());

    if (root->total_entries() != 0 || root->is_leaf()) {
        // Some other thread already collapsed root.
        CO_RETURN BtreeStatus::success;
    }

    BT_NODE_DBG_ASSERT_EQ(root->has_valid_edge(), true, root.operator->());
    {
        auto child_result = CO_AWAIT(underlying_->read_node(root->edge_id(), LockType::Write));
        if (!child_result.hasValue()) {
            CO_RETURN child_result.error();
        }
        auto child = std::move(child_result.value());

        underlying_->on_root_changed(child);

        if (req.route_tracing_) {
            append_route_trace(req, root, BtreeEvent::MERGE);
        }

        remove_node(std::move(root));
        root_node_id_ = child->node_id();
        COUNTER_DECREMENT(metrics_, btree_depth, 1);
    }

    CO_RETURN BtreeStatus::success;
}

template < typename K, typename V >
BtreeTask< RemoveFilterDecision > Btree< K, V >::apply_remove_filter(Node const& node, uint32_t idx,
                                                                     RemoveFilter* filter) const {
    if (!filter) {
        CO_RETURN RemoveFilterDecision::Remove;
    }

    K key = node->get_nth_key< K >(idx, false);
    auto decision = filter->check_key(key);
    if (decision != RemoveFilterDecision::NeedValue) {
        CO_RETURN decision;
    }

    V old_val;
    auto status = CO_AWAIT node_ops_.leaf_read_value(node, idx, old_val);
    if (status != BtreeStatus::success) {
        BT_LOG(ERROR, "apply_remove_filter: read_from_node failed status={}", status);
        DEBUG_ASSERT(false, "apply_remove_filter: failed to read value for filter");
        CO_RETURN RemoveFilterDecision::Skip;
    }
    CO_RETURN filter->check_kv(key, old_val);
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::merge_nodes(Node const& parent_node, Node const& leftmost_node,
                                                    uint32_t start_idx, uint32_t end_idx) {
    if (!bt_cfg_.merge_turned_on_) {
        CO_RETURN BtreeStatus::merge_not_required;
    }

    BT_NODE_LOG(DEBUG, parent_node, "merge_nodes: start_idx={} end_idx={} leftmost entries={}", start_idx, end_idx,
                leftmost_node->total_entries());

    // ── Phase 1: Read all sibling nodes to the right of leftmost ────────────
    NodeList old_nodes;
    old_nodes.reserve(end_idx - start_idx);
    for (uint32_t idx = start_idx + 1; idx <= end_idx; ++idx) {
        auto child_result = CO_AWAIT(get_child_node(parent_node, idx, LockType::Write));
        if (!child_result.hasValue()) {
            CO_RETURN child_result.error();
        }
        auto child = std::move(child_result.value());
        DEBUG_ASSERT_EQ(child->is_node_deleted(), false);
        BT_NODE_LOG(TRACE, child, "merge_nodes phase1: old_node[{}] entries={} has_edge={}", idx,
                    child->total_entries(), child->has_valid_edge());
        old_nodes.push_back(std::move(child));
    }

    if (old_nodes.empty()) {
        BT_NODE_LOG(DEBUG, parent_node, "merge_nodes: no sibling nodes to merge");
        CO_RETURN BtreeStatus::merge_not_required;
    }

    // ── Phase 2: Pack leftmost + old_nodes greedily.  leftmost itself is the first fill target; any overflow goes into
    // fresh siblings collected in new_nodes.  Before touching leftmost's contents we swap its phys_node_buf_ for a
    // fresh backend-allocated working copy, saving the original in saved_buf for rollback on abort.  No memcpy on the
    // commit path — leftmost simply keeps the working buf; saved_buf drops its refcount.
    auto const node_count = 1 + old_nodes.size();
    NodeList new_nodes; // extras only (does not include leftmost)
    new_nodes.reserve(old_nodes.size());

    auto saved_buf = leftmost_node->phys_node_buf_;
    {
        auto working_buf = underlying_->allocate_node_buf();
        std::memcpy(working_buf.get(), saved_buf.get(), bt_cfg_.node_size());
        leftmost_node->phys_node_buf_ = std::move(working_buf);
    }

    // Current fill target: leftmost until the first overflow, then the most recent new_nodes entry.
    auto cur_node = [&]() -> Node const& { return new_nodes.empty() ? leftmost_node : new_nodes.back(); };

    BT_NODE_LOG(TRACE, parent_node,
                "merge_nodes phase2: leftmost entries={}, packing {} old nodes into ideal_fill={}",
                leftmost_node->total_entries(), old_nodes.size(), bt_cfg_.ideal_fill_size());

    uint32_t src_cursor{0};
    for (size_t oi = 0; oi < old_nodes.size(); ++oi) {
        auto& old_node = old_nodes[oi];
        src_cursor = 0;
        while (src_cursor < old_node->total_entries()) {
            auto before = cur_node()->total_entries();
            cur_node()->append_copy_in_upto_size(*old_node, src_cursor, bt_cfg_.ideal_fill_size());
            BT_NODE_LOG(TRACE, parent_node,
                        "merge_nodes phase2: old[{}] src_cursor={}/{} -> slot[{}] entries {}->{}", oi, src_cursor,
                        old_node->total_entries(), new_nodes.size(), before, cur_node()->total_entries());
            if (src_cursor < old_node->total_entries()) {
                new_nodes.push_back(leftmost_node->is_leaf() ? create_leaf_node() : create_interior_node());
            }
        }
        if (old_node->has_valid_edge()) {
            node_ops_.set_edge_link(cur_node(), NodeLink{old_node->edge_id()});
        }
    }

    // Drop trailing empty extra (leftmost itself is never "dropped" — if it ends up empty+no-edge the all-empty
    // branch in phase 4 handles it via sibling collapse).
    if (!new_nodes.empty() && new_nodes.back()->is_empty()) {
        BT_NODE_LOG(TRACE, parent_node, "merge_nodes phase2: dropping trailing empty new_node");
        remove_node(std::move(new_nodes.back()));
        new_nodes.pop_back();
    }

    BT_NODE_LOG(DEBUG, parent_node, "merge_nodes phase2 done: {} old nodes -> 1+{} slots (was {})", old_nodes.size(),
                new_nodes.size(), node_count);

    // ── Phase 3: Decide whether merging is beneficial ───────────────────────
    auto abort_and_restore = [&]() {
        leftmost_node->phys_node_buf_ = std::move(saved_buf);
        for (auto& n : new_nodes) {
            remove_node(std::move(n));
        }
        new_nodes.clear();
    };

    // If we couldn't copy all entries from the last old node, abort.
    if (src_cursor < old_nodes.back()->total_entries()) {
        BT_NODE_LOG(DEBUG, parent_node,
                    "merge_nodes phase3: couldn't fit last old node (cursor={} < entries={}), aborting", src_cursor,
                    old_nodes.back()->total_entries());
        abort_and_restore();
        CO_RETURN BtreeStatus::merge_not_required;
    }

    // No reduction in slot count — not worth it.  Result slots = 1 (leftmost) + new_nodes.size().
    if (new_nodes.size() + 1 >= node_count) {
        BT_NODE_LOG(DEBUG, parent_node, "merge_nodes phase3: no reduction (1+{} >= {}), aborting", new_nodes.size(),
                    node_count);
        abort_and_restore();
        CO_RETURN BtreeStatus::merge_not_required;
    }

    // ── Phase 4: Commit the merge ───────────────────────────────────────────
    BT_NODE_LOG(TRACE, parent_node, "merge_nodes phase4: committing, 1+{} slots", new_nodes.size());

    // All entries vanished (e.g. range remove wiped whole leaves): leftmost ends up empty+no-edge and no extras got
    // created.  Collapse the siblings onto leftmost and shift the separator at end_idx onto leftmost's entry so parent
    // keeps the old upper bound for this range.  If end_idx is the edge slot, there is nothing to shift.
    if (new_nodes.empty() && leftmost_node->is_empty()) {
        BT_NODE_LOG(DEBUG, parent_node, "merge_nodes: all nodes empty, collapsing {} siblings into leftmost",
                    old_nodes.size());
        if (end_idx < parent_node->total_entries()) {
            K end_key = parent_node->get_nth_key< K >(end_idx, /*copy=*/true);
            node_ops_.remove_children(parent_node, start_idx + 1, start_idx + to_u32(old_nodes.size()));
            node_ops_.update_key(parent_node, start_idx, end_key);
        } else {
            node_ops_.remove_children(parent_node, start_idx + 1, start_idx + to_u32(old_nodes.size()));
        }
        leftmost_node->set_next_node(old_nodes.back()->next_node());
    } else {
        // Capture the old separator at end_idx so the new tail entry inherits it (preserves parent.last_key).
        // If end_idx is the edge slot, there is nothing to capture — edge keeps covering the high end.
        std::optional< K > old_last_key;
        if (end_idx < parent_node->total_entries()) {
            old_last_key = parent_node->get_nth_key< K >(end_idx, true /* copy */);
        }

        auto const extra_new = to_u32(new_nodes.size());
        node_ops_.remove_children(parent_node, start_idx + extra_new + 1, start_idx + to_u32(old_nodes.size()));

        auto next_id = old_nodes.back()->next_node();
        auto parent_idx = start_idx + extra_new;
        for (auto i = new_nodes.size(); i-- > 0;) {
            new_nodes[i]->set_next_node(next_id);
            node_ops_.update_child(parent_node, parent_idx,
                                   old_last_key.has_value() ? *old_last_key : new_nodes[i]->get_last_key< K >(),
                                   NodeLink{new_nodes[i]->node_id()});
            old_last_key.reset();
            next_id = new_nodes[i]->node_id();
            --parent_idx;
        }

        leftmost_node->set_next_node(next_id);
        node_ops_.update_child(parent_node, start_idx,
                               old_last_key.has_value() ? *old_last_key : leftmost_node->get_last_key< K >(),
                               NodeLink{leftmost_node->node_id()});
        old_last_key.reset();

        for (auto& n : new_nodes) {
            write_node(n);
        }
    }

    write_node(leftmost_node);
    write_node(parent_node);
    auto const merged_count = old_nodes.size();
    for (auto& n : old_nodes) {
        remove_node(std::move(n));
    }
    old_nodes.clear();

    BT_NODE_LOG(DEBUG, parent_node, "merge_nodes: merged {} siblings into 1+{} nodes at idx [{}-{}]", merged_count,
                new_nodes.size(), start_idx, end_idx);
    COUNTER_INCREMENT(metrics_, btree_merge_count, 1);
    CO_RETURN BtreeStatus::success;
}

} // namespace homestore