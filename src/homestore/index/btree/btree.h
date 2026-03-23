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

#include <atomic>
#include <array>
#include <queue>

#include <folly/small_vector.h>

#include <homestore/index/btree/btree_async.h>
#include <homestore/index/btree/detail/btree_req.h>
#include <homestore/index/btree/btree_kv.h>
#include <homestore/index/btree/detail/btree_internal.h>
#include <homestore/index/btree/detail/btree_node.h>
#include <homestore/index_service.hpp>
#include <homestore/index/btree/btree_base.h>

namespace homestore {

class BtreeStore;

template < typename K >
using RemovePaginateCookie = unique< BtreeRangeRemoveRequest< K > >;

template < typename K >
using QueryPaginateCookie = unique< BtreeQueryRequest< K > >;

template < typename K, typename V >
class Btree : public BtreeBase {
public:
    /////////////////////////////////////// All External APIs /////////////////////////////
    Btree(BtreeConfig const& cfg, uuid_t uuid = uuid_t{}, uuid_t parent_uuid = uuid_t{}, uint32_t user_sb_size = 0);
    virtual ~Btree();

    // @brief Inserts or updates a key-value pair in the B-tree.
    //
    // This function inserts a new key-value pair or updates an existing key-value pair in the B-tree
    // based on the specified put type. Optionally, it can return the existing value and apply a filter
    // callback before insertion.
    //
    // @param key The key to be inserted or updated.
    // @param value The value to be associated with the key.
    // @param put_type The type of put operation (e.g., insert, update, upsert).
    // @param existing_val Optional pointer to store the existing value prior to update if the key already exists.
    // @param filter_cb Optional callback function to apply a filter before insertion. If provided, before putting, if
    // an existing key-value pair is found, the filter callback is called with the existing key, value and the new
    // value. The callback could return "replace" in that case the existing value is replaced with the new value or it
    // could return "keep" in that case key is not modified.
    //
    // @return The status of the put operation.
    //
    BtreeTask< btree_status_t > put_one(BtreeKey const& key, BtreeValue const& value, btree_put_type put_type,
                                        BtreeValue* existing_val = nullptr, PutFilter* filter = nullptr);

    // @brief Inserts or updates a range of key-value pairs in the B-tree.
    //
    // This function inserts a new range of key-value pairs or updates existing key-value pairs in the B-tree
    // based on the specified put type. Optionally, it can return the existing value and apply a filter
    // callback before insertion.
    //
    // This is an unique function which can be used for multiple purpose based on the key type.
    //
    // Interval Key Behavior:
    // If the key is an interval key (which means can next_key be obtained by doing prev_key + 1), for example an
    // integer keys. If the input range is provided for an interval key, example [1, 50), then it will behave the
    // following way
    // 1. If the put_type is INSERT and if a specific key in the interval range is not present in the btree, then it
    // will insert it.
    //
    // 2. If the put_type is UPSERT, then it will insert the keys within the range for which there is no entry in the
    // btree. However for keys that exist, it will call the filter_cb(key, current_value, new_value) if provided and
    // expects the callback to return the decision. The decision could be
    //     a. replace - replace the existing value with the new value. Note that the new_value will also be added the
    //     same offset as the key. So if key range is [1. 50) and if the key is 10, then the value will be added at 10th
    //     of the original value provided (of course the shifting of 10 can be avoided by the caller by supplying a
    //     BtreeValue override which simply doesn't add)
    //
    //     b. remove - remove the key from the btree and don't add the new value. This feature is useful when we use the
    //     btree to maintain multiple versions of the key and when we write the new version of the key, we need to
    //     remove the older versions of the key along with this write operation.
    //
    //     c. keep - keep the existing value as is and don't add the new value.
    //
    // 3. If the put_type is UPDATE, then it will only act on keys which already exist and the behavior is identical to
    // upsert case above when the key is present.
    //
    // Non-Interval Key Behavior:
    // If the key is not an interval key, then only put_type = UPDATE is supported. It will walk through the keys within
    // the range and then do a filter_cb(key, current_value, new_value) if provided and expects the callback to return
    // the decision. The decision could be
    //    a. replace - replace the existing value with the new value for that key.
    //    b. remove - remove the key from the btree and don't update the new value.
    //    c. keep - keep the existing value as is and don't modify the key to new value.
    // In this non-interval key case, the range of keys are all updated with the same value.
    //
    // @param inp_range The range of keys to insert, upsert or update
    // @param put_type The type of put operation (e.g., insert, update, upsert).
    // @param value The value to be associated with the key. Behavior is different for interval and non-interval keys
    // (see above)
    // @param filter Optional filter to apply before insertion. (See above for details)
    //
    // @return The status of the put operation
    BtreeTask< btree_status_t > put_range(BtreeKeyRange< K >&& inp_range, btree_put_type put_type,
                                          BtreeValue const& value, PutFilter* filter = nullptr);

