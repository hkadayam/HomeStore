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

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "homestore/index/btree/btree_kv.h"
#include "homestore/index/btree/btree_base.h"

namespace homestore {

// Base class for any btree operations
struct BtreeRequest {
    BtreeRequest(BtreeBase& btree, bool enable_tracing) : btree_{btree} {
        if (enable_tracing) {
            route_tracing_ = std::make_unique< std::vector< TraceRouteEntry > >();
            route_tracing_->reserve(8);
        }
    }

    std::string route_string() const {
        std::string out;
        if (route_tracing_) {
            fmt::format_to(std::back_inserter(out), "Route size={}\n", route_tracing_->size());
            for (const auto& r : *route_tracing_) {
                fmt::format_to(std::back_inserter(out), "{}\n", r.to_string());
            }
        }
        return out;
    }

    BtreeBase& btree_;
    std::unique_ptr< std::vector< TraceRouteEntry > > route_tracing_{nullptr};
};

// Base class for all range related operations
template < typename K >
struct BtreeRangeRequest : public BtreeRequest {
public:
    uint32_t batch_size() const { return batch_size_; }
    void set_batch_size(uint32_t count) { batch_size_ = count; }

    BtreeTraversalState< K >& search_state() { return search_state_; }
    const BtreeKeyRange< K >& input_range() const { return search_state_.input_range(); }
    void shift_working_range(K&& start_key, bool start_incl) {
        search_state_.shift_working_range(std::move(start_key), start_incl);
    }
    void shift_working_range() { search_state_.shift_working_range(); }
    const BtreeKeyRange< K >& working_range() const { return search_state_.working_range(); }

    const K& first_key() const { return search_state_.first_key(); }
    uint32_t first_key_size() const { return search_state_.first_key_size(); }

    void trim_working_range(K&& end_key, bool end_incl) {
        search_state_.trim_working_range(std::move(end_key), end_incl);
    }

protected:
    BtreeRangeRequest(BtreeBase& btree, BtreeKeyRange< K >&& input_range, uint32_t batch_size = UINT32_MAX,
                      bool enable_tracing = false) :
            BtreeRequest{btree, enable_tracing}, search_state_{std::move(input_range)}, batch_size_{batch_size} {}

private:
    BtreeTraversalState< K > search_state_;
    uint32_t batch_size_{1};
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 2-Phase Filter Abstractions
//
// The 2-phase split (check_key first, then check_kv only when the old value is needed) avoids unnecessary value reads
// for sparse workloads.
//
// Closure adapters (make_put_filter / make_remove_filter / make_get_filter) let existing lambda callers migrate without
// changing call sites.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////// 1: Put Operations ///////////////////////////////////////////////////
enum class PutFilterDecision : uint8_t {
    Keep,         ///< leave the existing entry unchanged
    Replace,      ///< overwrite the existing entry with the request's new value
    ReplaceWith,  ///< overwrite with filter's custom value (from replacement_value())
    Remove,       ///< delete the existing entry
    NeedOldValue, ///< phase-1 only: fetch the old value and call check_kv()
};

struct PutFilter {
    virtual ~PutFilter() = default;

    /// Phase 1: cheap key-only pre-filter.  Default says "I need the value."
    virtual PutFilterDecision check_key(BtreeKey const&) { return PutFilterDecision::NeedOldValue; }

    /// Phase 2: called only when check_key() returns NeedOldValue.
    virtual PutFilterDecision check_kv(BtreeKey const&, BtreeValue const& /* old_val */) = 0;

    /// When check_kv returns ReplaceWith, the btree reads the replacement from here.
    /// The filter owns the storage — pointer must be valid until the next check_kv call.
    virtual BtreeValue const* replacement_value() const { return nullptr; }

    /// Optional: stamp the insert key before writing (e.g., MVCC sequence number).
    virtual void mutate_key(BtreeKey&) {}
};

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

struct BtreeSinglePutRequest : public BtreeRequest {
public:
    BtreeSinglePutRequest(BtreeBase& btree, const BtreeKey& k, const BtreeValue& v, BtreePutType put_type,
                          BtreeValue* existing_val, PutFilter* filter = nullptr) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::PUT)},
            k_{&k},
            v_{&v},
            put_type_{put_type},
            existing_val_{existing_val},
            filter_{filter} {}

    ~BtreeSinglePutRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::PUT, this->route_string());
        }
    }

    const BtreeKey& key() const { return *k_; }
    const BtreeValue& value() const { return *v_; }

    // Pointers (not refs) so the struct stays default-movable; BtreeKey/BtreeValue are abstract bases.
    BtreeKey const* k_;
    BtreeValue const* v_;
    const BtreePutType put_type_;
    BtreeValue* existing_val_;
    PutFilter* filter_{nullptr};
    PutStats stats_{}; ///< populated by the btree after the operation
};

