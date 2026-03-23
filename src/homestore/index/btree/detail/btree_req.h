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
#include <sisl/fds/buffer.h>
#include <homestore/index/btree/btree_kv.h>
#include <homestore/index/btree/btree_base.h>

namespace homestore {
struct BtreeRequest;

typedef std::pair< BtreeKey, BtreeValue > btree_kv_t;

///////////////////////////////////////////////////////////////////////////////
// 2-Phase Filter Abstractions
//
// Replace the old put_filter_cb_t / remove_filter_cb_t / get_filter_cb_t
// std::function callbacks with virtual base classes.  The 2-phase split
// (check_key first, then check_kv only when the old value is needed) avoids
// unnecessary value reads for sparse workloads.
//
// Closure adapters (make_put_filter / make_remove_filter / make_get_filter)
// let existing lambda callers migrate without changing call sites.
///////////////////////////////////////////////////////////////////////////////

enum class PutFilterDecision : uint8_t {
    Keep,         ///< leave the existing entry unchanged
    Replace,      ///< overwrite the existing entry with the new value
    Remove,       ///< delete the existing entry
    NeedOldValue, ///< phase-1 only: fetch the old value and call check_kv()
};

enum class RemoveFilterDecision : uint8_t {
    Skip,      ///< do not remove this entry
    Remove,    ///< remove this entry
    NeedValue, ///< phase-1 only: fetch value and call check_kv()
};

enum class GetFilterDecision : uint8_t {
    Skip,      ///< exclude this entry from results
    Include,   ///< include this entry in results
    NeedValue, ///< phase-1 only: fetch value and call check_kv()
};

struct PutFilter {
    virtual ~PutFilter() = default;
    /// Phase 1: cheap key-only pre-filter.  Default says "I need the value."
    virtual PutFilterDecision check_key(BtreeKey const&) { return PutFilterDecision::NeedOldValue; }
    /// Phase 2: called only when check_key() returns NeedOldValue.
    virtual PutFilterDecision check_kv(BtreeKey const&, BtreeValue const& /* old_val */) = 0;
    /// Optional: stamp the insert key before writing (e.g., MVCC sequence number).
    virtual void mutate_key(BtreeKey&) {}
};

struct RemoveFilter {
    virtual ~RemoveFilter() = default;
    virtual RemoveFilterDecision check_key(BtreeKey const&) { return RemoveFilterDecision::NeedValue; }
    virtual RemoveFilterDecision check_kv(BtreeKey const&, BtreeValue const&) = 0;
};

struct GetFilter {
    virtual ~GetFilter() = default;
    virtual GetFilterDecision check_key(BtreeKey const&) { return GetFilterDecision::NeedValue; }
    virtual GetFilterDecision check_kv(BtreeKey const&, BtreeValue const&) = 0;
};

///////////////////////////////////////////////////////////////////////////////
// PutStats — counts inserted / updated / removed entries during a put pass.
///////////////////////////////////////////////////////////////////////////////
struct PutStats {
    uint32_t inserted{0};
    uint32_t updated{0};
    uint32_t removed{0};

    int64_t count_delta() const { return int64_t(inserted) - int64_t(removed); }
    void merge(PutStats const& o) {
        inserted += o.inserted;
        updated += o.updated;
        removed += o.removed;
    }
};

// Base class for any btree operations
struct BtreeRequest {
    BtreeRequest(BtreeBase& btree, bool enable_tracing) : m_btree{btree} {
        if (enable_tracing) {
            m_route_tracing = std::make_unique< std::vector< trace_route_entry > >();
            m_route_tracing->reserve(8);
        }
    }

    std::string route_string() const {
        std::string out;
        if (m_route_tracing) {
            fmt::format_to(std::back_inserter(out), "Route size={}\n", m_route_tracing->size());
            for (const auto& r : *m_route_tracing) {
                fmt::format_to(std::back_inserter(out), "{}\n", r.to_string());
            }
        }
        return out;
    }

    BtreeBase& m_btree;
    CPContext* m_op_context{nullptr};
    std::unique_ptr< std::vector< trace_route_entry > > m_route_tracing{nullptr};
};

// Base class for all range related operations
template < typename K >
struct BtreeRangeRequest : public BtreeRequest {
public:
    uint32_t batch_size() const { return m_batch_size; }
    void set_batch_size(uint32_t count) { m_batch_size = count; }