    // @brief Gets the value associated with the specified key from the B-tree.
    //
    // @param key The key to search for.
    // @param out_val A pointer to store the value associated with the key. (Should be non-nullptr)
    //
    // @return The status of the get operation.
    BtreeTask< btree_status_t > get_one(BtreeKey const& key, BtreeValue* out_val);

    // @brief Gets any one value associated with the given key range. If the key range matches multiple keys, then btree
    // will randomly pick one key and return the value associated with it.
    //
    // @param inp_range The range of keys to search for.
    // @param out_key A pointer to store the picked key of the entry found. (Should be non-nullptr)
    // @param out_val A pointer to store the value associated with the picked key. (Should be non-nullptr)
    //
    // @return The status of the get_any operation.
    BtreeTask< btree_status_t > get_any(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val);

    // @brief Removes the key-value pair associated with the specified key from the B-tree.
    //
    // @param key The key to remove.
    // @param out_val An optional pointer to store the value associated with the key before removal.
    //
    // @return The status of the remove operation.
    BtreeTask< btree_status_t > remove_one(BtreeKey const& key, BtreeValue* out_val);

    // @brief Removes any one key-value pair associated with the given key range. If the key range matches multiple
    // keys, then btree will randomly pick one key and remove the key-value pair associated with it.
    //
    // @param inp_range The range of keys to search for.
    // @param out_key A pointer to store the picked key within the range. (Should be non-nullptr). Valid only if return
    // status is btree_status_t::success.
    // @param out_val A pointer to store the value associated with the picked key. (Should be non-nullptr) Valid only if
    // return status is btree_status_t::success.
    //
    // @return The status of the remove_any operation.
    BtreeTask< btree_status_t > remove_any(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val);

    BtreeTask< std::pair< btree_status_t, RemovePaginateCookie< K > > >
    remove_range(BtreeKeyRange< K >&& inp_range, uint32_t batch_size = std::numeric_limits< uint32_t >::max(),
                 RemoveFilter* filter = nullptr);

    BtreeTask< btree_status_t > remove_range_next(RemovePaginateCookie< K >& cookie);

    BtreeTask< std::pair< btree_status_t, QueryPaginateCookie< K > > >
    query(BtreeKeyRange< K >&& inp_range, std::vector< std::pair< K, V > >& out_kvs,
          uint32_t batch_size = std::numeric_limits< uint32_t >::max(),
          BtreeQueryType query_type = BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY,
          GetFilter* filter = nullptr);

    BtreeTask< btree_status_t > query_next(QueryPaginateCookie< K >& cookie,
                                           std::vector< std::pair< K, V > >& out_kvs);

    // @brief Scan a key range for inline GC (applying filter to existing entries), stamp the insert key via
    // filter->mutate_key(), then insert it — used for MVCC/versioned insert patterns.
    //
    // @param insert_key   Key to insert (may be mutated by filter->mutate_key before write).
    // @param value        Value to insert.
    // @param scan_range   Range to scan for entries to GC before the insert.
    // @param filter       Optional 2-phase filter applied to each entry in scan_range.
    // @param max_scan     Cap on how many entries to inspect during the GC scan.
    //
    // @return {status, was_inserted}  was_inserted is false if the key already existed and filter said Keep.
    BtreeTask< std::pair< btree_status_t, bool > >
    scan_and_put_one(BtreeKey& insert_key, BtreeValue const& value, BtreeKeyRange< K > const& scan_range,
                     PutFilter* filter = nullptr, size_t max_scan = std::numeric_limits< size_t >::max());

