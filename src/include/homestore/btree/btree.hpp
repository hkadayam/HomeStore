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

#include <boost/intrusive_ptr.hpp>
#include <folly/small_vector.h>
#include <iomgr/fiber_lib.hpp>

#include <homestore/btree/detail/btree_req.hpp>
#include <homestore/btree/btree_kv.hpp>
#include <homestore/btree/detail/btree_internal.hpp>
#include <homestore/btree/detail/btree_node.hpp>
#include <homestore/index_service.hpp>
#include <homestore/btree/btree_base.hpp>

namespace homestore {

class BtreeStore;

template < typename K, typename V >
class Btree : public BtreeBase {
public:
    /////////////////////////////////////// All External APIs /////////////////////////////
    Btree(BtreeConfig const& cfg, uuid_t uuid = uuid_t{}, uuid_t parent_uuid = uuid_t{}, uint32_t user_sb_size = 0);
    Btree(BtreeConfig const& cfg, superblk< IndexSuperBlock >&& sb);
    virtual ~Btree();

    // Destroy the entire btree from persistent and from memory. It is to be noted that all blocks are not destroyed at
    // one go. For persistent btree, it might be a staged operation on multiple checkpoints.
    void destroy() override;

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
    btree_status_t put_one(BtreeKey const& key, BtreeValue const& value, btree_put_type put_type,
                           BtreeValue* existing_val = nullptr, put_filter_cb_t filter_cb = nullptr);

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
    // About batch size:
    // The batch size is the number of keys that will be processed in one go. It will return with btree_status::has_more
    // and the caller is expected to call put_range_next() method with the cookie passed to resume the next batch until
    // it returns btree_status::success. It is to be noted that, the batch size is a best effort from the btree and at
    // any iteration it could put between 1 to batch_size keys (it will at least put one_key and at most batch_size keys
    // per iteration).
    //
    // @param inp_range The range of keys to insert, upsert or update
    // @param put_type The type of put operation (e.g., insert, update, upsert).
    // @param value The value to be associated with the key. Behavior is different for interval and non-interval keys
    // (see above)
    // @param batch_size The number of keys to process in one go. Default is to attempt to process all keys in one go.
    // Please see the note above about the batch size.
    // @param filter_cb Optional callback function to apply a filter before insertion. (See above for details)
    //
    // @return The status of the put operation and a cookie, if it returns btree_status::has_more, then the caller is
    // expected to call put_range_next()
    std::pair< btree_status_t, PutPaginateCookie >
    put_range(BtreeKeyRange< K >&& inp_range, btree_put_type put_type, BtreeValue const& value,
              uint32_t batch_size = std::numeric_limts< uint32_t >::max(), put_filter_cb_t filter_cb = nullptr);

    // @brief Continuation of the put_range call for the next batch of keys. Calling this method without calling
    // put_range first returns error.
    //
    // @param cookie The cookie returned by the put_range call
    //
    // @return The status of the put operation and a cookie, if it returns btree_status::has_more, then the caller is
    // expected to call put_range_next() again. Failing to do so will result in memory leak.
    btree_status_t put_range_next(PutPaginateCookie& cookie);

    // @brief Gets the value associated with the specified key from the B-tree.
    //
    // @param key The key to search for.
    // @param out_val A pointer to store the value associated with the key. (Should be non-nullptr)
    //
    // @return The status of the get operation.
    btree_status_t get_one(BtreeKey const& key, BtreeValue* out_val);

    // @brief Gets any one value associated with the given key range. If the key range matches multiple keys, then btree
    // will randomly pick one key and return the value associated with it.
    //
    // @param inp_range The range of keys to search for.
    // @param out_key A pointer to store the picked key of the entry found. (Should be non-nullptr)
    // @param out_val A pointer to store the value associated with the picked key. (Should be non-nullptr)
    //
    // @return The status of the get_any operation.
    btree_status_t get_any(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val);

    // @brief Removes the key-value pair associated with the specified key from the B-tree.
    //
    // @param key The key to remove.
    // @param out_val An optional pointer to store the value associated with the key before removal.
    //
    // @return The status of the remove operation.
    btree_status_t remove_one(BtreeKey const& key, BtreeValue* out_val);

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
    btree_status_t remove_any(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val);

    std::pair< btree_status_t, RemovePaginateCookie >
    remove_range(BtreeKeyRange< K >&& inp_range, uint32_t batch_size = std::numeric_limts< uint32_t >::max(),
                 remove_filter_cb_t filter_cb = nullptr);

    btree_status_t remove_range_next(RemovePaginateCookie& cookie);

