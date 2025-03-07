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

#include <homestore/btree/btree.hpp>
#include <homestore/btree/detail/btree_common.ipp>
#include <homestore/btree/detail/btree_node_mgr.ipp>
#include <homestore/btree/detail/btree_mutate_impl.ipp>
#include <homestore/btree/detail/btree_query_impl.ipp>
#include <homestore/btree/detail/btree_get_impl.ipp>
#include <homestore/btree/detail/btree_remove_impl.ipp>
#include <homestore/btree/detail/btree_node.hpp>

namespace homestore {
template < typename K, typename V >
Btree< K, V >::Btree(BtreeConfig const& cfg, uuid_t uuid, uuid_t parent_uuid, uint32_t user_sb_size) :
        BtreeBase::BtreeBase(cfg, uuid, parent_uuid, user_sb_size),
        m_metrics{cfg.name().c_str()},
        m_node_size{cfg.node_size()} {
    m_bt_cfg.set_node_data_size(cfg.node_size() - sizeof(persistent_hdr_t));
}

Btree< K, V >::Btree(BtreeConfig const& cfg, superblk< index_table_sb >&& sb) :
        BtreeBase::BtreeBase(cfg, std::move(sb)),
        m_metrics{cfg.name().c_str()},
        m_node_size{cfg.node_size()},
        m_bt_cfg{cfg} {
    m_bt_cfg.set_node_data_size(cfg.node_size() - sizeof(persistent_hdr_t));
}

template < typename K, typename V >
Btree< K, V >::~Btree() {
    if (is_ephemeral()) { do_destroy(); }
}

#if 0
template < typename K, typename V >
void Btree< K, V >::set_root_node_info(const BtreeLinkInfo& info) {
    m_root_node_info = info;
}
#endif

template < typename K, typename V >
void Btree< K, V >::destroy() {
    auto status = do_destroy();
    BT_LOG(DEBUG, "Btree destroy returned status={}", status);
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
void Btree< K, V >::dump_tree_to_file(const std::string& file) const {
    std::string buf;
    m_btree_lock.lock_shared();
    to_string(m_root_node_info.bnode_id(), buf);
    m_btree_lock.unlock_shared();

    BT_LOG(INFO, "Pre order traversal of tree:\n<{}>", buf);
    if (!file.empty()) {
        std::ofstream o(file);
        o.write(buf.c_str(), buf.size());
        o.flush();
    }
}

template < typename K, typename V >
std::string Btree< K, V >::to_custom_string(to_string_cb_t< K, V > const& cb) const {
    std::string buf;
    m_btree_lock.lock_shared();
    to_custom_string_internal(m_root_node_info.bnode_id(), buf, cb);
    m_btree_lock.unlock_shared();

    return buf;
}

template < typename K, typename V >
std::string Btree< K, V >::visualize_tree_keys(const std::string& file) const {
    std::map< uint32_t, std::vector< uint64_t > > level_map;
    std::map< uint64_t, BtreeVisualizeVariables > info_map;
    std::string buf = "digraph G\n"
                      "{ \n"
                      "ranksep = 3.0;\n"
                      R"(graph [splines="polyline"];
)";

    m_btree_lock.lock_shared();
    to_dot_keys(m_root_node_info.bnode_id(), buf, level_map, info_map);
    m_btree_lock.unlock_shared();
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
    if (!file.empty()) {
        std::ofstream o(file);
        o.write(buf.c_str(), buf.size());
        o.flush();
    }
    return buf;
}

template < typename K, typename V >
nlohmann::json Btree< K, V >::get_metrics_in_json(bool updated) {
    return m_metrics.get_result_in_json(updated);
}

template < typename K, typename V >
bnodeid_t Btree< K, V >::root_node_id() const {
    return m_root_node_info.bnode_id();
}

template < typename K, typename V >
uint64_t Btree< K, V >::root_link_version() const {
    return m_root_node_info.link_version();
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
