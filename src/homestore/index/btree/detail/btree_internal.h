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

#include <boost/preprocessor/control/if.hpp>
#include <boost/preprocessor/facilities/empty.hpp>
#include <boost/preprocessor/facilities/identity.hpp>
#include <boost/vmd/is_empty.hpp>

#include "sisl/fds/utils.h"
#include "sisl/metrics/metrics.h"

namespace homestore {

// fmt v11's make_format_args requires lvalue references; use format_to + fmt::runtime which takes a forwarding
// reference parameter pack so both rvalues and lvalues bind.
//
// Takes a `name` string-like (not a config object) for the [btree=] prefix so callers that don't yet have a bound
// BtreeBase (e.g. COWBtree::recover() before its base_btree_ is set) can use it directly.  Callers with a config
// just pass `cfg.name()`.
#define _BT_LOG_METHOD_IMPL(req, name, node)                                                                           \
    ([&](fmt::memory_buffer& buf, const char* msgcb, auto&&... args) -> bool {                                         \
        fmt::format_to(fmt::appender{buf}, fmt::runtime("[{}:{}] "), file_name(__FILE__), __LINE__);                   \
        BOOST_PP_IF(                                                                                                   \
            BOOST_VMD_IS_EMPTY(req), BOOST_PP_EMPTY,                                                                   \
            BOOST_PP_IDENTITY(fmt::format_to(fmt::appender{buf}, fmt::runtime("[req={}] "), req->to_string())))        \
        ();                                                                                                            \
        BOOST_PP_IF(BOOST_VMD_IS_EMPTY(name), BOOST_PP_EMPTY,                                                          \
                    BOOST_PP_IDENTITY(fmt::format_to(fmt::appender{buf}, fmt::runtime("[btree={}] "), name)))          \
        ();                                                                                                            \
        BOOST_PP_IF(BOOST_VMD_IS_EMPTY(node), BOOST_PP_EMPTY,                                                          \
                    BOOST_PP_IDENTITY(                                                                                 \
                        fmt::format_to(fmt::appender{buf}, fmt::runtime("[node={}] "), node->to_basic_string())))      \
        ();                                                                                                            \
        fmt::format_to(fmt::appender{buf}, fmt::runtime(msgcb), std::forward< decltype(args) >(args)...);              \
        return true;                                                                                                   \
    })

#define BT_LOG(level, msg, ...)                                                                                        \
    { LOG##level##MOD_FMT(btree, (_BT_LOG_METHOD_IMPL(, this->bt_cfg_.name(), )), msg, ##__VA_ARGS__); }

#define BT_NODE_LOG(level, node, msg, ...)                                                                             \
    { LOG##level##MOD_FMT(btree, (_BT_LOG_METHOD_IMPL(, this->bt_cfg_.name(), node)), msg, ##__VA_ARGS__); }

#define SPECIFIC_BT_LOG(level, bt, msg, ...)                                                                           \
    { LOG##level##MOD_FMT(btree, (_BT_LOG_METHOD_IMPL(, bt.bt_config().name(), )), msg, ##__VA_ARGS__); }
#if 0
#define THIS_BT_LOG(level, req, msg, ...)                                                                              \
    {                                                                                                                  \
        LOG##level##MOD_FMT(                                                                                           \
            btree, ([&](fmt::memory_buffer& buf, const char* msgcb, auto&&... args) -> bool {                          \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[{}:{}] "},                                      \
                                fmt::make_format_args(file_name(__FILE__), __LINE__));                                 \
                BOOST_PP_IF(BOOST_VMD_IS_EMPTY(req), BOOST_PP_EMPTY,                                                   \
                            BOOST_PP_IDENTITY(fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[req={}] "},       \
                                                              fmt::make_format_args(req->to_string()))))               \
                ();                                                                                                    \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[btree={}] "},                                   \
                                fmt::make_format_args(cfg_.name()));                                                   \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{msgcb},                                           \
                                fmt::make_format_args(std::forward< decltype(args) >(args)...));                       \
                return true;                                                                                           \
            }),                                                                                                        \
            msg, ##__VA_ARGS__);                                                                                       \
    }

#define THIS_NODE_LOG(level, btcfg, msg, ...)                                                                          \
    {                                                                                                                  \
        LOG##level##MOD_FMT(                                                                                           \
            btree, ([&](fmt::memory_buffer& buf, const char* msgcb, auto&&... args) -> bool {                          \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[{}:{}] "},                                      \
                                fmt::make_format_args(file_name(__FILE__), __LINE__));                                 \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[btree={}] "},                                   \
                                fmt::make_format_args(btcfg.name()));                                                  \
                BOOST_PP_IF(BOOST_VMD_IS_EMPTY(req), BOOST_PP_EMPTY,                                                   \
                            BOOST_PP_IDENTITY(fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[node={}] "},      \
                                                              fmt::make_format_args(to_string()))))                    \
                ();                                                                                                    \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{msgcb},                                           \
                                fmt::make_format_args(std::forward< decltype(args) >(args)...));                       \
                return true;                                                                                           \
            }),                                                                                                        \
            msg, ##__VA_ARGS__);                                                                                       \
    }

#define BT_ASSERT(assert_type, cond, req, ...)                                                                         \
    {                                                                                                                  \
        assert_type##_ASSERT_FMT(                                                                                      \
            cond,                                                                                                      \
            [&](fmt::memory_buffer& buf, const char* msgcb, auto&&... args) -> bool {                                  \
                BOOST_PP_IF(BOOST_VMD_IS_EMPTY(req), BOOST_PP_EMPTY,                                                   \
                            BOOST_PP_IDENTITY(fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"\n[req={}] "},     \
                                                              fmt::make_format_args(req->to_string()))))               \
                ();                                                                                                    \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[btree={}] "},                                   \
                                fmt::make_format_args(cfg_.name()));                                                   \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{msgcb},                                           \
                                fmt::make_format_args(std::forward< decltype(args) >(args)...));                       \
                return true;                                                                                           \
            },                                                                                                         \
            msg, ##__VA_ARGS__);                                                                                       \
    }

#define BT_ASSERT_CMP(assert_type, val1, cmp, val2, req, ...)                                                          \
    {                                                                                                                  \
        assert_type##_ASSERT_CMP(                                                                                      \
            val1, cmp, val2,                                                                                           \
            [&](fmt::memory_buffer& buf, const char* msgcb, auto&&... args) -> bool {                                  \
                BOOST_PP_IF(BOOST_VMD_IS_EMPTY(req), BOOST_PP_EMPTY,                                                   \
                            BOOST_PP_IDENTITY(fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"\n[req={}] "},     \
                                                              fmt::make_format_args(req->to_string()))))               \
                ();                                                                                                    \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[btree={}] "},                                   \
                                fmt::make_format_args(cfg_.name()));                                                   \
                fmt::vformat_to(fmt::appender{buf}, fmt::string_view{msgcb},                                           \
                                fmt::make_format_args(std::forward< decltype(args) >(args)...));                       \
                return true;                                                                                           \
            },                                                                                                         \
            msg, ##__VA_ARGS__);                                                                                       \
    }
#endif

#define BT_ASSERT(assert_type, cond, ...)                                                                              \
    { assert_type##_ASSERT_FMT(cond, _BT_LOG_METHOD_IMPL(, this->bt_cfg_.name(), ), ##__VA_ARGS__); }

#define BT_ASSERT_CMP(assert_type, val1, cmp, val2, ...)                                                               \
    { assert_type##_ASSERT_CMP(val1, cmp, val2, _BT_LOG_METHOD_IMPL(, this->bt_cfg_.name(), ), ##__VA_ARGS__); }

#define BT_DBG_ASSERT(cond, ...) BT_ASSERT(DEBUG, cond, ##__VA_ARGS__)
#define BT_DBG_ASSERT_EQ(val1, val2, ...) BT_ASSERT_CMP(DEBUG, val1, ==, val2, ##__VA_ARGS__)
#define BT_DBG_ASSERT_NE(val1, val2, ...) BT_ASSERT_CMP(DEBUG, val1, !=, val2, ##__VA_ARGS__)
#define BT_DBG_ASSERT_LT(val1, val2, ...) BT_ASSERT_CMP(DEBUG, val1, <, val2, ##__VA_ARGS__)
#define BT_DBG_ASSERT_LE(val1, val2, ...) BT_ASSERT_CMP(DEBUG, val1, <=, val2, ##__VA_ARGS__)
#define BT_DBG_ASSERT_GT(val1, val2, ...) BT_ASSERT_CMP(DEBUG, val1, >, val2, ##__VA_ARGS__)
#define BT_DBG_ASSERT_GE(val1, val2, ...) BT_ASSERT_CMP(DEBUG, val1, >=, val2, ##__VA_ARGS__)

#define BT_LOG_ASSERT(cond, ...) BT_ASSERT(LOGMSG, cond, ##__VA_ARGS__)
#define BT_LOG_ASSERT_EQ(val1, val2, ...) BT_ASSERT_CMP(LOGMSG, val1, ==, val2, ##__VA_ARGS__)
#define BT_LOG_ASSERT_NE(val1, val2, ...) BT_ASSERT_CMP(LOGMSG, val1, !=, val2, ##__VA_ARGS__)
#define BT_LOG_ASSERT_LT(val1, val2, ...) BT_ASSERT_CMP(LOGMSG, val1, <, val2, ##__VA_ARGS__)
#define BT_LOG_ASSERT_LE(val1, val2, ...) BT_ASSERT_CMP(LOGMSG, val1, <=, val2, ##__VA_ARGS__)
#define BT_LOG_ASSERT_GT(val1, val2, ...) BT_ASSERT_CMP(LOGMSG, val1, >, val2, ##__VA_ARGS__)
#define BT_LOG_ASSERT_GE(val1, val2, ...) BT_ASSERT_CMP(LOGMSG, val1, >=, val2, ##__VA_ARGS__)

#define BT_REL_ASSERT(cond, ...) BT_ASSERT(RELEASE, cond, ##__VA_ARGS__)
#define BT_REL_ASSERT_EQ(val1, val2, ...) BT_ASSERT_CMP(RELEASE, val1, ==, val2, ##__VA_ARGS__)
#define BT_REL_ASSERT_NE(val1, val2, ...) BT_ASSERT_CMP(RELEASE, val1, !=, val2, ##__VA_ARGS__)
#define BT_REL_ASSERT_LT(val1, val2, ...) BT_ASSERT_CMP(RELEASE, val1, <, val2, ##__VA_ARGS__)
#define BT_REL_ASSERT_LE(val1, val2, ...) BT_ASSERT_CMP(RELEASE, val1, <=, val2, ##__VA_ARGS__)
#define BT_REL_ASSERT_GT(val1, val2, ...) BT_ASSERT_CMP(RELEASE, val1, >, val2, ##__VA_ARGS__)
#define BT_REL_ASSERT_GE(val1, val2, ...) BT_ASSERT_CMP(RELEASE, val1, >=, val2, ##__VA_ARGS__)

#define BT_NODE_ASSERT(assert_type, cond, node, ...)                                                                   \
    { assert_type##_ASSERT_FMT(cond, _BT_LOG_METHOD_IMPL(, bt_cfg_.name(), node), ##__VA_ARGS__); }

#define BT_NODE_ASSERT_CMP(assert_type, val1, cmp, val2, node, ...)                                                    \
    { assert_type##_ASSERT_CMP(val1, cmp, val2, _BT_LOG_METHOD_IMPL(, bt_cfg_.name(), node), ##__VA_ARGS__); }

#define BT_NODE_DBG_ASSERT(cond, ...) BT_NODE_ASSERT(DEBUG, cond, ##__VA_ARGS__)
#define BT_NODE_DBG_ASSERT_EQ(val1, val2, ...) BT_NODE_ASSERT_CMP(DEBUG, val1, ==, val2, ##__VA_ARGS__)
#define BT_NODE_DBG_ASSERT_NE(val1, val2, ...) BT_NODE_ASSERT_CMP(DEBUG, val1, !=, val2, ##__VA_ARGS__)
#define BT_NODE_DBG_ASSERT_LT(val1, val2, ...) BT_NODE_ASSERT_CMP(DEBUG, val1, <, val2, ##__VA_ARGS__)
#define BT_NODE_DBG_ASSERT_LE(val1, val2, ...) BT_NODE_ASSERT_CMP(DEBUG, val1, <=, val2, ##__VA_ARGS__)
#define BT_NODE_DBG_ASSERT_GT(val1, val2, ...) BT_NODE_ASSERT_CMP(DEBUG, val1, >, val2, ##__VA_ARGS__)
#define BT_NODE_DBG_ASSERT_GE(val1, val2, ...) BT_NODE_ASSERT_CMP(DEBUG, val1, >=, val2, ##__VA_ARGS__)

#define BT_NODE_LOG_ASSERT(cond, ...) BT_NODE_ASSERT(LOGMSG, cond, ##__VA_ARGS__)
#define BT_NODE_LOG_ASSERT_EQ(val1, val2, ...) BT_NODE_ASSERT_CMP(LOGMSG, val1, ==, val2, ##__VA_ARGS__)
#define BT_NODE_LOG_ASSERT_NE(val1, val2, ...) BT_NODE_ASSERT_CMP(LOGMSG, val1, !=, val2, ##__VA_ARGS__)
#define BT_NODE_LOG_ASSERT_LT(val1, val2, ...) BT_NODE_ASSERT_CMP(LOGMSG, val1, <, val2, ##__VA_ARGS__)
#define BT_NODE_LOG_ASSERT_LE(val1, val2, ...) BT_NODE_ASSERT_CMP(LOGMSG, val1, <=, val2, ##__VA_ARGS__)
#define BT_NODE_LOG_ASSERT_GT(val1, val2, ...) BT_NODE_ASSERT_CMP(LOGMSG, val1, >, val2, ##__VA_ARGS__)
#define BT_NODE_LOG_ASSERT_GE(val1, val2, ...) BT_NODE_ASSERT_CMP(LOGMSG, val1, >=, val2, ##__VA_ARGS__)

#define BT_NODE_REL_ASSERT(cond, ...) BT_NODE_ASSERT(RELEASE, cond, ##__VA_ARGS__)
#define BT_NODE_REL_ASSERT_EQ(val1, val2, ...) BT_NODE_ASSERT_CMP(RELEASE, val1, ==, val2, ##__VA_ARGS__)
#define BT_NODE_REL_ASSERT_NE(val1, val2, ...) BT_NODE_ASSERT_CMP(RELEASE, val1, !=, val2, ##__VA_ARGS__)
#define BT_NODE_REL_ASSERT_LT(val1, val2, ...) BT_NODE_ASSERT_CMP(RELEASE, val1, <, val2, ##__VA_ARGS__)
#define BT_NODE_REL_ASSERT_LE(val1, val2, ...) BT_NODE_ASSERT_CMP(RELEASE, val1, <=, val2, ##__VA_ARGS__)
#define BT_NODE_REL_ASSERT_GT(val1, val2, ...) BT_NODE_ASSERT_CMP(RELEASE, val1, >, val2, ##__VA_ARGS__)
#define BT_NODE_REL_ASSERT_GE(val1, val2, ...) BT_NODE_ASSERT_CMP(RELEASE, val1, >=, val2, ##__VA_ARGS__)

#define ASSERT_IS_VALID_INTERIOR_CHILD_INDX(is_found, found_idx, node)                                                 \
    BT_NODE_DBG_ASSERT((!is_found || ((int)found_idx < (int)node->total_entries()) || node->has_valid_edge()), node,   \
                       "Is_valid_interior_child_check_failed: found_idx={}", found_idx)

using bnodeid_t = uint64_t;
static constexpr bnodeid_t empty_bnodeid = std::numeric_limits< bnodeid_t >::max();
static constexpr uint16_t bt_init_crc_16 = 0x8005;

// ── OpGuard ─────────────────────────────────────────────────────────────────
// Type-erased RAII guard for btree operations. COWBtree places a CPGuard inside; MemBtree uses default (no-op).
// No heap allocation, no vptr — just an inline buffer + function pointer for destruction.
struct OpGuard {
    static constexpr size_t kBufSize = 16;
    alignas(8) uint8_t buf_[kBufSize]{};
    void (*dtor_)(uint8_t*){nullptr};

    OpGuard() = default;
    ~OpGuard() {
        if (dtor_) {
            dtor_(buf_);
        }
    }

    OpGuard(OpGuard&& o) noexcept : dtor_{o.dtor_} {
        std::memcpy(buf_, o.buf_, kBufSize);
        o.dtor_ = nullptr;
    }
    OpGuard(OpGuard const&) = delete;
    OpGuard& operator=(OpGuard const&) = delete;
    OpGuard& operator=(OpGuard&&) = delete;

    // Forwarding, NOT by-value: the held type may own per-thread state whose ownership must transfer by move (e.g.
    // CPGuard's thread-stack pin) — a by-value parameter would copy non-owningly and the temporary's destructor
    // would release that state before make() even returns.
    template < typename T >
    static OpGuard make(T&& val) {
        using U = std::decay_t< T >;
        static_assert(sizeof(U) <= kBufSize, "OpGuard buffer too small for this type");
        static_assert(alignof(U) <= 8, "OpGuard alignment insufficient for this type");
        OpGuard g;
        new (g.buf_) U{std::forward< T >(val)};
        g.dtor_ = [](uint8_t* p) { r_cast< U* >(p)->~U(); };
        return g;
    }
};

VENUM(BtreeNodeType, uint32_t, FIXED = 0, VAR_VALUE = 1, VAR_KEY = 2, VAR_OBJECT = 3, FIXED_PREFIX = 4, COMPACT = 5)

ENUM(BtreeStatus, uint32_t,
     success,                  // Operation completed fully
     has_more,                 // Query pagination: more results available, call query_next_batch
     retry,                    // Concurrent modification detected, caller should retry from root
     node_full,                // Node too full for insert/update, you can retry with larger estimated size in value
     key_not_found,            // Key or range not found
     key_already_exists,       // INSERT_ONLY_IF_NOT_EXISTS and key already present
     merge_not_required,       // Merge check determined no merge is beneficial. An internal status not exposed to user
     partial_removal,          // Remove operation completed partially, caller can retry, but might or might not help
     interior_entry_corrupted, // Interior node entry is missing
     space_not_avail,          // No space in the system to alloc a node
     node_read_failed,         // I/O error reading a node
     node_freed,               // Node was deleted by a concurrent merge, An internal status - not exposed to user
     not_supported,            // Operation not supported by the btree
     btree_destroyed           // The btree is being (or has been) destroyed; no further IO is accepted
);

class NodeCore;

ENUM(BtreeEvent, uint8_t, READ, MUTATE, REMOVE, SPLIT, REPAIR, MERGE);

struct TraceRouteEntry {
    bnodeid_t node_id{empty_bnodeid};
    NodeCore* node{nullptr};
    uint32_t start_idx{0};
    uint32_t end_idx{0};
    uint32_t num_entries{0};
    uint16_t level{0};
    bool is_leaf{false};
    BtreeEvent event{BtreeEvent::READ};

    std::string to_string() const {
        return fmt::format("[level={} {} event={} id={} ptr={} start_idx={} end_idx={} entries={}]", level,
                           (is_leaf ? "LEAF" : "INTERIOR"), enum_name(event), node_id, (void*)node, start_idx, end_idx,
                           num_entries);
    }
};

struct BtreeConfig {
    uint32_t node_size_{0};
    uint32_t inline_value_size_{std::numeric_limits< uint32_t >::max()}; // values larger than this go to overflow
    uint8_t ideal_fill_pct_{90};
    uint8_t suggested_min_pct_{30};
    uint8_t split_pct_{50};
    uint32_t max_merge_nodes_{3};
    bool rebalance_turned_on_{false};
    bool merge_turned_on_{true};

    BtreeNodeType leaf_node_type_{BtreeNodeType::VAR_OBJECT};
    BtreeNodeType int_node_type_{BtreeNodeType::VAR_KEY};
    std::string btree_name_{""}; // Unique name for the btree

private:
    uint32_t suggested_min_size_; // Precomputed values
    uint32_t ideal_fill_size_;

public:
    void finalize(uint32_t node_header_size) {
        ideal_fill_size_ = (uint32_t)((node_size_ - node_header_size) * ideal_fill_pct_) / 100;
        suggested_min_size_ = (uint32_t)((node_size_ - node_header_size) * suggested_min_pct_) / 100;
    }

    uint32_t node_size() const { return node_size_; };
    uint32_t inline_value_size() const { return inline_value_size_; }
    uint32_t split_size(uint32_t filled_size) const { return uint32_cast(filled_size * split_pct_) / 100; }
    uint32_t ideal_fill_size() const { return ideal_fill_size_; }
    uint32_t suggested_min_size() const { return suggested_min_size_; }

    const std::string& name() const { return btree_name_; }
    BtreeNodeType leaf_node_type() const { return leaf_node_type_; }
    BtreeNodeType interior_node_type() const { return int_node_type_; }
};

class BtreeMetrics : public sisl::MetricsGroup {
public:
    explicit BtreeMetrics(const char* inst_name) : sisl::MetricsGroup("Btree", inst_name) {
        // register_counter signature: (grp, desc, report_name="", label_pair={}, ptype=Histogram).  For Gauge-published
        // counters without a custom report_name/label, pass empty defaults explicitly.
        REGISTER_COUNTER(btree_obj_count, "Btree object count", "", sisl::MetricLabel{"", ""}, sisl::PublishAs::Gauge);
        REGISTER_COUNTER(btree_leaf_node_count, "Btree Leaf node count", "btree_node_count",
                         sisl::MetricLabel{"node_type", "leaf"}, sisl::PublishAs::Gauge);
        REGISTER_COUNTER(btree_int_node_count, "Btree Interior node count", "btree_node_count",
                         sisl::MetricLabel{"node_type", "interior"}, sisl::PublishAs::Gauge);
        REGISTER_COUNTER(btree_split_count, "Total number of btree node splits");
        REGISTER_COUNTER(btree_merge_count, "Total number of btree node merges");
        REGISTER_COUNTER(btree_depth, "Depth of btree", "", sisl::MetricLabel{"", ""}, sisl::PublishAs::Gauge);

        REGISTER_COUNTER(btree_int_node_writes, "Total number of btree interior node writes", "btree_node_writes",
                         {"node_type", "interior"});
        REGISTER_COUNTER(btree_leaf_node_writes, "Total number of btree leaf node writes", "btree_node_writes",
                         {"node_type", "leaf"});
        REGISTER_COUNTER(btree_num_pc_gen_mismatch, "Number of gen mismatches to recover");

        REGISTER_HISTOGRAM(btree_int_node_occupancy, "Interior node occupancy", "btree_node_occupancy",
                           {"node_type", "interior"});
        REGISTER_HISTOGRAM(btree_leaf_node_occupancy, "Leaf node occupancy", "btree_node_occupancy",
                           {"node_type", "leaf"});
        REGISTER_COUNTER(btree_retry_count, "number of retries");
        REGISTER_COUNTER(write_err_cnt, "number of errors in write");
        REGISTER_COUNTER(query_err_cnt, "number of errors in query");
        REGISTER_COUNTER(remove_err_cnt, "number of errors in remove");
        REGISTER_COUNTER(read_node_count_in_write_ops, "number of nodes read in write_op");
        REGISTER_COUNTER(read_node_count_in_query_ops, "number of nodes read in query_op");
        REGISTER_COUNTER(btree_write_ops_count, "number of btree operations");
        REGISTER_COUNTER(btree_query_ops_count, "number of btree operations");
        REGISTER_COUNTER(btree_remove_ops_count, "number of btree operations");
        REGISTER_HISTOGRAM(btree_exclusive_time_in_int_node,
                           "Exclusive time spent (Write locked) on interior node (ns)", "btree_exclusive_time_in_node",
                           {"node_type", "interior"});
        REGISTER_HISTOGRAM(btree_exclusive_time_in_leaf_node, "Exclusive time spent (Write locked) on leaf node (ns)",
                           "btree_exclusive_time_in_node", {"node_type", "leaf"});
        REGISTER_HISTOGRAM(btree_inclusive_time_in_int_node, "Inclusive time spent (Read locked) on interior node (ns)",
                           "btree_inclusive_time_in_node", {"node_type", "interior"});
        REGISTER_HISTOGRAM(btree_inclusive_time_in_leaf_node, "Inclusive time spent (Read locked) on leaf node (ns)",
                           "btree_inclusive_time_in_node", {"node_type", "leaf"});

        register_me_to_farm();
    } // namespace homestore

    ~BtreeMetrics() { deregister_me_from_farm(); }
};

} // namespace homestore