    BtreeTraversalState< K >& search_state() { return m_search_state; }
    const BtreeKeyRange< K >& input_range() const { return m_search_state.input_range(); }
    void shift_working_range(K&& start_key, bool start_incl) {
        m_search_state.shift_working_range(std::move(start_key), start_incl);
    }
    void shift_working_range() { m_search_state.shift_working_range(); }
    const BtreeKeyRange< K >& working_range() const { return m_search_state.working_range(); }

    const K& first_key() const { return m_search_state.first_key(); }
    uint32_t first_key_size() const { return m_search_state.first_key_size(); }

    void trim_working_range(K&& end_key, bool end_incl) {
        m_search_state.trim_working_range(std::move(end_key), end_incl);
    }

protected:
    BtreeRangeRequest(BtreeBase& btree, BtreeKeyRange< K >&& input_range, uint32_t batch_size = UINT32_MAX,
                      bool enable_tracing = false) :
            BtreeRequest{btree, enable_tracing}, m_search_state{std::move(input_range)}, m_batch_size{batch_size} {}

private:
    BtreeTraversalState< K > m_search_state;
    uint32_t m_batch_size{1};
};

/////////////////////////// 1: Put Operations /////////////////////////////////////
struct BtreeSinglePutRequest : public BtreeRequest {
public:
    BtreeSinglePutRequest(BtreeBase& btree, const BtreeKey* k, const BtreeValue* v, btree_put_type put_type,
                          BtreeValue* existing_val = nullptr, PutFilter* filter = nullptr) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::PUT)},
            m_k{k},
            m_v{v},
            m_put_type{put_type},
            m_existing_val{existing_val},
            m_filter{filter} {}

    ~BtreeSinglePutRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::PUT, this->route_string());
        }
    }

    const BtreeKey& key() const { return *m_k; }
    const BtreeValue& value() const { return *m_v; }

    const BtreeKey* m_k;
    const BtreeValue* m_v;
    const btree_put_type m_put_type;
    BtreeValue* m_existing_val;
    PutFilter* m_filter{nullptr};
    PutStats m_stats{}; ///< populated by the btree after the operation
};

template < typename K >
struct BtreeRangePutRequest : public BtreeRangeRequest< K > {
public:
    BtreeRangePutRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range, btree_put_type put_type,
                         const BtreeValue* value, PutFilter* filter = nullptr) :
            BtreeRangeRequest< K >{btree, std::move(inp_range), std::numeric_limits< uint32_t >::max(),
                                   btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::PUT)},
            m_put_type{put_type},
            m_newval{value},
            m_filter{filter} {}

    ~BtreeRangePutRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::PUT, this->route_string());
        }
    }

    const btree_put_type m_put_type{btree_put_type::UPDATE};
    const BtreeValue* m_newval;
    PutFilter* m_filter{nullptr};
    PutStats m_stats{};
};

/////////////////////////// 2: Remove Operations /////////////////////////////////////
struct BtreeSingleRemoveRequest : public BtreeRequest {
public:
    BtreeSingleRemoveRequest(BtreeBase& btree, const BtreeKey* k, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::REMOVE)},
            m_k{k},
            m_outval{out_val} {}

    ~BtreeSingleRemoveRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::REMOVE, this->route_string());
        }
    }

    const BtreeKey& key() const { return *m_k; }
    const BtreeValue& value() const { return *m_outval; }

    const BtreeKey* m_k;
    BtreeValue* m_outval;
};

template < typename K >
struct BtreeRemoveAnyRequest : public BtreeRequest {
public:
    BtreeRemoveAnyRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::REMOVE)},
            m_range{std::move(inp_range)},
            m_outkey{out_key},
            m_outval{out_val} {}

    ~BtreeRemoveAnyRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::REMOVE, this->route_string());
        }
    }

    BtreeKeyRange< K > m_range;
    BtreeKey* m_outkey;
    BtreeValue* m_outval;
};

template < typename K >
struct BtreeRangeRemoveRequest : public BtreeRangeRequest< K > {
public:
    BtreeRangeRemoveRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range,
                            uint32_t batch_size = std::numeric_limits< uint32_t >::max(),
                            RemoveFilter* filter = nullptr) :
            BtreeRangeRequest< K >(btree, std::move(inp_range), batch_size,
                                   btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::REMOVE)),
            m_filter{filter} {}

    ~BtreeRangeRemoveRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::REMOVE, this->route_string());
        }
    }

    RemoveFilter* m_filter{nullptr};
};

