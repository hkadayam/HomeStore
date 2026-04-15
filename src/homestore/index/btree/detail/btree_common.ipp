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
#include <homestore/index/btree/btree_store.h>

namespace homestore {

// to_variant_node removed — multi_get/get_any/multi_put/multi_remove are now template methods on NodeCore

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::post_order_traversal(LockType ltype, const auto& cb) {
    // Read and write locks have different guard types; each path is explicit.
    BtreeStatus ret{BtreeStatus::success};
    if (ltype == LockType::Write) {
        auto tree_lock = CO_AWAIT(lock_tree_excl());
        if (m_root_node_info.bnode_id() != empty_bnodeid) {
            auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.bnode_id(), ltype));
            if (read_ret != BtreeStatus::success) {
                CO_RETURN read_ret;
            }
            ret = CO_AWAIT(post_order_traversal(std::move(root), ltype, cb));
            if (ret == BtreeStatus::node_freed) {
                ret = BtreeStatus::success;
            }
        }
    } else {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        if (m_root_node_info.bnode_id() != empty_bnodeid) {
            auto [read_ret, root] = CO_AWAIT(read_node(m_root_node_info.bnode_id(), ltype));
            if (read_ret != BtreeStatus::success) {
                CO_RETURN read_ret;
            }
            ret = CO_AWAIT(post_order_traversal(std::move(root), ltype, cb));
            if (ret == BtreeStatus::node_freed) {
                ret = BtreeStatus::success;
            }
        }
    }
    CO_RETURN ret;
}

