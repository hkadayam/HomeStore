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

    LockType acq_lock = LockType::Read;

    for (;;) {
        BtreeStatus ret = BtreeStatus::success;
        bool need_collapse = false;

        {
            auto tree_lock = CO_AWAIT(lock_tree_shared());

            auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.bnode_id(), LockType::ReadInteriorWriteLeaf));
            if (read_ret != BtreeStatus::success) {
                if (read_ret == BtreeStatus::retry) {
                    continue;
                }
                CO_RETURN read_ret;
            }

            if (root->total_entries() == 0) {
                if (root->is_leaf()) {
                    CO_RETURN BtreeStatus::not_found;
                }

                BT_NODE_LOG_ASSERT_EQ(root->has_valid_edge(), true, root.operator->(),
                                      "Orphaned root with no entries and no edge");
                unlock_node(root);
                need_collapse = true;
                // tree_lock released at end of this block before check_collapse_root acquires excl
            } else {
                ret = CO_AWAIT(do_remove(std::move(root), req));
                if (ret == BtreeStatus::retry) {
                    continue;
                }
                CO_RETURN ret;
            }
        } // tree_lock released here

        if (need_collapse) {
            ret = CO_AWAIT(check_collapse_root(req));
            if (ret != BtreeStatus::success && ret != BtreeStatus::merge_not_required && ret != BtreeStatus::retry) {
                LOGERROR("check collapse read failed btree name {}", m_bt_cfg.name());
                CO_RETURN ret;
            }
            // Continue loop: retry from new root with shared lock
        }
    }
}