    // @brief Insert a pre-sorted vector of key/value pairs in a single tree traversal.
    //
    // @param entries   Sorted vector of (key, value) pairs to insert.
    // @param put_type  Put semantics (INSERT_ONLY, INSERT_OR_REPLACE, etc.)
    //
    // @return status and aggregate PutStats for the entire batch.
    BtreeTask< std::pair< btree_status_t, PutStats > > batch_put(std::vector< std::pair< K, V > >&& entries,
                                                                  btree_put_type put_type);

    // @brief Retrieve the first (leftmost) key/value in a range.
    //
    // @param inp_range  Range to search.
    // @param out_key    Receives the first key found (must be non-nullptr).
    // @param out_val    Receives the value of the first key found (must be non-nullptr).
    //
    // @return btree_status_t::success if found, btree_status_t::not_found otherwise.
    BtreeTask< btree_status_t > get_first(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val) {
        inp_range.set_multi_option(MultiMatchOption::LEFT_MOST);
        CO_RETURN CO_AWAIT(get_any(std::move(inp_range), out_key, out_val));
    }

    // @brief Start a paginated traversal returning a QueryResultHandle.
    //
    // Unlike query() the handle owns the continuation state; pass it to query_next_batch() to fetch the next page.
    //
    // @param inp_range      Range to query.
    // @param batch_size     Max entries per batch.
    // @param filter         Optional per-entry filter.
    // @param reverse_order  Iterate from high to low key if true.
    // @param query_type     Sweep vs tree-traversal vs serializable.
    //
    // @return QueryResultHandle whose has_more() == true if further pages exist.
    BtreeTask< QueryResultHandle< K, V > >
    query_traversal(BtreeKeyRange< K >&& inp_range, uint32_t batch_size = std::numeric_limits< uint32_t >::max(),
                    GetFilter* filter = nullptr, bool reverse_order = false,
                    BtreeQueryType query_type = BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY);

    // @brief Fetch the next page from a QueryResultHandle returned by query_traversal().
    //
    // @param handle  Handle returned by query_traversal() or a previous query_next_batch() call.
    //                Must have has_more() == true.
    //
    // @return New QueryResultHandle for this page.  has_more() == false on the final page.
    BtreeTask< QueryResultHandle< K, V > > query_next_batch(QueryResultHandle< K, V >&& handle);

    nlohmann::json get_status(int log_level) const;

    nlohmann::json get_metrics_in_json(bool updated);

    BtreeTask< std::string > to_string() const;

    BtreeTask< std::string > to_custom_string(NodeCore::ToStringCallback< K, V > cb) const;

    BtreeTask< std::string > to_digraph_visualize_format() const;

    BtreeTask< void > dump(const std::string& file, std::string format = "string",
                           NodeCore::ToStringCallback< K, V > cb = nullptr) const;

    bnodeid_t root_node_id() const;

    BtreeTask< uint64_t > count_keys(bnodeid_t start_bnodeid = empty_bnodeid) const;

private:
    /////////////////////////////////// Mutate Impl methods /////////////////////////
    template < typename ReqT >
    BtreeTask< btree_status_t > put(ReqT& put_req);

    template < typename ReqT >
    BtreeTask< btree_status_t > do_put(Node my_node, ReqT& req); // owns the lock; RAII unlock on return

    template < typename ReqT >
    btree_status_t mutate_write_leaf_node(Node const& my_node, ReqT& req); // sync: no async calls

    template < typename ReqT >
    BtreeTask< btree_status_t > check_split_root(ReqT& req);

    template < typename ReqT >
    bool is_split_needed(Node const& node, ReqT& req) const; // sync: no async calls

    btree_status_t split_node(Node const& parent_node, Node const& child_node, uint32_t parent_ind,
                              K* out_split_key); // sync: no async calls

    ///////////////////////////////// Get Impl Methods /////////////////////////////////
    template < typename ReqT >
    BtreeTask< btree_status_t > get(ReqT& get_req);

    template < typename ReqT >
    BtreeTask< btree_status_t > do_get(Node my_node, ReqT& greq); // owns the lock

