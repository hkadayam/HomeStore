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
BtreeTask< btree_status_t > Btree< K, V >::get(ReqT& greq) {
    static_assert(std::is_same_v< BtreeSingleGetRequest, ReqT > || std::is_same_v< BtreeGetAnyRequest< K >, ReqT >,
                  "get api is called with non get request type");

    auto tree_lock = CO_AWAIT(lock_tree_shared());
    auto [ret, root] = CO_AWAIT(read_node(m_root_node_info.bnode_id(), LockType::Read));
    if (ret == btree_status_t::success) { ret = CO_AWAIT(do_get(std::move(root), greq)); }
    CO_RETURN ret;
}

// do_get takes my_node by value — owns the RAII lock. Unlocks via destructor.
template < typename K, typename V >
template < typename ReqT >
BtreeTask< btree_status_t > Btree< K, V >::do_get(Node my_node, ReqT& greq) {
    bool found{false};
    uint32_t idx{0};

    if (my_node->is_leaf()) {
        btree_status_t ret{btree_status_t::success};
        if constexpr (std::is_same_v< BtreeGetAnyRequest< K >, ReqT >) {
            std::tie(found, idx) = to_variant_node(my_node)->get_any(greq.m_range, greq.m_outkey, greq.m_outval,
                                                                     /*copy_key=*/true, /*copy_val=*/true);
        } else if constexpr (std::is_same_v< BtreeSingleGetRequest, ReqT >) {
            std::tie(found, idx) = my_node->find(greq.key(), greq.m_outval, true);
        }
        if (!found) {
            ret = btree_status_t::not_found;
        } else {
            if (greq.m_route_tracing) { append_route_trace(greq, my_node, btree_event_t::READ, idx, idx); }
        }
        CO_RETURN ret; // RAII: my_node destructor unlocks
    }

    NodeId child_id;
    if constexpr (std::is_same_v< BtreeGetAnyRequest< K >, ReqT >) {
        std::tie(found, idx) = my_node->find(greq.m_range.start_key(), &child_id, true);
    } else if constexpr (std::is_same_v< BtreeSingleGetRequest, ReqT >) {
        std::tie(found, idx) = my_node->find(greq.key(), &child_id, true);
    }

    if (greq.m_route_tracing) { append_route_trace(greq, my_node, btree_event_t::READ, idx, idx); }
    ASSERT_IS_VALID_INTERIOR_CHILD_INDX(found, idx, my_node);

    auto [child_ret, child] = CO_AWAIT(read_node(child_id.id(), LockType::Read));
    if (child_ret != btree_status_t::success) { CO_RETURN child_ret; }

    unlock_node(my_node); // release parent before descending
    CO_RETURN CO_AWAIT(do_get(std::move(child), greq));
}

} // namespace homestore
