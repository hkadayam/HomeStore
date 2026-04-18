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
BtreeTask< BtreeStatus > Btree< K, V >::post_order_traversal(LockType ltype, const auto& cb) {
    BtreeStatus ret{BtreeStatus::success};
    if (root_node_id_ == empty_bnodeid) {
        CO_RETURN ret;
    }

    if (ltype == LockType::Write) {
        auto tree_lock = CO_AWAIT(lock_tree_excl());
        auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, ltype));
        if (!root_result.hasValue()) {
            CO_RETURN root_result.error();
        }
        ret = CO_AWAIT(post_order_traversal(std::move(root_result.value()), ltype, cb));
        if (ret == BtreeStatus::node_freed) {
            ret = BtreeStatus::success;
        }
    } else {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        auto root_result = CO_AWAIT(underlying_->read_node(root_node_id_, ltype));
        if (!root_result.hasValue()) {
            CO_RETURN root_result.error();
        }
        ret = CO_AWAIT(post_order_traversal(std::move(root_result.value()), ltype, cb));
        if (ret == BtreeStatus::node_freed) {
            ret = BtreeStatus::success;
        }
    }
    CO_RETURN ret;
}

template < typename K, typename V >
BtreeTask< BtreeStatus > Btree< K, V >::post_order_traversal(Node node, LockType ltype, const auto& cb) {
    if (!node->is_leaf()) {
        for (uint32_t i = 0; i <= node->total_entries(); ++i) {
            bnodeid_t child_id;
            if (i == node->total_entries()) {
                if (!node->has_valid_edge()) {
                    break;
                }
                child_id = node->edge_id();
            } else {
                NodeLink link;
                node->get_nth_value(i, &link, false /* copy */);
                child_id = link.id();
            }

            auto child_result = CO_AWAIT(underlying_->read_node(child_id, ltype));
            if (!child_result.hasValue()) {
                CO_RETURN child_result.error();
            }
            CO_AWAIT(post_order_traversal(std::move(child_result.value()), ltype, cb));
        }
        CO_RETURN cb(std::move(node), false /* is_leaf */);
    }
    CO_RETURN cb(std::move(node), true /* is_leaf */);
}

// ── to_string methods ───────────────────────────────────────────────────────
template < typename K, typename V >
BtreeTask< void > Btree< K, V >::to_string_internal(bnodeid_t bnodeid, std::string& buf) const {
    auto result = CO_AWAIT(underlying_->read_node(bnodeid, LockType::Read));
    if (!result.hasValue()) {
        CO_RETURN;
    }
    auto node = std::move(result.value());

    fmt::format_to(std::back_inserter(buf), "{}\n", node->to_string(true /* print_friendly */));

    if (!node->is_leaf()) {
        for (uint32_t i = 0; i < node->total_entries(); ++i) {
            NodeLink link;
            node->get_nth_value(i, &link, /*copy=*/false);
            CO_AWAIT(to_string_internal(link.id(), buf));
        }
        if (node->has_valid_edge()) {
            CO_AWAIT(to_string_internal(node->edge_id(), buf));
        }
    }
    CO_RETURN;
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::to_custom_string_internal(bnodeid_t bnodeid, std::string& buf,
                                                           NodeCore::ToStringCallback< K, V > const& cb) const {
    auto result = CO_AWAIT(underlying_->read_node(bnodeid, LockType::Read));
    if (!result.hasValue()) {
        CO_RETURN;
    }
    auto node = std::move(result.value());

    fmt::format_to(std::back_inserter(buf), "{}\n", node->to_custom_string(cb));

    if (!node->is_leaf()) {
        for (uint32_t i = 0; i < node->total_entries(); ++i) {
            NodeLink link;
            node->get_nth_value(i, &link, /*copy=*/false);
            CO_AWAIT(to_custom_string_internal(link.id(), buf, cb));
        }
        if (node->has_valid_edge()) {
            CO_AWAIT(to_custom_string_internal(node->edge_id(), buf, cb));
        }
    }
    CO_RETURN;
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::to_dot_keys(bnodeid_t bnodeid, std::string& buf,
                                             std::map< uint32_t, std::vector< uint64_t > >& l_map,
                                             std::map< uint64_t, BtreeVisualizeVariables >& info_map) const {
    auto result = CO_AWAIT(underlying_->read_node(bnodeid, LockType::Read));
    if (!result.hasValue()) {
        CO_RETURN;
    }
    auto node = std::move(result.value());

    fmt::format_to(std::back_inserter(buf), "{}\n", node->to_dot_keys());
    l_map[node->level()].push_back(node->node_id());
    info_map[node->node_id()].midPoint = node->is_leaf() ? 0 : node->total_entries() / 2;

    if (!node->is_leaf()) {
        for (uint32_t i = 0; i < node->total_entries(); ++i) {
            NodeLink link;
            node->get_nth_value(i, &link, /*copy=*/false);
            CO_AWAIT(to_dot_keys(link.id(), buf, l_map, info_map));
            info_map[link.id()].parent = node->node_id();
            info_map[link.id()].index = i;
        }
        if (node->has_valid_edge()) {
            CO_AWAIT(to_dot_keys(node->edge_id(), buf, l_map, info_map));
            info_map[node->edge_id()].parent = node->node_id();
            info_map[node->edge_id()].index = node->total_entries();
        }
    }
    CO_RETURN;
}

// ── print_node ──────────────────────────────────────────────────────────────
template < typename K, typename V >
BtreeTask< void > Btree< K, V >::print_node(bnodeid_t bnodeid) const {
    auto tree_lock = CO_AWAIT(lock_tree_shared());
    auto result = CO_AWAIT(underlying_->read_node(bnodeid, LockType::Read));
    if (result.hasValue()) {
        BT_LOG(INFO, "Node: <{}>", result.value()->to_string(true /* print_friendly */));
    }
    CO_RETURN;
}

// ── Route tracing ───────────────────────────────────────────────────────────
template < typename K, typename V >
void Btree< K, V >::append_route_trace(BtreeRequest& req, Node const& node, BtreeEvent event, uint32_t start_idx,
                                       uint32_t end_idx) const {
    if (req.route_tracing_) {
        req.route_tracing_->emplace_back(TraceRouteEntry{.node_id = node->node_id(),
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