template < typename K >
struct BtreeRangePutRequest : public BtreeRangeRequest< K > {
public:
    BtreeRangePutRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range, BtreePutType put_type,
                         const BtreeValue& value, PutFilter* filter = nullptr) :
            BtreeRangeRequest< K >{btree, std::move(inp_range), std::numeric_limits< uint32_t >::max(),
                                   btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::PUT)},
            put_type_{put_type},
            newval_{&value},
            filter_{filter} {}

    ~BtreeRangePutRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::PUT, this->route_string());
        }
    }

    const BtreePutType put_type_{BtreePutType::UPDATE};
    const BtreeValue* newval_;
    PutFilter* filter_{nullptr};
    PutStats stats_{};
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BtreeBatchPutRequest — insert a pre-sorted vector of key/value pairs. The btree advances offset_ as it processes
// leaves so that callers that split across multiple leaves resume from the right position.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template < typename K, typename V >
struct BtreeBatchPutRequest : public BtreeRequest {
public:
    using entry_t = std::pair< K, V >;

    BtreeBatchPutRequest(BtreeBase& btree, std::vector< entry_t > entries,
                         BtreePutType put_type = BtreePutType::INSERT, PutFilter* filter = nullptr) :
            BtreeRequest{btree, false},
            entries_{std::move(entries)},
            put_type_{put_type},
            filter_{filter},
            end_key_{entries_.back().first} {}

    const entry_t& current() const { return entries_[offset_]; }
    bool done() const { return offset_ >= entries_.size(); }
    void advance() { ++offset_; }

    // For interior traversal: key range covering all remaining entries up to the current trimmed boundary.
    // Start shifts right as advance() is called; end is trimmed per-leaf and reset by shift_working_range().
    BtreeKeyRange< K > working_range() const { return BtreeKeyRange< K >{entries_[offset_].first, end_key_}; }

    // Trim the end boundary to a child's upper key before descending into that leaf.
    void trim_working_range(K&& end_key, bool /*end_incl*/) { end_key_ = std::move(end_key); }

    // Reset the end boundary to the last entry after finishing a leaf so the next sibling
    // sees the full remaining range.
    void shift_working_range() { end_key_ = entries_.back().first; }

    std::vector< entry_t > entries_;
    BtreePutType put_type_;
    PutFilter* filter_{nullptr};
    size_t offset_{0};
    K end_key_{}; ///< current upper boundary; trimmed per leaf, reset by shift_working_range()
    PutStats stats_{};
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BtreeScanPutRequest — insert one key/value, scanning a range first (e.g. to detect overlaps or apply a filter before
// committing the insert). Routes through the unified put() → do_put() → mutate_write_leaf_node() flow.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template < typename K >
struct BtreeScanPutRequest : public BtreeRequest {
public:
    BtreeScanPutRequest(BtreeBase& btree, BtreeKey& insert_key, BtreeValue const& value,
                        BtreeKeyRange< K > const& scan_range, PutFilter* filter = nullptr,
                        size_t max_scan = std::numeric_limits< size_t >::max()) :
            BtreeRequest{btree, false},
            insert_key_{insert_key},
            value_{value},
            scan_range_{scan_range},
            filter_{filter},
            max_scan_{max_scan} {}

    /// Interior traversal uses insert_key to pick the child to descend into.
    const BtreeKey& key() const { return insert_key_; }

    BtreeKey& insert_key_;
    BtreeValue const& value_;
    BtreeKeyRange< K > scan_range_;
    PutFilter* filter_{nullptr};
    size_t max_scan_;
    bool inserted_{false};
};

////////////////////////////////////////////// 2: Remove Operations //////////////////////////////////////////////
enum class RemoveFilterDecision : uint8_t {
    Skip,      ///< do not remove this entry
    Remove,    ///< remove this entry
    NeedValue, ///< phase-1 only: fetch value and call check_kv()
};

struct RemoveFilter {
    virtual ~RemoveFilter() = default;
    virtual RemoveFilterDecision check_key(BtreeKey const&) { return RemoveFilterDecision::NeedValue; }
    virtual RemoveFilterDecision check_kv(BtreeKey const&, BtreeValue const&) = 0;
};

struct BtreeSingleRemoveRequest : public BtreeRequest {
public:
    BtreeSingleRemoveRequest(BtreeBase& btree, const BtreeKey* k, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::REMOVE)},
            k_{k},
            outval_{out_val} {}

    ~BtreeSingleRemoveRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::REMOVE, this->route_string());
        }
    }

    const BtreeKey& key() const { return *k_; }
    const BtreeValue& value() const { return *outval_; }

    const BtreeKey* k_;
    BtreeValue* outval_;
};

template < typename K >
struct BtreeRemoveAnyRequest : public BtreeRequest {
public:
    BtreeRemoveAnyRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::REMOVE)},
            range_{std::move(inp_range)},
            outkey_{out_key},
            outval_{out_val} {}

    ~BtreeRemoveAnyRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::REMOVE, this->route_string());
        }
    }

    BtreeKeyRange< K > range_;
    BtreeKey* outkey_;
    BtreeValue* outval_;
};