// Takes node by value — owns the lock. RAII unlocks when function returns (or callback freed the node).
template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::post_order_traversal(Node node, LockType ltype, const auto& cb) {
    BtreeStatus ret = BtreeStatus::success;

    if (!node->is_leaf()) {
        uint32_t i{0};
        BtreeLinkInfo child_info;
        while (i <= node->total_entries()) {
            if (i == node->total_entries()) {
                if (!node->has_valid_edge()) {
                    break;
                }
                child_info.set_bnode_id(node->edge_id());
            } else {
                node->get_nth_value(i, &child_info, false /* copy */);
            }

            auto [child_ret, child] = CO_AWAIT(read_node(child_info.bnode_id(), ltype));
            if (child_ret != BtreeStatus::success) {
                CO_RETURN child_ret;
            }

            ret = CO_AWAIT(post_order_traversal(std::move(child), ltype, cb));
            // child is moved-from; RAII in the recursive call handles unlock/free.
            ++i;
        }
        CO_RETURN cb(std::move(node), false /* is_leaf */);
    } else {
        CO_RETURN cb(std::move(node), true /* is_leaf */);
    }
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::get_all_kvs(std::vector< std::pair< K, V > >& kvs) const {
    CO_AWAIT(post_order_traversal(LockType::Read, [this, &kvs](Node node, bool is_leaf) -> BtreeStatus {
        if (!is_leaf) {
            node->get_all_kvs(kvs);
        }
        return BtreeStatus::success;
    }));
    CO_RETURN_VOID;
}

template < typename K, typename V >
folly::Future< folly::Unit > Btree< K, V >::destroy() {
    bool expected = false;
    if (!m_destroyed.compare_exchange_strong(expected, true)) {
        BT_LOG(DEBUG, "Btree is already being destroyed, ignoring this request");
        return folly::makeFuture< folly::Unit >(folly::Unit{});
    }

    if (m_store->is_ephemeral()) {
        post_order_traversal(LockType::Write, [this](Node node, bool is_leaf) -> BtreeStatus {
            remove_node(std::move(node));
            return BtreeStatus::node_freed;
        });
    } else if (!m_store->is_fast_destroy_supported()) {
        // TODO: Implement full range remove.
    } else {
        // Let the store handle fast deletion via destroy_underlying_btree().
    }

    BT_LOG(DEBUG, "btree(root: {}) destroyed successfully", m_root_node_info.bnode_id());
    return m_store->destroy_underlying_btree(*this);
}

template < typename K, typename V >
BtreeTask< uint64_t > Btree< K, V >::get_btree_node_cnt() const {
    uint64_t cnt = 1; /* increment it for root */
    auto tree_lock = CO_AWAIT(lock_tree_shared());
    cnt += CO_AWAIT(get_child_node_cnt(m_root_node_info.bnode_id()));
    CO_RETURN cnt;
}

template < typename K, typename V >
BtreeTask< uint64_t > Btree< K, V >::get_child_node_cnt(bnodeid_t bnodeid) const {
    uint64_t cnt{0};

    auto [ret, node] = CO_AWAIT(read_node(bnodeid, LockType::Read));
    if (ret != BtreeStatus::success) {
        CO_RETURN cnt;
    }

    if (!node->is_leaf()) {
        uint32_t i = 0;
        while (i < node->total_entries()) {
            BtreeLinkInfo p = node->get_nth_key< K >(i, false);
            cnt += CO_AWAIT(get_child_node_cnt(p.bnode_id())) + 1;
            ++i;
        }
        if (node->has_valid_edge()) {
            cnt += CO_AWAIT(get_child_node_cnt(node->edge_id())) + 1;
        }
    }
    // RAII: node unlocks when it goes out of scope
    CO_RETURN cnt;
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::to_string_internal(bnodeid_t bnodeid, std::string& buf) const {
    auto [ret, node] = CO_AWAIT(read_node(bnodeid, LockType::Read));
    if (ret != BtreeStatus::success) {
        CO_RETURN_VOID;
    }

    fmt::format_to(std::back_inserter(buf), "{}\n", node->to_string(true /* print_friendly */));

    if (!node->is_leaf()) {
        uint32_t i = 0;
        while (i < node->total_entries()) {
            BtreeLinkInfo p;
            node->get_nth_value(i, &p, false);
            CO_AWAIT(to_string_internal(p.bnode_id(), buf));
            ++i;
        }
        if (node->has_valid_edge()) {
            CO_AWAIT(to_string_internal(node->edge_id(), buf));
        }
    }
    // RAII unlock
    CO_RETURN_VOID;
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::to_custom_string_internal(bnodeid_t bnodeid, std::string& buf,
                                                           NodeCore::ToStringCallback< K, V > const& cb) const {
    auto [ret, node] = CO_AWAIT(read_node(bnodeid, LockType::Read));
    if (ret != BtreeStatus::success) {
        CO_RETURN_VOID;
    }

    fmt::format_to(std::back_inserter(buf), "{}\n", node->to_custom_string(cb));

    if (!node->is_leaf()) {
        uint32_t i = 0;
        while (i < node->total_entries()) {
            BtreeLinkInfo p;
            node->get_nth_value(i, &p, false);
            CO_AWAIT(to_custom_string_internal(p.bnode_id(), buf, cb));
            ++i;
        }
        if (node->has_valid_edge()) {
            CO_AWAIT(to_custom_string_internal(node->edge_id(), buf, cb));
        }
    }
    CO_RETURN_VOID;
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::to_dot_keys(bnodeid_t bnodeid, std::string& buf,
                                             std::map< uint32_t, std::vector< uint64_t > >& l_map,
                                             std::map< uint64_t, BtreeVisualizeVariables >& info_map) const {
    auto [ret, node] = CO_AWAIT(read_node(bnodeid, LockType::Read));
    if (ret != BtreeStatus::success) {
        CO_RETURN_VOID;
    }

    fmt::format_to(std::back_inserter(buf), "{}\n", node->to_dot_keys());
    l_map[node->level()].push_back(node->node_id());
    info_map[node->node_id()].midPoint = node->is_leaf() ? 0 : node->total_entries() / 2;
    if (!node->is_leaf()) {
        uint32_t i = 0;
        while (i < node->total_entries()) {
            BtreeLinkInfo p;
            node->get_nth_value(i, &p, false);
            CO_AWAIT(to_dot_keys(p.bnode_id(), buf, l_map, info_map));
            info_map[p.bnode_id()].parent = node->node_id();
            info_map[p.bnode_id()].index = i;
            ++i;
        }
        if (node->has_valid_edge()) {
            CO_AWAIT(to_dot_keys(node->edge_id(), buf, l_map, info_map));
            info_map[node->edge_id()].parent = node->node_id();
            info_map[node->edge_id()].index = node->total_entries();
        }
    }
    CO_RETURN_VOID;
}

template < typename K, typename V >
void Btree< K, V >::validate_sanity_child(Node const& parent_node, uint32_t ind) const {
    BtreeLinkInfo child_info;
    K child_first_key;
    K child_last_key;
    K parent_key;

    parent_node->get_nth_value(ind, &child_info, false /* copy */);
    Node child_node = m_underlying->read_node(child_info.bnode_id());
    BT_REL_ASSERT_EQ(child_node.valid(), true, "read failed for child node");

    if (child_node->total_entries() == 0) {
        auto parent_entries = parent_node->total_entries();
        if (!child_node->is_leaf()) {
            BT_REL_ASSERT_EQ(((parent_node->has_valid_edge() && ind == parent_entries)), true);
        }
        return;
    }
    child_node->get_first_key(&child_first_key);
    child_node->get_last_key(&child_last_key);
    BT_REL_ASSERT_LE(child_first_key.compare(&child_last_key), 0);
    if (ind == parent_node->total_entries()) {
        BT_REL_ASSERT_EQ(parent_node->has_valid_edge(), true);
        if (ind > 0) {
            parent_node->get_nth_key< K >(ind - 1, &parent_key, false);
            BT_REL_ASSERT_GT(child_first_key.compare(&parent_key), 0);
            BT_REL_ASSERT_LT(parent_key.compare_start(&child_first_key), 0);
        }
    } else {
        parent_node->get_nth_key< K >(ind, &parent_key, false);
        BT_REL_ASSERT_LE(child_first_key.compare(&parent_key), 0)
        BT_REL_ASSERT_LE(child_last_key.compare(&parent_key), 0)
        BT_REL_ASSERT_GE(parent_key.compare_start(&child_first_key), 0)
        BT_REL_ASSERT_GE(parent_key.compare_start(&child_first_key), 0)
        if (ind != 0) {
            parent_node->get_nth_key< K >(ind - 1, &parent_key, false);
            BT_REL_ASSERT_GT(child_first_key.compare(&parent_key), 0)
            BT_REL_ASSERT_LT(parent_key.compare_start(&child_first_key), 0)
        }
    }
}

template < typename K, typename V >
void Btree< K, V >::validate_sanity_next_child(Node const& parent_node, uint32_t ind) const {
    BtreeLinkInfo child_info;
    K child_key;
    K parent_key;

    if (parent_node->has_valid_edge()) {
        if (ind == parent_node->total_entries()) {
            return;
        }
    } else {
        if (ind == parent_node->total_entries() - 1) {
            return;
        }
    }
    parent_node->get_nth_value(ind + 1, &child_info, false /* copy */);

    Node child_node = m_underlying->read_node(child_info.bnode_id());
    BT_REL_ASSERT_EQ(child_node.valid(), true, "read failed for next child node");

    if (child_node->total_entries() == 0) {
        auto parent_entries = parent_node->total_entries();
        if (!child_node->is_leaf()) {
            BT_REL_ASSERT_EQ(((parent_node->has_valid_edge() && ind == parent_entries) || (ind = parent_entries - 1)),
                             true);
        }
        return;
    }
    BT_NODE_REL_ASSERT_NE(child_node->total_entries(), 0, child_node.operator->());
    child_node->get_first_key(&child_key);
    parent_node->get_nth_key< K >(ind, &parent_key, false);
    BT_REL_ASSERT_GT(child_key.compare(&parent_key), 0)
    BT_REL_ASSERT_LT(parent_key.compare_start(&child_key), 0)
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::print_node(const bnodeid_t& bnodeid) const {
    std::string buf;

    auto tree_lock = CO_AWAIT(lock_tree_shared());
    auto [ret, node] = CO_AWAIT(read_node(bnodeid, LockType::Read));
    if (ret == BtreeStatus::success) {
        buf = node->to_string(true /* print_friendly */);
        // RAII: node unlocks when it goes out of scope
    }

    BT_LOG(INFO, "Node: <{}>", buf);
    CO_RETURN_VOID;
}

template < typename K, typename V >
void Btree< K, V >::append_route_trace(BtreeRequest& req, Node const& node, BtreeEvent event, uint32_t start_idx,
                                       uint32_t end_idx) const {
    if (req.m_route_tracing) {
        req.m_route_tracing->emplace_back(TraceRouteEntry{.node_id = node->node_id(),
                                                          .node = node.operator->(),
                                                          .start_idx = start_idx,
                                                          .end_idx = end_idx,
                                                          .num_entries = node->total_entries(),
                                                          .level = node->level(),
                                                          .is_leaf = node->is_leaf(),
                                                          .event = event});
    }
}
} // namespace homestore
