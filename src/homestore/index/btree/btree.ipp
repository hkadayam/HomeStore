/*********************************************************************************
 * Copyright 2024-2026 Harihara Kadayam
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

#include <fstream>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include <sisl/logging/logging.h>

#include "homestore/index/btree/btree.h"
#include "homestore/index/btree/detail/btree_common.ipp"
#include "homestore/index/btree/detail/btree_node_mgr.ipp"
#include "homestore/index/btree/detail/mutate_impl.ipp"
#include "homestore/index/btree/detail/query_impl.ipp"
#include "homestore/index/btree/detail/get_impl.ipp"
#include "homestore/index/btree/detail/remove_impl.ipp"

namespace homestore {
template < typename K, typename V >
Btree< K, V >::Btree(BtreeConfig const& cfg, cshared< UnderlyingBtree >& underlying_btree, bnodeid_t root_node_id) :
        BtreeBase::BtreeBase(cfg, underlying_btree), node_ops_(this->bt_cfg_, *this->underlying_) {
    if (root_node_id == empty_bnodeid) {
        // Fresh boot — allocate the root leaf and let the backend know (e.g. to persist the super-block in COWBtree).
        create_root_node();
    } else {
        // Recovery path — caller has already recovered the root node id from persistent state.
        root_node_id_ = root_node_id;
    }
}

template < typename K, typename V >
BtreeResult< PutStats > Btree< K, V >::put_one(BtreeKey const& key, BtreeValue const& value, BtreePutType put_type,
                                               BtreeValue* existing_val, PutFilter* filter) {
    BtreeSinglePutRequest req{*this, key, value, put_type, existing_val, filter};
    auto ret = CO_AWAIT(put(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN req.stats_;
}

template < typename K, typename V >
BtreeResult< PutStats > Btree< K, V >::put_range(BtreeKeyRange< K >&& inp_range, BtreePutType put_type,
                                                 BtreeValue const& value, PutFilter* filter) {
    BtreeRangePutRequest< K > req{*this, std::move(inp_range), put_type, value, filter};
    auto ret = CO_AWAIT(put(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN req.stats_;
}

template < typename K, typename V >
BtreeResult< bool > Btree< K, V >::scan_and_put_one(BtreeKey& insert_key, BtreeValue const& value,
                                                    BtreeKeyRange< K > const& scan_range, PutFilter* filter,
                                                    size_t max_scan) {
    BtreeScanPutRequest< K > req{*this, insert_key, value, scan_range, filter, max_scan};
    auto ret = CO_AWAIT(put(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN req.inserted_;
}

template < typename K, typename V >
BtreeResult< PutStats > Btree< K, V >::batch_put(std::vector< std::pair< K, V > >&& entries, BtreePutType put_type) {
    if (entries.empty()) {
        CO_RETURN PutStats{};
    }
    BtreeBatchPutRequest< K, V > req{*this, std::move(entries), put_type};
    auto ret = CO_AWAIT(put(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN req.stats_;
}

template < typename K, typename V >
BtreeResult< V > Btree< K, V >::remove_one(BtreeKey const& key, RemoveFilter* filter) {
    V out_val;
    BtreeSingleRemoveRequest req{*this, &key, &out_val};
    auto ret = CO_AWAIT(remove(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN out_val;
}

template < typename K, typename V >
BtreeResult< std::pair< K, V > > Btree< K, V >::remove_any(BtreeKeyRange< K >&& inp_range) {
    K out_key;
    V out_val;
    BtreeRemoveAnyRequest< K > req{*this, std::move(inp_range), &out_key, &out_val};
    auto ret = CO_AWAIT(remove(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN std::make_pair(std::move(out_key), std::move(out_val));
}

template < typename K, typename V >
BtreeResult< uint32_t > Btree< K, V >::remove_range(BtreeKeyRange< K >&& inp_range, RemoveFilter* filter) {
    BtreeRangeRemoveRequest< K > req{*this, std::move(inp_range), std::numeric_limits< uint32_t >::max(), filter};
    auto ret = CO_AWAIT(remove(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN req.removed_count_;
}

template < typename K, typename V >
BtreeResult< V > Btree< K, V >::get_one(BtreeKey const& key) {
    V out_val;
    BtreeSingleGetRequest req{*this, &key, &out_val};
    auto ret = CO_AWAIT(get(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN out_val;
}

template < typename K, typename V >
BtreeResult< std::pair< K, V > > Btree< K, V >::get_any(BtreeKeyRange< K >&& inp_range) {
    K out_key;
    V out_val;
    BtreeGetAnyRequest< K > req{*this, std::move(inp_range), &out_key, &out_val};
    auto ret = CO_AWAIT(get(req));
    if (ret != BtreeStatus::success) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    CO_RETURN std::make_pair(std::move(out_key), std::move(out_val));
}

template < typename K, typename V >
BtreeResult< QueryResultHandle< K, V > > Btree< K, V >::query(BtreeKeyRange< K >&& inp_range, uint32_t batch_size,
                                                              BtreeQueryType query_type, GetFilter* filter,
                                                              bool reverse_order) {
    if (reverse_order) {
        query_type = BtreeQueryType::Traversal;
    }

    QueryResultHandle< K, V > handle;
    handle.next_range_ = inp_range;
    handle.query_type_ = query_type;
    handle.batch_size_ = batch_size;
    handle.filter_ = filter;
    handle.reverse_order_ = reverse_order;

    BtreeQueryRequest< K > req{*this, std::move(inp_range), query_type, batch_size, filter, reverse_order};
    auto status = CO_AWAIT(do_query(req, handle.results));
    if (status != BtreeStatus::success && status != BtreeStatus::has_more) {
        CO_RETURN folly::makeUnexpected(status);
    }

    handle.has_more_ = false;
    if (status == BtreeStatus::has_more) {
        handle.has_more_ = true;
        handle.next_range_ = req.working_range();
    }

    BT_LOG(DEBUG, "query returned status={}, results.size={} has_more?={}", enum_name(status), handle.results.size(),
           handle.has_more_);
    CO_RETURN handle;
}

template < typename K, typename V >
BtreeResult< QueryResultHandle< K, V > > Btree< K, V >::query_next_batch(QueryResultHandle< K, V >&& handle) {
    handle.results.clear();

    BtreeQueryRequest< K > qreq{*this,
                                BtreeKeyRange< K >{handle.next_range_},
                                handle.query_type_,
                                handle.batch_size_,
                                handle.filter_,
                                handle.reverse_order_};

    auto ret = CO_AWAIT(do_query(qreq, handle.results));
    handle.has_more_ = (ret == BtreeStatus::has_more);
    if (ret != BtreeStatus::success && ret != BtreeStatus::has_more) {
        CO_RETURN folly::makeUnexpected(ret);
    }
    if (handle.has_more_) {
        handle.next_range_ = qreq.working_range();
    }
    CO_RETURN std::move(handle);
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
    btree_lock_.lock_shared();
    bool ret = verify_node(root_node_id_.bnode_id(), nullptr, -1, update_debug_bm);
    btree_lock_.unlock_shared();

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
    return metrics_.get_result_in_json(updated);
}

template < typename K, typename V >
BtreeTask< std::string > Btree< K, V >::to_string() const {
    std::string buf;
    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        CO_AWAIT(to_string_internal(root_node_id_, buf));
    }
    BT_LOG(DEBUG, "Pre order traversal of tree:\n<{}>", buf);
    CO_RETURN buf;
}

template < typename K, typename V >
BtreeTask< std::string > Btree< K, V >::to_custom_string(NodeCore::ToStringCallback< K, V > cb) const {
    std::string buf;
    {
        auto tree_lock = CO_AWAIT(lock_tree_shared());
        CO_AWAIT(to_custom_string_internal(root_node_id_, buf, std::move(cb)));
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
        CO_AWAIT(to_dot_keys(root_node_id_, buf, level_map, info_map));
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
        CO_RETURN;
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
        CO_RETURN;
    }

    std::ofstream o(file);
    o.write(buf.c_str(), buf.size());
    o.flush();
    CO_RETURN;
}

template < typename K, typename V >
bnodeid_t Btree< K, V >::root_node_id() const {
    return root_node_id_;
}

template < typename K, typename V >
BtreeTask< uint64_t > Btree< K, V >::count_keys(bnodeid_t bnodeid) const {
    if (bnodeid == empty_bnodeid) {
        CO_RETURN 0ULL;
    }
    // read_node returns Expected<Node, BtreeStatus>; unpack without structured binding (Expected has an anonymous union
    // so it is not decomposable).
    auto node_result = CO_AWAIT(underlying_->read_node(bnodeid, LockType::Read));
    if (!node_result.hasValue()) {
        CO_RETURN 0ULL;
    }
    auto node = std::move(node_result.value());
    uint64_t result = 0;
    if (!node->is_leaf()) {
        uint32_t i = 0;
        while (i < node->total_entries()) {
            NodeLink child_info;
            node->get_nth_value(i, &child_info, false);
            result += CO_AWAIT(count_keys(child_info.id()));
            ++i;
        }
        if (node->has_valid_edge()) {
            result += CO_AWAIT(count_keys(node->edge_id()));
        }
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