// do_remove takes my_node by value — RAII owns the lock.
// On interior nodes, child locks are acquired and released by RAII or explicit unlock.
template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::do_remove(Node my_node, ReqT& req) {
    BtreeStatus ret = BtreeStatus::success;
    bool at_least_one_child_modified{false};

    if (my_node->is_leaf()) {
        BT_NODE_DBG_ASSERT_EQ(my_node.lock_type(), LockType::Write, my_node.operator->());

        uint32_t removed_count{0};
        bool modified{false};
#ifndef NDEBUG
        my_node->validate_key_order< K >();
#endif

        if constexpr (std::is_same_v< ReqT, BtreeSingleRemoveRequest >) {
            if ((modified = my_node->remove_one(req.key(), nullptr, req.m_outval))) {
                ++removed_count;
            }
        } else if constexpr (std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > >) {
            remove_filter_cb_t cb = nullptr;
            if (req.m_filter) {
                cb = [f = req.m_filter](BtreeKey const& k, BtreeValue const& v) {
                    return (f->check_kv(k, v) == RemoveFilterDecision::Remove);
                };
            }
            removed_count = to_variant_node(my_node)->multi_remove(req.working_range(), cb);
            modified = (removed_count != 0);
            req.shift_working_range();
        } else if constexpr (std::is_same_v< ReqT, BtreeRemoveAnyRequest< K > >) {
            if ((modified = my_node->remove_any(req.m_range, req.m_outkey, req.m_outval))) {
                ++removed_count;
            }
        }
#ifndef NDEBUG
        my_node->validate_key_order< K >();
#endif
        if (modified) {
            write_node(my_node);
            COUNTER_DECREMENT(m_metrics, btree_obj_count, removed_count);
            if (req.m_route_tracing) {
                append_route_trace(req, my_node, BtreeEvent::REMOVE);
            }
        }
        CO_RETURN modified ? BtreeStatus::success : BtreeStatus::not_found;
        // RAII: my_node destructor unlocks
    }

    // Interior node: use a retry loop for merge-after-merge scenarios.
    // (goto is not allowed in coroutines when jumping over co_await points.)
    bool need_retry_inner = false;
    do {
        need_retry_inner = false;
        uint32_t start_idx{0};
        uint32_t end_idx{0};
        uint32_t curr_idx{0};

        // Determine child index range to visit.
        if constexpr (std::is_same_v< ReqT, BtreeSingleRemoveRequest >) {
            auto const [found, idx] = my_node->find(req.key(), nullptr, false);
            ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node.operator->());
            end_idx = start_idx = idx;
        } else if constexpr (std::is_same_v< ReqT, BtreeRangeRemoveRequest< K > >) {
            auto const matched = my_node->match_range< K >(req.working_range(), start_idx, end_idx);
            if (!matched) {
                CO_RETURN(at_least_one_child_modified ? BtreeStatus::success : BtreeStatus::not_found);
            }
        } else if constexpr (std::is_same_v< ReqT, BtreeRemoveAnyRequest< K > >) {
            auto const matched = my_node->match_range< K >(req.m_range, start_idx, end_idx);
            if (!matched) {
                CO_RETURN(at_least_one_child_modified ? BtreeStatus::success : BtreeStatus::not_found);
            }
            end_idx = start_idx = (end_idx - start_idx) / 2; // pick middle
        }

        if (req.m_route_tracing) {
            append_route_trace(req, my_node, BtreeEvent::READ, start_idx, end_idx);
        }
        curr_idx = start_idx;

        while (curr_idx <= end_idx) {
            NodeLink child_id;
            auto [child_ret, child] = CO_AWAIT(get_child_node(my_node, curr_idx, child_id, LockType::Read));
            if (child_ret != BtreeStatus::success) {
                CO_RETURN(at_least_one_child_modified ? BtreeStatus::success : child_ret);
            }

            if (child->is_merge_needed(m_bt_cfg)) {
                uint32_t node_end_idx = my_node->total_entries();
                if (!my_node->has_valid_edge()) {
                    --node_end_idx;
                }
                if (node_end_idx > (curr_idx + m_bt_cfg.m_max_merge_nodes - 1)) {
                    node_end_idx = curr_idx + m_bt_cfg.m_max_merge_nodes - 1;
                }

                if (node_end_idx > curr_idx) {
                    // Upgrade parent + child to WRITE; on failure, caller must retry.
                    ret = CO_AWAIT(upgrade_node_locks(my_node, child));
                    if (ret != BtreeStatus::success) {
                        CO_RETURN(at_least_one_child_modified ? BtreeStatus::success : ret);
                    }

                    ret = CO_AWAIT(merge_nodes(my_node, child, curr_idx, node_end_idx));
                    if ((ret != BtreeStatus::success) && (ret != BtreeStatus::merge_not_required)) {
                        CO_RETURN(at_least_one_child_modified ? BtreeStatus::success : ret);
                    } else if (ret == BtreeStatus::success) {
                        if (req.m_route_tracing) {
                            append_route_trace(req, child, BtreeEvent::MERGE);
                        }
                        COUNTER_INCREMENT(m_metrics, btree_merge_count, 1);
                        need_retry_inner = true;
                        break; // exit while loop, retry the do-while
                    } else {
                        BT_NODE_LOG(DEBUG, my_node.operator->(), "merge is not required for child = {} keys: {}",
                                    curr_idx, child->to_string());
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
                        BT_NODE_LOG(DEBUG, my_node.operator->(), "Subrange:idx=[{}-{}],c={},working={}", start_idx,
                                    end_idx, curr_idx, req.working_range().to_string());
                    }
                }
            }

#ifndef NDEBUG
            if (child->total_entries()) {
                if (curr_idx != my_node->total_entries()) {
                    BT_NODE_DBG_ASSERT_LE(
                        child->get_last_key< K >().compare(my_node->get_nth_key< K >(curr_idx, false)), 0,
                        my_node.operator->());
                }
                if (curr_idx > 0) {
                    BT_NODE_DBG_ASSERT_GT(
                        child->get_first_key< K >().compare(my_node->get_nth_key< K >(curr_idx - 1, false)), 0,
                        my_node.operator->());
                }
            }
#endif

            if (curr_idx == end_idx) {
                // Last child: release parent lock now (child lock still held).
                unlock_node(my_node);
            }

            ret = CO_AWAIT(do_remove(std::move(child), req));
            if (ret == BtreeStatus::success) {
                at_least_one_child_modified = true;
            }
            ++curr_idx;
        }
    } while (need_retry_inner);

    // RAII: my_node destructor unlocks (no-op if already unlocked via unlock_node above)
    CO_RETURN(at_least_one_child_modified ? BtreeStatus::success : ret);
}