    std::pair< btree_status_t, QueryPaginateCookie >
    query(BtreeKeyRange< K >&& inp_range,                              // Input range to query for
          std::vector< std::pair< K, V > >& out_kvs,                   // Results will be appended
          uint32_t batch_size = std::numeric_limts< uint32_t >::max(), // Batch size, default the whole set
          get_filter_cb_t filter_cb = nullptr, // Any filtering condition while picking the result set
          BtreeQueryType query_type =
              BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY // See query_impl for more details
    );

    btree_status_t query_next(QueryPaginateCookie& cookie, std::vector< std::pair< K, V > >& out_kvs);

    nlohmann::json get_status(int log_level) const;

    nlohmann::json get_metrics_in_json(bool updated);

    std::string to_string() const;
    std::string to_custom_string(to_string_cb_t< K, V > const& cb) const;
    std::string to_digraph_visualizer_format() const;
    void dump(const std::string& file, std::string format = "string", to_string_cb_t< K, V > cb = nullptr);

    bnodeid_t root_node_id() const;

    uint64_t count_keys(bnodeid_t start_bnodeid = empty_bnodeid) const;

protected:
    BtreeNode* init_node(uint8_t* node_buf, bnodeid_t id, bool init_buf, bool is_leaf, uint32_t ctx_size) override;

private:
    /////////////////////////////////// Mutate Impl methods /////////////////////////
    template < typename ReqT >
    btree_status_t put(ReqT& put_req);

    ///////// Mutate Impl Methods
    template < typename ReqT >
    btree_status_t do_put(const BtreeNodePtr& my_node, locktype_t curlock, ReqT& req);

    template < typename ReqT >
    btree_status_t mutate_write_leaf_node(const BtreeNodePtr& my_node, ReqT& req);

    template < typename ReqT >
    btree_status_t check_split_root(ReqT& req);

    template < typename ReqT >
    bool is_split_needed(const BtreeNodePtr& node, ReqT& req) const;

    btree_status_t split_node(const BtreeNodePtr& parent_node, const BtreeNodePtr& child_node, uint32_t parent_ind,
                              K* out_split_key, CPContext* context);

    ///////////////////////////////// Get Impl Methods /////////////////////////////////
    template < typename ReqT >
    btree_status_t get(ReqT& get_req) const;

    template < typename ReqT >
    btree_status_t do_get(const BtreeNodePtr& my_node, ReqT& greq) const;

    ///////////////////////////////// Remove Impl Methods /////////////////////////////////
    template < typename ReqT >
    btree_status_t remove(ReqT& rreq);

    template < typename ReqT >
    btree_status_t do_remove(const BtreeNodePtr& my_node, locktype_t curlock, ReqT& rreq);

    template < typename ReqT >
    btree_status_t check_collapse_root(ReqT& rreq);

    btree_status_t merge_nodes(const BtreeNodePtr& parent_node, const BtreeNodePtr& leftmost_node, uint32_t start_indx,
                               uint32_t end_indx, CPContext* context);

    ///////////////////////////////// Query Impl Methods /////////////////////////////////
    btree_status_t query(BtreeQueryRequest< K >& query_req, std::vector< std::pair< K, V > >& out_values) const;

    btree_status_t do_sweep_query(BtreeNodePtr& my_node, BtreeQueryRequest< K >& qreq,
                                  std::vector< std::pair< K, V > >& out_values) const;
    btree_status_t do_traversal_query(const BtreeNodePtr& my_node, BtreeQueryRequest< K >& qreq,
                                      std::vector< std::pair< K, V > >& out_values) const;
#ifdef SERIALIZABLE_QUERY_IMPLEMENTATION
    btree_status_t do_serialzable_query(const BtreeNodePtr& my_node, BtreeSerializableQueryRequest& qreq,
                                        std::vector< std::pair< K, V > >& out_values);
    btree_status_t sweep_query(BtreeQueryRequest< K >& qreq, std::vector< std::pair< K, V > >& out_values);
    btree_status_t serializable_query(BtreeSerializableQueryRequest& qreq,
                                      std::vector< std::pair< K, V > >& out_values);
#endif

    // bool verify_tree(bool update_debug_bm) const;
    // void set_root_node_info(const BtreeLinkInfo& info);

    // static void set_io_flip();
    // static void set_error_flip();
#ifdef _PRERELEASE
    void set_flip_point(std::string flip) { m_flips.set_flip(flip); }
    void set_flips(std::vector< std::string > flips) {
        for (const auto& flip : flips) {
            set_flip_point(flip);
        }
    }
    std::string flip_list() const { return m_flips.list(); }
#endif

