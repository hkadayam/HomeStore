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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/intrusive_ptr.hpp>
// #include <flip/flip.hpp>
#include <sisl/logging/logging.h>
#include <sisl/fds/buffer.hpp>

#include <homestore/index/btree/btree.h>
#include <homestore/index/btree/detail/btree_common.ipp>
#include <homestore/index/btree/detail/btree_node_mgr.ipp>
#include <homestore/index/btree/detail/mutate_impl.ipp>
#include <homestore/index/btree/detail/btree_query_impl.ipp>
#include <homestore/index/btree/detail/get_impl.ipp>
#include <homestore/index/btree/detail/btree_remove_impl.ipp>
#include <homestore/index/btree/detail/btree_node.h>

namespace homestore {
template < typename K, typename V >
Btree< K, V >::Btree(BtreeConfig const& cfg, uuid_t uuid, uuid_t parent_uuid, uint32_t user_sb_size) :
        BtreeBase::BtreeBase(cfg, uuid, parent_uuid, user_sb_size) {
    create_root_node();
}

template < typename K, typename V >
Btree< K, V >::Btree(BtreeConfig const& cfg, superblk< IndexSuperBlock >&& sb) :
        BtreeBase::BtreeBase(cfg, std::move(sb)) {
    if (m_root_node_info.bnode_id() == empty_bnodeid) {
        BT_LOG(INFO, "Loaded an empty btree, we are creating a new root node");
        create_root_node();
    }
}

template < typename K, typename V >
Btree< K, V >::~Btree() {
    if (is_ephemeral()) { destroy(); }
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::put_one(BtreeKey const& key, BtreeValue const& value,
                                                    btree_put_type put_type, BtreeValue* existing_val,
                                                    PutFilter* filter) {
    BtreeSinglePutRequest req{*this, &key, &value, put_type, existing_val, filter};
    CO_RETURN CO_AWAIT(put(req));
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::put_range(BtreeKeyRange< K >&& inp_range, btree_put_type put_type,
                                                      BtreeValue const& value, PutFilter* filter) {
    BtreeRangePutRequest< K > req{*this, std::move(inp_range), put_type, &value, filter};
    CO_RETURN CO_AWAIT(put(req));
}

template < typename K, typename V >
BtreeTask< std::pair< btree_status_t, bool > >
Btree< K, V >::scan_and_put_one(BtreeKey& insert_key, BtreeValue const& value,
                                 BtreeKeyRange< K > const& scan_range, PutFilter* filter, size_t max_scan) {
    BtreeScanPutRequest< K > req{*this, insert_key, value, scan_range, filter, max_scan};
    auto ret = CO_AWAIT(put(req));
    CO_RETURN {ret, req.m_inserted};
}

template < typename K, typename V >
BtreeTask< std::pair< btree_status_t, PutStats > >
Btree< K, V >::batch_put(std::vector< std::pair< K, V > >&& entries, btree_put_type put_type) {
    if (entries.empty()) CO_RETURN {btree_status_t::success, PutStats{}};
    BtreeBatchPutRequest< K, V > req{*this, std::move(entries), put_type};
    auto ret = CO_AWAIT(put(req));
    CO_RETURN {ret, req.m_stats};
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::get_one(BtreeKey const& key, BtreeValue* out_val) {
    BtreeSingleGetRequest req{*this, &key, out_val};
    CO_RETURN CO_AWAIT(get(req));
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::get_any(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key,
                                                    BtreeValue* out_val) {
    BtreeGetAnyRequest< K > req{*this, std::move(inp_range), out_key, out_val};
    CO_RETURN CO_AWAIT(get(req));
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::remove_one(BtreeKey const& key, BtreeValue* out_val) {
    BtreeSingleRemoveRequest req{*this, &key, out_val};
    CO_RETURN CO_AWAIT(remove(req));
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::remove_any(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key,
                                                       BtreeValue* out_val) {
    BtreeRemoveAnyRequest< K > req{*this, std::move(inp_range), out_key, out_val};
    CO_RETURN CO_AWAIT(remove(req));
}

template < typename K, typename V >
BtreeTask< std::pair< btree_status_t, RemovePaginateCookie< K > > >
Btree< K, V >::remove_range(BtreeKeyRange< K >&& inp_range, uint32_t batch_size, RemoveFilter* filter) {
    auto req_ptr =
        std::make_unique< BtreeRangeRemoveRequest< K > >(*this, std::move(inp_range), batch_size, filter);
    auto status = CO_AWAIT(remove(*req_ptr));
    CO_RETURN std::pair(status, std::move(req_ptr));
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::remove_range_next(RemovePaginateCookie< K >& cookie) {
    auto const status = CO_AWAIT(remove(*cookie));
    if (status != btree_status_t::has_more) { cookie.reset(); }
    CO_RETURN status;
}

template < typename K, typename V >
BtreeTask< std::pair< btree_status_t, QueryPaginateCookie< K > > >
Btree< K, V >::query(BtreeKeyRange< K >&& inp_range,            // Input range to query for
                     std::vector< std::pair< K, V > >& out_kvs, // Results will be appended
                     uint32_t batch_size,                       // Batch size, default the whole set
                     BtreeQueryType query_type,                 // See query_impl for more details
                     GetFilter* filter                          // Any filtering condition while picking the result set
) {
    auto req_ptr = std::make_unique< BtreeQueryRequest< K > >(*this, std::move(inp_range), query_type, batch_size,
                                                              filter);
    auto status = CO_AWAIT(query(*req_ptr, out_kvs));
    CO_RETURN std::pair(status, std::move(req_ptr));
}

template < typename K, typename V >
BtreeTask< btree_status_t > Btree< K, V >::query_next(QueryPaginateCookie< K >& cookie,
                                                       std::vector< std::pair< K, V > >& out_kvs) {
    if (cookie == nullptr) { CO_RETURN btree_status_t::success; }
    auto const status = CO_AWAIT(query(*cookie, out_kvs));
    if (status != btree_status_t::has_more) { cookie.reset(); }
    CO_RETURN status;
}

#if 0
/**
 * @brief : verify btree is consistent and no corruption;
 *
 * @param update_debug_bm : true or false;
 *
 * @return : true if btree is not corrupted.
 *           false if btree is corrupted;
 */
template < typename K, typename V >
bool Btree< K, V >::verify_tree(bool update_debug_bm) const {
    m_btree_lock.lock_shared();
    bool ret = verify_node(m_root_node_info.bnode_id(), nullptr, -1, update_debug_bm);
    m_btree_lock.unlock_shared();

    return ret;
}
#endif

/**
 * @brief : get the status of this btree;
 *
 * @param log_level : verbosity level;
 *
 * @return : status in json form;
 */
template < typename K, typename V >
nlohmann::json Btree< K, V >::get_status(int log_level) const {
    nlohmann::json j;
    return j;
}

template < typename K, typename V >
nlohmann::json Btree< K, V >::get_metrics_in_json(bool updated) {
    return m_metrics.get_result_in_json(updated);
}

template < typename K, typename V >
BtreeTask< std::string > Btree< K, V >::to_string() const {
    std::string buf;
    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        CO_AWAIT(to_string_internal(m_root_node_info.bnode_id(), buf));
    }
    BT_LOG(DEBUG, "Pre order traversal of tree:\n<{}>", buf);
    CO_RETURN buf;
}

template < typename K, typename V >
BtreeTask< std::string > Btree< K, V >::to_custom_string(NodeCore::ToStringCallback< K, V > cb) const {
    std::string buf;
    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        CO_AWAIT(to_custom_string_internal(m_root_node_info.bnode_id(), buf, std::move(cb)));
    }
    CO_RETURN buf;
}

template < typename K, typename V >
BtreeTask< std::string > Btree< K, V >::to_digraph_visualize_format() const {
    std::map< uint32_t, std::vector< uint64_t > > level_map;
    std::map< uint64_t, BtreeVisualizeVariables > info_map;
    std::string buf = "digraph G\n"
                      "{ \n"
                      "ranksep = 3.0;\n"
                      R"(graph [splines="polyline"];
                    )";

    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        CO_AWAIT(to_dot_keys(m_root_node_info.bnode_id(), buf, level_map, info_map));
    }
    for (const auto& [child, info] : info_map) {
        if (info.parent) {
            buf += fmt::format(R"(
            "{}":connector{} -> "{}":"key{}" [splines=false];)",
                               info.parent, info.index, child, info.midPoint);
        }
    }

    std::string result;
    for (const auto& [key, values] : level_map) {
        result += "{rank=same; ";
        std::vector< std::string > quotedValues;
        std::transform(values.begin(), values.end(), std::back_inserter(quotedValues),
                       [](uint64_t value) { return fmt::format("\"{}\"", value); });

        result += fmt::to_string(fmt::join(quotedValues, " ")) + "}\n";
    }

    buf += "\n" + result + " }\n";
    CO_RETURN buf;
}

template < typename K, typename V >
BtreeTask< void > Btree< K, V >::dump(const std::string& file, std::string format,
                                       NodeCore::ToStringCallback< K, V > cb) const {
    if (file.empty()) {
        BT_LOG(ERROR, "Wrong file name to dump btree");
        CO_RETURN_VOID;
    }

    std::string buf;
    if (format == "string") {
        BT_LOG(DEBUG, "Dumping btree in string format");
        buf = CO_AWAIT(to_string());
    } else if (format == "dot") {
        BT_LOG(DEBUG, "Dumping btree to dot format");
        buf = CO_AWAIT(to_digraph_visualize_format());
    } else if (format == "custom") {
        if (cb == nullptr) {
            BT_LOG(WARN, "Custom format requested but no callback provided, dumping as string");
            buf = CO_AWAIT(to_string());
        } else {
            buf = CO_AWAIT(to_custom_string(std::move(cb)));
        }
    } else {
        BT_LOG(ERROR, "Invalid format={} to dump btree", format);
        CO_RETURN_VOID;
    }

    std::ofstream o(file);
    o.write(buf.c_str(), buf.size());
    o.flush();
    CO_RETURN_VOID;
}

template < typename K, typename V >
bnodeid_t Btree< K, V >::root_node_id() const {
    return m_root_node_info.bnode_id();
}

template < typename K, typename V >
BtreeTask< uint64_t > Btree< K, V >::count_keys(bnodeid_t bnodeid) const {
    if (bnodeid == empty_bnodeid) { CO_RETURN 0ULL; }
    auto [ret, node] = CO_AWAIT(read_node(bnodeid, LockType::Read));
    if (ret != btree_status_t::success) { CO_RETURN 0ULL; }
    uint64_t result = 0;
    if (!node->is_leaf()) {
        uint32_t i = 0;
        while (i < node->total_entries()) {
            BtreeLinkInfo child_info;
            node->get_nth_value(i, &child_info, false);
            result += CO_AWAIT(count_keys(child_info.bnode_id()));
            ++i;
        }
        if (node->has_valid_edge()) { result += CO_AWAIT(count_keys(node->edge_id())); }
    } else {
        result = node->total_entries();
    }
    // RAII: node unlocks when it goes out of scope
    CO_RETURN result;
}

// TODO: Commenting out flip till we figure out how to move flip dependency inside sisl package.
#if 0
#ifdef _PRERELEASE
template < typename K, typename V >
static void Btree< K, V >::set_io_flip() {
    /* IO flips */
    FlipClient* fc = iomgr_flip::client_instance();
    FlipFrequency freq;
    FlipCondition cond1;
    FlipCondition cond2;
    freq.set_count(2000000000);
    freq.set_percent(2);

    FlipCondition null_cond;
    fc->create_condition("", flip::Operator::DONT_CARE, (int)1, &null_cond);

    fc->create_condition("nuber of entries in a node", flip::Operator::EQUAL, 0, &cond1);
    fc->create_condition("nuber of entries in a node", flip::Operator::EQUAL, 1, &cond2);
    fc->inject_noreturn_flip("btree_upgrade_node_fail", {cond1, cond2}, freq);

    fc->create_condition("nuber of entries in a node", flip::Operator::EQUAL, 4, &cond1);
    fc->create_condition("nuber of entries in a node", flip::Operator::EQUAL, 2, &cond2);

    fc->inject_retval_flip("btree_delay_and_split", {cond1, cond2}, freq, 20);
    fc->inject_retval_flip("btree_delay_and_split_leaf", {cond1, cond2}, freq, 20);
    fc->inject_noreturn_flip("btree_parent_node_full", {null_cond}, freq);
    fc->inject_noreturn_flip("btree_leaf_node_split", {null_cond}, freq);
    fc->inject_retval_flip("btree_upgrade_delay", {null_cond}, freq, 20);
    fc->inject_retval_flip("writeBack_completion_req_delay_us", {null_cond}, freq, 20);
    fc->inject_noreturn_flip("btree_read_fast_path_not_possible", {null_cond}, freq);
}

template < typename K, typename V >
static void Btree< K, V >::set_error_flip() {
    /* error flips */
    FlipClient* fc = iomgr_flip::client_instance();
    FlipFrequency freq;
    freq.set_count(20);
    freq.set_percent(10);

    FlipCondition null_cond;
    fc->create_condition("", flip::Operator::DONT_CARE, (int)1, &null_cond);

    fc->inject_noreturn_flip("btree_read_fail", {null_cond}, freq);
    fc->inject_noreturn_flip("fixed_blkalloc_no_blks", {null_cond}, freq);
}
#endif
#endif
} // namespace homestore
