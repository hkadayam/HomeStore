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
#include <homestore/index/btree/node_variant/simple_node.hpp>
#include <homestore/index/btree/node_variant/varlen_node.hpp>
#include <homestore/index/btree/node_variant/prefix_node.hpp>
#include <sisl/fds/utils.hpp>

#include <chrono>

namespace homestore {
template < typename T, typename... Args >
static NodeCore* do_create_node(NodeCore::Allocator::Token token, Args&&... args) {
    uint8_t* ptr = NodeCore::Allocator::get(token).alloc_btree_node(sizeof(T));
    T* node = new (ptr) T(std::forward< Args >(args)..., token);
    return dynamic_cast< NodeCore* >(node);
}

template < typename K, typename V, typename... Args >
static NodeCore* do_form_node(btree_node_type node_type, NodeCore::Allocator::Token token, Args&&... args) {
    NodeCore* n{nullptr};
    switch (node_type) {
    case btree_node_type::VAR_OBJECT:
        n = do_create_node< VarObjSizeNode< K, V > >(token, std::forward< Args >(args)...);
        break;

    case btree_node_type::FIXED:
        n = do_create_node< SimpleNode< K, V > >(token, std::forward< Args >(args)...);
        break;

    case btree_node_type::VAR_VALUE:
        n = do_create_node< VarValueSizeNode< K, V > >(token, std::forward< Args >(args)...);
        break;

    case btree_node_type::VAR_KEY:
        n = do_create_node< VarKeySizeNode< K, V > >(token, std::forward< Args >(args)...);
        break;

    case btree_node_type::FIXED_PREFIX:
        n = do_create_node< FixedPrefixNode< K, V > >(token, std::forward< Args >(args)...);
        break;

    default:
        RELEASE_ASSERT(false, "Unsupported node type {}", node_type);
        break;
    }
    return n;
}

template < typename K, typename V >
NodeCore* Btree< K, V >::alloc_node_core(bnodeid_t id, bool is_leaf) const {
    auto const token = NodeCore::Allocator::default_token;
    if (is_leaf) {
        return do_form_node< K, V >(m_bt_cfg.leaf_node_type(), token, id, is_leaf, m_bt_cfg.node_size());
    } else {
        return do_form_node< K, NodeId >(m_bt_cfg.interior_node_type(), token, id, is_leaf,
                                                m_bt_cfg.node_size());
    }
}

template < typename K, typename V >
NodeCore* Btree< K, V >::load_node_core(uint8_t* node_buf, bnodeid_t id) const {
    auto const token = NodeCore::Allocator::default_token;
    if (NodeCore::identify_leaf_node(node_buf)) {
        return do_form_node< K, V >(m_bt_cfg.leaf_node_type(), token, node_buf, id);
    } else {
        return do_form_node< K, NodeId >(m_bt_cfg.interior_node_type(), token, node_buf, id);
    }
}

template < typename K, typename V >
Node Btree< K, V >::clone_temp_node(NodeCore const& src) {
    // Ask the backend to allocate a fresh node of the same type, then overwrite with src's data.
    Node clone = m_underlying->create_node(src.is_leaf());
    clone->overwrite(src);
    return clone;
}
} // namespace homestore