    ///////////////////////////////// Remove Impl Methods /////////////////////////////////
    template < typename ReqT >
    BtreeTask< btree_status_t > remove(ReqT& rreq);

    template < typename ReqT >
    BtreeTask< btree_status_t > do_remove(Node my_node, ReqT& rreq); // owns the lock

    template < typename ReqT >
    BtreeTask< btree_status_t > check_collapse_root(ReqT& rreq);

    BtreeTask< btree_status_t > merge_nodes(Node const& parent_node, Node const& leftmost_node,
                                            uint32_t start_indx, uint32_t end_indx);

    ///////////////////////////////// Query Impl Methods /////////////////////////////////
    BtreeTask< btree_status_t > query(BtreeQueryRequest< K >& query_req,
                                      std::vector< std::pair< K, V > >& out_values);

    BtreeTask< btree_status_t > do_sweep_query(Node my_node, BtreeQueryRequest< K >& qreq,
                                               std::vector< std::pair< K, V > >& out_values); // owns the lock

    BtreeTask< btree_status_t > do_traversal_query(Node my_node, BtreeQueryRequest< K >& qreq,
                                                   std::vector< std::pair< K, V > >& out_values); // owns the lock

    PutFilterDecision apply_put_filter(Node const& node, uint32_t idx, PutFilter* filter) const; // sync

#ifdef SERIALIZABLE_QUERY_IMPLEMENTATION
    btree_status_t do_serialzable_query(Node const& my_node, BtreeSerializableQueryRequest& qreq,
                                        std::vector< std::pair< K, V > >& out_values);
    btree_status_t sweep_query(BtreeQueryRequest< K >& qreq, std::vector< std::pair< K, V > >& out_values);
    btree_status_t serializable_query(BtreeSerializableQueryRequest& qreq,
                                      std::vector< std::pair< K, V > >& out_values);
#endif

    /////////////////////////////// Internal Node Management Methods ////////////////////////////////////
    virtual NodeCore* alloc_node_core(bnodeid_t id, bool is_leaf) const override;
    virtual NodeCore* load_node_core(uint8_t* node_buf, bnodeid_t id) const override;
    virtual Node      clone_temp_node(NodeCore const& node) override;

    /////////////////////////////////// Helper Methods ///////////////////////////////////////
    BtreeTask< btree_status_t > post_order_traversal(LockType acq_lock, const auto& cb);
    BtreeTask< btree_status_t > post_order_traversal(Node node, LockType acq_lock, const auto& cb); // owns the lock
    BtreeTask< void > get_all_kvs(std::vector< std::pair< K, V > >& kvs) const;
    BtreeTask< uint64_t > get_btree_node_cnt() const;
    BtreeTask< uint64_t > get_child_node_cnt(bnodeid_t bnodeid) const;
    BtreeTask< void > to_string_internal(bnodeid_t bnodeid, std::string& buf) const;
    BtreeTask< void > to_custom_string_internal(bnodeid_t bnodeid, std::string& buf,
                                                NodeCore::ToStringCallback< K, V > const& cb) const;
    BtreeTask< void > to_dot_keys(bnodeid_t bnodeid, std::string& buf,
                                  std::map< uint32_t, std::vector< uint64_t > >& l_map,
                                  std::map< uint64_t, BtreeVisualizeVariables >& info_map) const;
    void validate_sanity_child(Node const& parent_node, uint32_t ind) const;
    void validate_sanity_next_child(Node const& parent_node, uint32_t ind) const;
    BtreeTask< void > print_node(const bnodeid_t& bnodeid) const;

    void append_route_trace(BtreeRequest& req, Node const& node, btree_event_t event, uint32_t start_idx = 0,
                            uint32_t end_idx = 0) const;

protected:
    mutable BtreeMutex m_btree_lock;
    std::atomic< bool > m_destroyed{false};

#ifdef BTREE_SYNC_MODE
    auto lock_tree_shared() const { return BtreeSharedLockGuard{m_btree_lock}; }
    auto lock_tree_excl()   const { return BtreeExclLockGuard{m_btree_lock}; }
#else
    auto lock_tree_shared() const { return m_btree_lock.co_rlock(); }
    auto lock_tree_excl()   const { return m_btree_lock.co_wlock(); }
#endif
};
} // namespace homestore