/////////////////////////// 3: Get Operations /////////////////////////////////////
struct BtreeSingleGetRequest : public BtreeRequest {
public:
    BtreeSingleGetRequest(BtreeBase& btree, const BtreeKey* k, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::GET)},
            m_k{k},
            m_outval{out_val} {}

    ~BtreeSingleGetRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::GET, this->route_string());
        }
    }

    const BtreeKey& key() const { return *m_k; }
    const BtreeValue& value() const { return *m_outval; }

    const BtreeKey* m_k;
    BtreeValue* m_outval;
};

template < typename K >
struct BtreeGetAnyRequest : public BtreeRequest {
public:
    BtreeGetAnyRequest(BtreeBase& btree, BtreeKeyRange< K >&& range, BtreeKey* out_key, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::GET)},
            m_range{std::move(range)},
            m_outkey{out_key},
            m_outval{out_val} {}

    ~BtreeGetAnyRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::GET, this->route_string());
        }
    }

    BtreeKeyRange< K > m_range;
    BtreeKey* m_outkey;
    BtreeValue* m_outval;
};

/////////////////////////// 4 Range Query Operations /////////////////////////////////////
ENUM(BtreeQueryType, uint8_t,
     // This is default query which walks to first element in range, and then sweeps/walks
     // across the leaf nodes. However, if upon pagination, it again walks down the query from
     // the key it left off.
     SWEEP_NON_INTRUSIVE_PAGINATION_QUERY,

     // Similar to sweep query, except that it retains the node and its lock during
     // pagination. This is more of intrusive query and if the caller is not careful, the read
     // lock will never be unlocked and could cause deadlocks. Use this option carefully.
     SWEEP_INTRUSIVE_PAGINATION_QUERY,

     // This is relatively inefficient query where every leaf node goes from its parent node
     // instead of walking the leaf node across. This is useful only if we want to check and
     // recover if parent and leaf node are in different generations or crash recovery cases.
     TREE_TRAVERSAL_QUERY,

     // This is both inefficient and quiet intrusive/unsafe query, where it locks the range
     // that is being queried for and do not allow any insert or update within that range. It
     // essentially create a serializable level of isolation.
     SERIALIZABLE_QUERY);

template < typename K >
struct BtreeQueryRequest : public BtreeRangeRequest< K > {
public:
    BtreeQueryRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range,
                      BtreeQueryType query_type = BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY,
                      uint32_t batch_size = UINT32_MAX, GetFilter* filter = nullptr, bool reverse_order = false) :
            BtreeRangeRequest< K >{btree, std::move(inp_range), batch_size,
                                   btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::QUERY)},
            m_query_type{query_type},
            m_filter{filter},
            m_reverse_order{reverse_order} {}

    ~BtreeQueryRequest() {
        if (this->m_route_tracing) {
            this->m_btree.route_tracer().append_to(BtreeRouteTracer::Op::QUERY, this->route_string());
        }
    }

    BtreeQueryType query_type() const { return m_query_type; }
    GetFilter* filter() const { return m_filter; }
    bool reverse_order() const { return m_reverse_order; }

protected:
    const BtreeQueryType m_query_type;
    GetFilter* m_filter{nullptr};
    bool m_reverse_order{false};
};

/* This class is a top level class to keep track of the locks that are held currently. It is
 * used for serializabke query to unlock all nodes in right order at the end of the lock */
class BtreeLockTracker {
public:
    virtual ~BtreeLockTracker() = default;
};

///////////////////////////////////////////////////////////////////////////////
// BtreeBatchPutRequest — insert a pre-sorted vector of key/value pairs.
// The btree advances m_offset as it processes leaves so that callers that
// split across multiple leaves resume from the right position.
///////////////////////////////////////////////////////////////////////////////
template < typename K, typename V >
struct BtreeBatchPutRequest : public BtreeRequest {
public:
    using entry_t = std::pair< K, V >;

    BtreeBatchPutRequest(BtreeBase& btree, std::vector< entry_t > entries,
                         btree_put_type put_type = btree_put_type::INSERT_ONLY_IF_NOT_EXISTS) :
            BtreeRequest{btree, false}, m_entries{std::move(entries)}, m_put_type{put_type},
            m_end_key{m_entries.back().first} {}