template < typename K >
struct BtreeRangeRemoveRequest : public BtreeRangeRequest< K > {
public:
    BtreeRangeRemoveRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range,
                            uint32_t batch_size = std::numeric_limits< uint32_t >::max(),
                            RemoveFilter* filter = nullptr) :
            BtreeRangeRequest< K >(btree, std::move(inp_range), batch_size,
                                   btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::REMOVE)),
            filter_{filter} {}

    ~BtreeRangeRemoveRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::REMOVE, this->route_string());
        }
    }

    RemoveFilter* filter_{nullptr};
    uint32_t removed_count_{0}; // accumulated in do_remove across leaf visits
};

////////////////////////////////////////////// 3: Get Operations //////////////////////////////////////////////
enum class GetFilterDecision : uint8_t {
    Skip,      ///< exclude this entry from results
    Include,   ///< include this entry in results
    NeedValue, ///< phase-1 only: fetch value and call check_kv()
};

struct GetFilter {
    virtual ~GetFilter() = default;
    virtual GetFilterDecision check_key(BtreeKey const&) { return GetFilterDecision::NeedValue; }
    virtual GetFilterDecision check_kv(BtreeKey const&, BtreeValue const&) = 0;
};

struct BtreeSingleGetRequest : public BtreeRequest {
public:
    BtreeSingleGetRequest(BtreeBase& btree, const BtreeKey* k, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::GET)},
            k_{k},
            outval_{out_val} {}

    ~BtreeSingleGetRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::GET, this->route_string());
        }
    }

    const BtreeKey& key() const { return *k_; }
    const BtreeValue& value() const { return *outval_; }

    const BtreeKey* k_;
    BtreeValue* outval_;
};

template < typename K >
struct BtreeGetAnyRequest : public BtreeRequest {
public:
    BtreeGetAnyRequest(BtreeBase& btree, BtreeKeyRange< K >&& range, BtreeKey* out_key, BtreeValue* out_val) :
            BtreeRequest{btree, btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::GET)},
            range_{std::move(range)},
            outkey_{out_key},
            outval_{out_val} {}

    ~BtreeGetAnyRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::GET, this->route_string());
        }
    }

    BtreeKeyRange< K > range_;
    BtreeKey* outkey_;
    BtreeValue* outval_;
};

////////////////////////////////////////////// 4 Range Query Operations //////////////////////////////////////////////
ENUM(BtreeQueryType, uint8_t,
     Sweep, // Walk to first element, then sweep across leaf sibling links. Can only do (and default) forward iteration.
     Traversal // Leafs are reached from parent. Can do both forward & reverse iteration, but slightly slower than Sweep

     // SWEEP_INTRUSIVE_PAGINATION_QUERY,  // TODO: intrusive sweep with held locks
     // SERIALIZABLE_QUERY                 // TODO: serializable isolation
);

template < typename K >
struct BtreeQueryRequest : public BtreeRangeRequest< K > {
public:
    BtreeQueryRequest(BtreeBase& btree, BtreeKeyRange< K >&& inp_range,
                      BtreeQueryType query_type = BtreeQueryType::Sweep, uint32_t batch_size = UINT32_MAX,
                      GetFilter* filter = nullptr, bool reverse_order = false) :
            BtreeRangeRequest< K >{btree, std::move(inp_range), batch_size,
                                   btree.route_tracer().is_enabled_for(BtreeRouteTracer::Op::QUERY)},
            query_type_{query_type},
            filter_{filter},
            reverse_order_{reverse_order} {}

    ~BtreeQueryRequest() {
        if (this->route_tracing_) {
            this->btree_.route_tracer().append_to(BtreeRouteTracer::Op::QUERY, this->route_string());
        }
    }

    BtreeQueryType query_type() const { return query_type_; }
    GetFilter* filter() const { return filter_; }
    bool reverse_order() const { return reverse_order_; }

protected:
    const BtreeQueryType query_type_;
    GetFilter* filter_{nullptr};
    bool reverse_order_{false};
};

/* This class is a top level class to keep track of the locks that are held currently. It is
 * used for serializabke query to unlock all nodes in right order at the end of the lock */
class BtreeLockTracker {
public:
    virtual ~BtreeLockTracker() = default;
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// QueryResultHandle — returned by query() / query_next_batch(). Holds the result batch plus the internal
// state needed to fetch the next one. has_more() == true means query_next_batch() should be called.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template < typename K, typename V >
struct QueryResultHandle {
    using ResultVec = std::vector< std::pair< K, V > >;

    ResultVec results;
    bool has_more_{false};

    // Continuation state — rebuilt into a BtreeQueryRequest by query_next_batch()
    BtreeKeyRange< K > next_range_;
    BtreeQueryType query_type_{BtreeQueryType::Sweep};
    uint32_t batch_size_{UINT32_MAX};
    GetFilter* filter_{nullptr};
    bool reverse_order_{false};

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
