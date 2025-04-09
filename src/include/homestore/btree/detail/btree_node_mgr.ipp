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

#include <homestore/btree/btree.hpp>
#include <homestore/btree/node_variant/simple_node.hpp>
#include <homestore/btree/node_variant/varlen_node.hpp>
#include <homestore/btree/node_variant/prefix_node.hpp>
#include <sisl/fds/utils.hpp>
// #include <iomgr/iomgr_flip.hpp>

#include <chrono>

namespace homestore {
template < typename T, typename... Args >
static BtreeNode* do_create_node(uint32_t ctx_size, Args&&... args) {
    uint8_t* ptr = new uint8_t[sizeof(T) + ctx_size];
    T* node = new (ptr + ctx_size) T(std::forward< Args >(args)...);
    return dynamic_cast< BtreeNode* >(node);
}

template < typename K, typename V >
BtreeNode* Btree< K, V >::init_node(BtreeNode::Buffer node_buf, bnodeid_t id, bool init_buf, bool is_leaf,
                                    uint32_t ctx_size) const {
    BtreeNode* n{nullptr};
    btree_node_type node_type = is_leaf ? m_bt_cfg.leaf_node_type() : m_bt_cfg.interior_node_type();

    switch (node_type) {
    case btree_node_type::VAR_OBJECT:
        n = is_leaf
            ? do_create_node< VarObjSizeNode< K, V > >(ctx_size, node_buf, id, init_buf, true, m_bt_cfg.node_size())
            : do_create_node< VarObjSizeNode< K, BtreeLinkInfo > >(ctx_size, node_buf, id, init_buf, false,
                                                                   m_bt_cfg.node_size());
        break;

    case btree_node_type::FIXED:
        n = is_leaf ? do_create_node< SimpleNode< K, V > >(ctx_size, node_buf, id, init_buf, true, m_bt_cfg.node_size())
                    : do_create_node< SimpleNode< K, BtreeLinkInfo > >(ctx_size, node_buf, id, init_buf, false,
                                                                       m_bt_cfg.node_size());
        break;

    case btree_node_type::VAR_VALUE:
        n = is_leaf
            ? do_create_node< VarValueSizeNode< K, V > >(ctx_size, node_buf, id, init_buf, true, m_bt_cfg.node_size())
            : do_create_node< VarValueSizeNode< K, BtreeLinkInfo > >(ctx_size, node_buf, id, init_buf, false,
                                                                     m_bt_cfg.node_size());
        break;

    case btree_node_type::VAR_KEY:
        n = is_leaf
            ? do_create_node< VarKeySizeNode< K, V > >(ctx_size, node_buf, id, init_buf, true, m_bt_cfg.node_size())
            : do_create_node< VarKeySizeNode< K, BtreeLinkInfo > >(ctx_size, node_buf, id, init_buf, false,
                                                                   m_bt_cfg.node_size());
        break;

    case btree_node_type::PREFIX:
        n = is_leaf
            ? do_create_node< FixedPrefixNode< K, V > >(ctx_size, node_buf, id, init_buf, true, m_bt_cfg.node_size())
            : do_create_node< FixedPrefixNode< K, BtreeLinkInfo > >(ctx_size, node_buf, id, init_buf, false,
                                                                    m_bt_cfg.node_size());
        break;

    default:
        BT_REL_ASSERT(false, "Unsupported node type {}", node_type);
        break;
    }

    if (n) { n->set_store_type(m_bt_cfg.store_type()); }
    return n;
}
} // namespace homestore