    const entry_t& current() const { return m_entries[m_offset]; }
    bool done() const { return m_offset >= m_entries.size(); }
    void advance() { ++m_offset; }

    // For interior traversal: key range covering all remaining entries up to the current trimmed boundary.
    // Start shifts right as advance() is called; end is trimmed per-leaf and reset by shift_working_range().
    BtreeKeyRange< K > working_range() const {
        return BtreeKeyRange< K >{m_entries[m_offset].first, m_end_key};
    }

    // Trim the end boundary to a child's upper key before descending into that leaf.
    void trim_working_range(K&& end_key, bool /*end_incl*/) { m_end_key = std::move(end_key); }

    // Reset the end boundary to the last entry after finishing a leaf so the next sibling
    // sees the full remaining range.
    void shift_working_range() { m_end_key = m_entries.back().first; }

    std::vector< entry_t > m_entries;
    btree_put_type m_put_type;
    size_t m_offset{0};
    K m_end_key{};   ///< current upper boundary; trimmed per leaf, reset by shift_working_range()
    PutStats m_stats{};
};

///////////////////////////////////////////////////////////////////////////////
// BtreeScanPutRequest — insert one key/value, scanning a range first (e.g. to
// detect overlaps or apply a filter before committing the insert).
// Routes through the unified put() → do_put() → mutate_write_leaf_node() flow.
///////////////////////////////////////////////////////////////////////////////
template < typename K >
struct BtreeScanPutRequest : public BtreeRequest {
public:
    BtreeScanPutRequest(BtreeBase& btree, BtreeKey& insert_key, BtreeValue const& value,
                        BtreeKeyRange< K > const& scan_range, PutFilter* filter = nullptr,
                        size_t max_scan = std::numeric_limits< size_t >::max()) :
            BtreeRequest{btree, false},
            m_insert_key{insert_key},
            m_value{value},
            m_scan_range{scan_range},
            m_filter{filter},
            m_max_scan{max_scan} {}

    /// Interior traversal uses insert_key to pick the child to descend into.
    const BtreeKey& key() const { return m_insert_key; }

    BtreeKey& m_insert_key;
    BtreeValue const& m_value;
    BtreeKeyRange< K > m_scan_range;
    PutFilter* m_filter{nullptr};
    size_t m_max_scan;
    bool m_inserted{false};
};

///////////////////////////////////////////////////////////////////////////////
// QueryResultHandle — returned by query_traversal() / query_next_batch().
// Holds the result batch plus the internal state needed to fetch the next one.
// has_more() == true means query_next_batch() should be called.
///////////////////////////////////////////////////////////////////////////////
template < typename K, typename V >
struct QueryResultHandle {
    using ResultVec = std::vector< std::pair< K, V > >;

    ResultVec results;
    bool has_more_{false};

    // Continuation state — rebuilt into a BtreeQueryRequest by query_next_batch()
    BtreeBase* btree_{nullptr};
    BtreeKeyRange< K > next_range_;
    BtreeQueryType query_type_{BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY};
    uint32_t batch_size_{UINT32_MAX};
    GetFilter* filter_{nullptr};
    bool reverse_order_{false};
    CPContext* op_context_{nullptr};

    bool has_more() const { return has_more_; }
};

#if 0
class BtreeSweepQueryRequest : public BtreeQueryRequest {
public:
    BtreeSweepQueryRequest(const BtreeSearchRange& criteria, uint32_t iter_count = 1000,
            const match_item_cb_t& match_item_cb = nullptr) :
            BtreeQueryRequest(criteria, iter_count, match_item_cb) {}

    BtreeSweepQueryRequest(const BtreeSearchRange &criteria, const match_item_cb_t& match_item_cb) :
            BtreeQueryRequest(criteria, 1000, match_item_cb) {}

    bool is_serializable() const { return false; }
};

class BtreeSerializableQueryRequest : public BtreeQueryRequest {
public:
    BtreeSerializableQueryRequest(const BtreeSearchRange &range, uint32_t iter_count = 1000,
                             const match_item_cb_t& match_item_cb = nullptr) :
            BtreeQueryRequest(range, iter_count, match_item_cb) {}

    BtreeSerializableQueryRequest(const BtreeSearchRange &criteria, const match_item_cb_t& match_item_cb) :
            BtreeSerializableQueryRequest(criteria, 1000, match_item_cb) {}

    bool is_serializable() const { return true; }
};
#endif
} // namespace homestore