template < typename K, typename V >
template < typename ReqT >
BtreeTask< BtreeStatus > Btree< K, V >::check_collapse_root(ReqT& req) {
    if (!m_bt_cfg.m_merge_turned_on) {
        CO_RETURN BtreeStatus::merge_not_required;
    }
    BtreeStatus ret = BtreeStatus::success;

    auto tree_lock = CO_AWAIT(lock_tree_excl());

    auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.bnode_id(), LockType::Write));
    if (read_ret != BtreeStatus::success) {
        CO_RETURN read_ret;
    }

    if (root->total_entries() != 0 || root->is_leaf()) {
        // Some other thread already collapsed root.
        CO_RETURN BtreeStatus::success; // RAII: root + tree_lock released
    }

    BT_NODE_DBG_ASSERT_EQ(root->has_valid_edge(), true, root.operator->());
    {
        auto [child_ret, child] = CO_AWAIT(read_node(root->edge_id(), LockType::Write));
        if (child_ret != BtreeStatus::success) {
            CO_RETURN child_ret; // root RAII at function exit
        }

        ret = m_underlying->on_root_changed(child);
        if (ret != BtreeStatus::success) {
            CO_RETURN ret; // child + root RAII at block/function exit
        }

        if (req.m_route_tracing) {
            append_route_trace(req, root, BtreeEvent::MERGE);
        }

        remove_node(std::move(root)); // unlock + remove; RAII is now no-op
        m_root_node_info = child->link_info();
        COUNTER_DECREMENT(m_metrics, btree_depth, 1);
        // RAII: child unlocks when inner block exits
    }

    CO_RETURN ret;
    // RAII: tree_lock released; root destructor no-op (was moved)
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::merge_nodes(Node const& parent_node, Node const& leftmost_node,
                                                    uint32_t start_idx, uint32_t end_idx) {
    if (!m_bt_cfg.m_merge_turned_on) {
        CO_RETURN BtreeStatus::merge_not_required;
    }

    BtreeStatus ret{BtreeStatus::success};
    NodeList old_nodes;
    NodeList new_nodes;
    old_nodes.reserve(end_idx - start_idx + 1);
    new_nodes.reserve(end_idx - start_idx + 1);

    // Erase last element from list. If node_removal==true, backend-removes it before popping.
    auto erase_last_node = [this](NodeList& list, bool node_removal) {
        if (node_removal) {
            remove_node(std::move(list.back()));
        }
        list.pop_back(); // RAII: destructor unlocks if not already removed
    };

    // Clone leftmost_node as the initial staging destination.
    Node cloned_new_node = clone_temp_node(*leftmost_node);

    // Loop state: non-owning pointers into old_nodes / new_nodes (stable after reserve).
    Node* new_node_ptr = &cloned_new_node; // initial destination = clone of leftmost
    Node* old_node_ptr = nullptr;          // null = need to read next old node
    uint32_t src_cursor = 0;
    bnodeid_t next_node_id;

    uint32_t idx = start_idx + 1;
    while (idx <= end_idx) {
        if (old_node_ptr == nullptr) {
            // Read next old node.
            if (idx == parent_node->total_entries()) {
                BT_NODE_LOG_ASSERT(parent_node->has_valid_edge(), parent_node.operator->(),
                                   "Assertion failure, expected valid edge for parent node");
            }
            BtreeLinkInfo child_info;
            parent_node->get_nth_value(idx, &child_info, false /* copy */);

            auto [child_ret, child] = CO_AWAIT(read_node(child_info.bnode_id(), LockType::Write));
            if (child_ret != BtreeStatus::success) {
                ret = child_ret;
                goto out;
            }
            BT_NODE_LOG_ASSERT_EQ(child->is_node_deleted(), false, child.operator->());

            old_nodes.push_back(std::move(child));
            old_node_ptr = &old_nodes.back();
            src_cursor = 0;
        }

        if (new_node_ptr == nullptr) {
            Node new_node = leftmost_node->is_leaf() ? create_leaf_node() : create_interior_node();
            new_nodes.push_back(std::move(new_node));
            new_node_ptr = &new_nodes.back();
        }

        if (idx == end_idx) {
            // Try to fit the last old node entirely into new_node_ptr.
            auto const copied = new_node_ptr->operator->()->append_copy_in_upto_size(
                *old_node_ptr->operator->(), src_cursor, m_bt_cfg.ideal_fill_size(), /*copy_only_if_fits=*/true);
            if (!copied) {
                if (new_node_ptr->operator->()->total_entries() == 0) {
                    erase_last_node(new_nodes, /*node_removal=*/true);
                    new_node_ptr = nullptr;
                }
                erase_last_node(old_nodes, /*node_removal=*/false);
                old_node_ptr = nullptr;
            }
            break;
        } else {
            new_node_ptr->operator->()->append_copy_in_upto_size(
                *old_node_ptr->operator->(), src_cursor, m_bt_cfg.ideal_fill_size(), /*copy_only_if_fits=*/false);
            if (src_cursor == old_node_ptr->operator->()->total_entries()) {
                // Entire old_node consumed; advance to next.
                old_node_ptr = nullptr;
                ++idx;
            } else {
                // new_node_ptr is full; need a new destination.
                new_node_ptr = nullptr;
            }
        }
    }

    // Commit only if we actually reduced node count.
    if (new_nodes.size() >= old_nodes.size()) {
        ret = BtreeStatus::merge_not_required;
        goto out;
    }

    // Remove excess parent entries.
    parent_node->remove(start_idx + new_nodes.size() + 1, start_idx + old_nodes.size());

    // Update parent entries and node links (reverse order so next_bnode is set first).
    idx = start_idx + new_nodes.size();
    next_node_id = old_nodes.back()->next_node();
    for (auto it = new_nodes.rbegin(); it != new_nodes.rend(); ++it) {
        (*it)->set_next_node(next_node_id);
        auto this_node_id = (*it)->node_id();
        if ((*it)->total_entries()) {
            parent_node->update(idx--, (*it)->get_last_key< K >(), BtreeLinkInfo{this_node_id, 0});
        }
        next_node_id = this_node_id;
    }

    // Copy staged clone back into leftmost_node in-place.
    leftmost_node->overwrite(*cloned_new_node);
    leftmost_node->set_next_node(next_node_id);
    if (leftmost_node->total_entries()) {
        parent_node->update(start_idx, leftmost_node->get_last_key< K >(), leftmost_node->link_info());
    }

    for (auto const& node : new_nodes) {
        write_node(node);
    }
    write_node(leftmost_node);
    write_node(parent_node);
    ret = BtreeStatus::success;

    if (ret == BtreeStatus::success) {
        // Explicitly remove old nodes (backend marks storage for reuse).
        for (auto& node : old_nodes) {
            remove_node(std::move(node));
        }
        old_nodes.clear(); // moved-from; RAII destructors are no-ops
        // new_nodes: RAII unlocks when NodeList goes out of scope (they're live in tree)
    }

out:
    // Free the temp clone (NONE-locked; remove_node just frees storage).
    remove_node(std::move(cloned_new_node));

    if (ret != BtreeStatus::success) {
        // new_nodes: were allocated but never committed — explicitly remove.
        for (auto& node : new_nodes) {
            remove_node(std::move(node));
        }
        new_nodes.clear();
        // old_nodes: still valid in tree — RAII unlocks when NodeList goes out of scope.
    }
    CO_RETURN ret;
}

} // namespace homestore