    /////////////////////////// Methods the application use case is expected to handle ///////////////////////////

private:
    /////////////////////////////// Internal Node Management Methods ////////////////////////////////////
    btree_status_t create_root_node();
    btree_status_t read_and_lock_node(bnodeid_t id, BtreeNodePtr& node_ptr, locktype_t int_lock_type,
                                      locktype_t leaf_lock_type, CPContext* context) const;
    btree_status_t get_child_and_lock_node(const BtreeNodePtr& node, uint32_t index, BtreeLinkInfo& child_info,
                                           BtreeNodePtr& child_node, locktype_t int_lock_type,
                                           locktype_t leaf_lock_type, CPContext* context) const;
    btree_status_t write_node(const BtreeNodePtr& node, CPContext* context);
    void read_node_or_fail(bnodeid_t id, BtreeNodePtr& node) const;

    btree_status_t upgrade_node_locks(const BtreeNodePtr& parent_node, const BtreeNodePtr& child_node,
                                      locktype_t& parent_cur_lock, locktype_t& child_cur_lock, CPContext* context);
    btree_status_t upgrade_node_lock(const BtreeNodePtr& node, locktype_t& cur_lock, CPContext* context);
    btree_status_t _lock_node(const BtreeNodePtr& node, locktype_t type, CPContext* context, const char* fname,
                              int line) const;
    void unlock_node(const BtreeNodePtr& node, locktype_t type) const;

    BtreeNodePtr create_leaf_node();
    BtreeNodePtr create_interior_node();
    BtreeNode* init_node(uint8_t* node_buf, bnodeid_t id, bool init_buf, bool is_leaf,
                         uint32_t ctx_size) const override;

    void remove_node(const BtreeNodePtr& node, locktype_t cur_lock, CPContext* context);

    void observe_lock_time(const BtreeNodePtr& node, locktype_t type, uint64_t time_spent) const;

    static void _start_of_lock(const BtreeNodePtr& node, locktype_t ltype, const char* fname, int line);
    static bool remove_locked_node(const BtreeNodePtr& node, locktype_t ltype, btree_locked_node_info* out_info);
    static uint64_t end_of_lock(const BtreeNodePtr& node, locktype_t ltype);

#ifndef NDEBUG
    static void check_lock_debug();
#endif

    /////////////////////////////////// Helper Methods ///////////////////////////////////////
    btree_status_t post_order_traversal(locktype_t acq_lock, const auto& cb);
    btree_status_t post_order_traversal(const BtreeNodePtr& node, locktype_t acq_lock, const auto& cb);
    void get_all_kvs(std::vector< std::pair< K, V > >& kvs) const;
    btree_status_t do_destroy();
    uint64_t get_btree_node_cnt() const;
    uint64_t get_child_node_cnt(bnodeid_t bnodeid) const;
    void to_string_internal(bnodeid_t bnodeid, std::string& buf) const;
    void to_custom_string_internal(bnodeid_t bnodeid, std::string& buf, to_string_cb_t< K, V > const& cb) const;
    void to_dot_keys(bnodeid_t bnodeid, std::string& buf, std::map< uint32_t, std::vector< uint64_t > >& l_map,
                     std::map< uint64_t, BtreeVisualizeVariables >& info_map) const;
    void validate_sanity_child(const BtreeNodePtr& parent_node, uint32_t ind) const;
    void validate_sanity_next_child(const BtreeNodePtr& parent_node, uint32_t ind) const;
    void print_node(const bnodeid_t& bnodeid) const;

    void append_route_trace(BtreeRequest& req, const BtreeNodePtr& node, btree_event_t event, uint32_t start_idx = 0,
                            uint32_t end_idx = 0) const;

protected:
    mutable iomgr::FiberManagerLib::shared_mutex m_btree_lock;
    BtreeLinkInfo m_root_node_info;

    BtreeMetrics m_metrics;
    std::atomic< bool > m_destroyed{false};
    std::atomic< uint64_t > m_total_nodes{0};
#ifndef NDEBUG
    std::atomic< uint64_t > m_req_id{0};
#endif
#ifdef _PRERELEASE
    BTREE_FLIPS m_flips;
#endif
    // This workaround of BtreeThreadVariables is needed instead of directly declaring statics
    // to overcome the gcc bug, pointer here: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66944
    static BtreeThreadVariables* bt_thread_vars() {
        auto this_id(boost::this_fiber::get_id());
        static thread_local std::map< boost::fibers::fiber::id, std::unique_ptr< BtreeThreadVariables > > fiber_map;
        if (fiber_map.count(this_id)) { return fiber_map[this_id].get(); }
        fiber_map[this_id] = std::make_unique< BtreeThreadVariables >();
        return fiber_map[this_id].get();
    }
};
} // namespace homestore
