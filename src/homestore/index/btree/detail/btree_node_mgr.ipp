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

#include <array>
#include <chrono>
#include <memory>

#include "homestore/index/btree/btree.h"
#include "homestore/index/btree/node_variant/simple_node.hpp"
#include "homestore/index/btree/node_variant/varlen_node.hpp"
// TODO: re-enable FixedPrefixNode when variant_node.hpp is ported.
// #include "homestore/index/btree/node_variant/prefix_node.hpp"

namespace homestore {

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Node construction tables
//
// Instead of a run-time switch on BtreeNodeType, each variant's constructor is pre-bound into a small array indexed
// directly by the enum value.  At call time we do one array lookup and one indirect call — no branching.
// One table per (K, V) instantiation is emitted by the compiler as a constexpr variable template.
//
// BtreeNodeType ordinals (see btree_internal.h):
//   FIXED=0, VAR_VALUE=1, VAR_KEY=2, VAR_OBJECT=3, FIXED_PREFIX=4, COMPACT=5
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

using fresh_ctor_t = unique< NodeCore > (*)(std::shared_ptr< uint8_t >, bnodeid_t, bool, uint32_t);
using existing_ctor_t = unique< NodeCore > (*)(std::shared_ptr< uint8_t >, bnodeid_t);

template < typename T >
static unique< NodeCore > make_fresh_node(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf,
                                          uint32_t node_size) {
    return std::make_unique< T >(std::move(buf), id, is_leaf, node_size);
}

template < typename T >
static unique< NodeCore > make_existing_node(std::shared_ptr< uint8_t > buf, bnodeid_t id) {
    return std::make_unique< T >(std::move(buf), id);
}

template < typename K, typename V >
inline constexpr std::array< fresh_ctor_t, 6 > kFreshNodeCtors = {
    &make_fresh_node< SimpleNode< K, V > >,       // FIXED        = 0
    &make_fresh_node< VarValueSizeNode< K, V > >, // VAR_VALUE    = 1
    &make_fresh_node< VarKeySizeNode< K, V > >,   // VAR_KEY      = 2
    &make_fresh_node< VarObjSizeNode< K, V > >,   // VAR_OBJECT   = 3
    nullptr,                                      // FIXED_PREFIX = 4 (token-based; not in this table)
    nullptr,                                      // COMPACT      = 5 (not implemented)
};

template < typename K, typename V >
inline constexpr std::array< existing_ctor_t, 6 > kExistingNodeCtors = {
    &make_existing_node< SimpleNode< K, V > >,       // FIXED        = 0
    &make_existing_node< VarValueSizeNode< K, V > >, // VAR_VALUE    = 1
    &make_existing_node< VarKeySizeNode< K, V > >,   // VAR_KEY      = 2
    &make_existing_node< VarObjSizeNode< K, V > >,   // VAR_OBJECT   = 3
    nullptr,                                         // FIXED_PREFIX = 4
    nullptr,                                         // COMPACT      = 5
};

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Btree<K,V>::construct_fresh_node / construct_existing_node
//
// Both backends (MemBtree, COWBtree) allocate the raw page buffer themselves and hand it over.  These methods just
// dispatch to the correct variant constructor based on the configured node type for the level (leaf vs interior).
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

template < typename K, typename V >
unique< NodeCore > Btree< K, V >::construct_fresh_node(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf) {
    const uint32_t node_sz = bt_cfg_.node_size();
    if (is_leaf) {
        const auto nt = bt_cfg_.leaf_node_type();
        auto ctor = kFreshNodeCtors< K, V >[to_size(nt)];
        RELEASE_ASSERT(ctor != nullptr, "Unsupported leaf node type {}", nt);
        return ctor(std::move(buf), id, /*is_leaf=*/true, node_sz);
    } else {
        const auto nt = bt_cfg_.interior_node_type();
        auto ctor = kFreshNodeCtors< K, NodeLink >[to_size(nt)];
        RELEASE_ASSERT(ctor != nullptr, "Unsupported interior node type {}", nt);
        return ctor(std::move(buf), id, /*is_leaf=*/false, node_sz);
    }
}

template < typename K, typename V >
unique< NodeCore > Btree< K, V >::construct_existing_node(std::shared_ptr< uint8_t > buf, bnodeid_t id) {
    const bool is_leaf = NodeCore::identify_leaf_node(buf.get());
    if (is_leaf) {
        const auto nt = bt_cfg_.leaf_node_type();
        auto ctor = kExistingNodeCtors< K, V >[to_size(nt)];
        RELEASE_ASSERT(ctor != nullptr, "Unsupported leaf node type {}", nt);
        return ctor(std::move(buf), id);
    } else {
        const auto nt = bt_cfg_.interior_node_type();
        auto ctor = kExistingNodeCtors< K, NodeLink >[to_size(nt)];
        RELEASE_ASSERT(ctor != nullptr, "Unsupported interior node type {}", nt);
        return ctor(std::move(buf), id);
    }
}

} // namespace homestore
