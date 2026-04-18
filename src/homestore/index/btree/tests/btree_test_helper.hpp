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

#include <random>
#include <map>
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <boost/algorithm/string.hpp>

#include <folly/coro/Task.h>
#include <folly/coro/BlockingWait.h>

#include "sisl/options/options.h"
#include "sisl/logging/logging.h"
#include "sisl/fds/enum.h"
#include "iomanager/iomanager.h"
#include "homestore/index/btree/btree.ipp"
#include "shadow_map.hpp"

static constexpr uint32_t g_node_size{4096};

struct BtreeTestOptions {
    uint32_t num_entries;
    uint32_t preload_size;
    uint32_t num_ios;
    uint32_t run_time_secs;
    bool disable_merge{false};
};

template < typename TestType >
struct BtreeTestHelper {
    using T = TestType;
    using K = typename TestType::KeyType;
    using V = typename TestType::ValueType;
    using op_func_t = std::function< void(void) >;

    BtreeTestHelper(BtreeTestOptions options) : options_{std::move(options)}, shadow_map_{options.num_entries} {
        cfg_.leaf_node_type_ = T::leaf_node_type;
        cfg_.int_node_type_ = T::interior_node_type;
        // NOTE: BtreeConfig no longer carries a store_type; backend selection is done by the factory the test calls.
    }

    virtual void SetUp(std::shared_ptr< Btree< K, V > > bt, bool load, bool is_multi_threaded = false) {
        bt_ = std::move(bt);
        shadow_filename_ = fmt::format("/tmp/btree_shadow_map");

        if (!load) {
            std::filesystem::remove(shadow_filename_);
        }
        max_range_input_ = options_.num_entries;
        is_multi_threaded_ = is_multi_threaded;
        if (options_.disable_merge) {
            cfg_.merge_turned_on_ = false;
        }

        // In the new iomanager API there are no fibers — concurrent tasks run one per reactor thread.  We save the
        // reactor count so run_in_parallel / preload can spawn parallel coroutines across all reactors.
        if (is_multi_threaded_) {
            num_workers_ = iomgr().num_reactors();
        } else {
            num_workers_ = 1;
        }

        operations_["put"] = std::bind(&BtreeTestHelper::put_random, this);
        operations_["remove"] = std::bind(&BtreeTestHelper::remove_random, this);
        operations_["range_put"] = std::bind(&BtreeTestHelper::range_put_random, this);
        operations_["range_remove"] = std::bind(&BtreeTestHelper::range_remove_existing_random, this);
        operations_["query"] = std::bind(&BtreeTestHelper::query_random, this);
    }

    void TearDown() {}

public:
    std::shared_ptr< Btree< K, V > > bt_;
    BtreeConfig cfg_;

protected:
    BtreeTestOptions const options_;
    ShadowMap< K, V > shadow_map_;
    uint32_t max_range_input_{1000};
    bool is_multi_threaded_{false};

    std::map< std::string, op_func_t > operations_;
    // Number of parallel workers (= num_reactors in multi-threaded mode, 1 otherwise).  Replaces the old fibers_ list.
    size_t num_workers_{1};
    std::mutex test_done_mtx_;
    std::condition_variable test_done_cv_;
    std::random_device re_;
    std::atomic< uint32_t > num_ops_{0};
    Clock::time_point start_time_;
    std::string shadow_filename_;

#ifdef _PRERELEASE
    flip::FlipClient fc_{iomgr_flip::instance()};
#endif
public:
#ifdef _PRERELEASE
    void set_flip_point(const std::string flip_name) {
        flip::FlipCondition null_cond;
        flip::FlipFrequency freq;
        freq.set_count(10000);
        freq.set_percent(100);
        fc_.inject_noreturn_flip(flip_name, {null_cond}, freq);
        bt_->set_flip_point(flip_name);
        LOGINFO("Flip {} set", flip_name);
    }
    void reset_flip_point(const std::string flip_name) {
        fc_.remove_flip(flip_name);
        LOGINFO("Flip {} reset", flip_name);
    }
#endif

    void preload(uint32_t preload_size) {
        if (preload_size == 0) {
            LOGINFO("Preload Skipped");
            return;
        }

        const auto n_workers = std::min(preload_size, (uint32_t)num_workers_);
        const auto chunk_size = preload_size / n_workers;
        const auto last_chunk_size = preload_size % chunk_size ?: chunk_size;
        std::atomic< uint32_t > test_count{n_workers};

        LOGINFO("{} entries will be preloaded across {} workers in parallel", preload_size, n_workers);
        start_time_ = Clock::now();
        for (std::size_t i = 0; i < n_workers; ++i) {
            const auto start_range = i * chunk_size;
            const auto end_range = start_range + ((i == n_workers - 1) ? last_chunk_size : chunk_size) - 1;
            // One coroutine per reactor; the body is synchronous put/track_progress but runs concurrently with
            // other reactors' coroutines.
            iomgr().spawn_detached(
                ReactorTarget::reactor(i),
                [this, start_range, end_range, &test_count, preload_size]() -> folly::coro::Task< void > {
                    for (uint32_t k = start_range; k < end_range; k++) {
                        put(k, BtreePutType::INSERT);
                        track_progress(preload_size, "Preload");
                    }
                    {
                        std::unique_lock lg(test_done_mtx_);
                        if (test_count.fetch_sub(1) == 1) {
                            test_done_cv_.notify_one();
                        }
                    }
                    co_return;
                });
        }

        {
            std::unique_lock< std::mutex > lk(test_done_mtx_);
            test_done_cv_.wait(lk, [&]() { return test_count.load() == 0; });
        }

        LOGINFO("Preload Done");
    }

    uint32_t get_op_num() const {
        return num_ops_.load();
    }

    void track_progress(uint32_t max_ops, std::string_view work_type) {
        static Clock::time_point last_print_time{Clock::now()};

        bool print{false};
        auto completed = num_ops_.fetch_add(1) + 1;

        auto elapsed_time = get_elapsed_time_sec(last_print_time);
        if (elapsed_time > 30) {
            // Print percent every 30 seconds no matter what
            print = true;
        } else if ((completed % (max_ops / 10) == 0) && (elapsed_time > 1)) {
            // 10% completed and at least 1 second after last print time, we can print again
            print = true;
        }

        if (print) {
            auto map_size = shadow_map_.size();
            LOGINFO("Progress=({:.2f}%) IOsCompleted={} ElapsedTime={} seconds {} EntriesFilled={} ({:.2f}%)",
                    completed * 100.0 / max_ops, completed, get_elapsed_time_sec(start_time_), work_type, map_size,
                    map_size * 100.0 / max_range_input_);
            last_print_time = Clock::now();
        }
    }

    ////////////////////// All put operation variants ///////////////////////////////
    void put(uint64_t k, BtreePutType put_type, bool expect = true) {
        do_put(k, put_type, V::generate_rand(), expect);
    }

    void put_random() {
        auto [start_k, end_k] = shadow_map_.pick_random_non_existing_keys(1);
        RELEASE_ASSERT_EQ(start_k, end_k, "Range scheduler pick_random_non_existing_keys issue");

        do_put(start_k, BtreePutType::INSERT, V::generate_rand());
    }

    void force_upsert(uint64_t k) {
        auto existing_v = std::make_unique< V >();
        K key = K{k};
        V value = V::generate_rand();

        auto result = bt_->put_one(key, value, BtreePutType::UPSERT, existing_v.get());
        ASSERT_TRUE(result.hasValue()) << "Upsert key=" << k << " failed";
        shadow_map_.force_put(k, value);
    }

    void put_delta(uint64_t k) {
        K key{k};
        auto it = shadow_map_.map_const().find(key);
        ASSERT_TRUE(it != shadow_map_.map_const().cend())
            << "Asked to put_delta for key=" << k << " but its not in the map";

        auto existing_v = std::make_unique< V >();
        auto result = bt_->put_one(key, it->second, BtreePutType::UPSERT, existing_v.get());
        ASSERT_TRUE(result.hasValue()) << "Upsert key=" << k << " failed";
    }

    void range_put(uint32_t start_k, uint32_t end_k, V const& value, bool update) {
        K start_key = K{start_k};
        K end_key = K{end_k};
        auto const nkeys = end_k - start_k + 1;

        auto result = bt_->put_range(BtreeKeyRange< K >{start_key, true, end_key, true},
                                     update ? BtreePutType::UPDATE : BtreePutType::UPSERT, value);
        ASSERT_TRUE(result.hasValue()) << "range_put failed for " << start_k << "-" << end_k;

        if (update) {
            shadow_map_.range_update(start_key, nkeys, value);
        } else {
            shadow_map_.range_upsert(start_k, nkeys, value);
        }
    }

    void range_put_random() {
        bool is_update{true};
        if constexpr (std::is_same_v< V, TestIntervalValue >) {
            is_update = false;
        }

        static thread_local std::uniform_int_distribution< uint32_t > s_rand_range_generator{1, 50};

        auto const [start_k, end_k] = is_update ? shadow_map_.pick_random_existing_keys(s_rand_range_generator(re_))
                                                : shadow_map_.pick_random_non_working_keys(s_rand_range_generator(re_));
        if (start_k == UINT32_MAX) {
            return;
        } // no keys available — skip

        range_put(start_k, end_k, V::generate_rand(), is_update);
    }

    ////////////////////// All remove operation variants ///////////////////////////////
    void remove_one(uint32_t k, bool care_success = true) {
        auto pk = std::make_unique< K >(k);

        // New API: remove_one returns Expected<V, BtreeStatus> — value on success holds the removed V.
        auto result = bt_->remove_one(*pk);
        bool removed = result.hasValue();
        if (care_success) {
            ASSERT_EQ(removed, shadow_map_.exists(*pk))
                << "Removal of key " << pk->key() << " status doesn't match with shadow";
            if (removed) {
                shadow_map_.remove_and_check(*pk, result.value());
            }
        } else {
            // Do not care if the key is not present in the btree, just cleanup the shadow map
            shadow_map_.erase(*pk);
        }
    }

    void remove_random() {
        auto const [start_k, end_k] = shadow_map_.pick_random_existing_keys(1);
        if (start_k == UINT32_MAX) {
            return;
        } // all keys removed or in-flight — skip
        RELEASE_ASSERT_EQ(start_k, end_k, "Range scheduler pick_random_existing_keys issue");

        remove_one(start_k);
    }

    void range_remove_existing(uint32_t start_k, uint32_t count) {
        auto [start_key, end_key] = shadow_map_.pick_existing_range(K{start_k}, count);
        do_range_remove(start_k, end_key.key(), true /* removing_all_existing */);
    }

    void range_remove_existing_random() {
        static std::uniform_int_distribution< uint32_t > s_rand_range_generator{2, 50};

        auto const [start_k, end_k] = shadow_map_.pick_random_existing_keys(s_rand_range_generator(re_));
        if (start_k == UINT32_MAX) {
            return; // all keys removed — skip
        }
        do_range_remove(start_k, end_k, true /* only_existing */);
    }

    void range_remove_any(uint32_t start_k, uint32_t end_k) {
        do_range_remove(start_k, end_k, false /* removing_all_existing */);
    }

    ////////////////////// All query operation variants ///////////////////////////////
    void query_all() {
        do_query(0u, options_.num_entries - 1, UINT32_MAX);
    }

    void query_all_paginate(uint32_t batch_size) {
        do_query(0u, options_.num_entries - 1, batch_size);
    }

    void do_query(uint32_t start_k, uint32_t end_k, uint32_t batch_size) {
        // New query API returns an Expected<QueryResultHandle<K,V>>; continuation is via query_next_batch(handle).
        // Hold shadow_map_ lock only for the validation loop; release before remove_keys_from_working (which also
        // locks the mutex internally).
        {
            std::lock_guard shadow_lock{shadow_map_.guard()};
            uint32_t remaining = shadow_map_.num_elems_in_range(start_k, end_k);
            auto it = shadow_map_.map_const().lower_bound(K{start_k});

            auto result = bt_->query(BtreeKeyRange< K >{K{start_k}, true, K{end_k}, true}, batch_size);
            while (remaining > 0) {
                ASSERT_TRUE(result.hasValue()) << "Query failed";
                auto const& out_vector = result.value().results;
                auto const expected_count = std::min(remaining, batch_size);
                ASSERT_EQ(out_vector.size(), expected_count) << "Received incorrect value on query pagination";

                if (remaining > batch_size) {
                    ASSERT_TRUE(result.value().has_more()) << "Expected query to return has_more";
                }
                remaining -= expected_count;

                for (size_t idx{0}; idx < out_vector.size(); ++idx) {
                    ASSERT_EQ(out_vector[idx].second, it->second)
                        << "Range get doesn't return correct data for key=" << it->first << " idx=" << idx;
                    ++it;
                }
                if (remaining == 0) {
                    break;
                }

                result = bt_->query_next_batch(std::move(result.value()));
            }
        } // shadow_lock released here

        if (start_k < max_range_input_) {
            shadow_map_.remove_keys_from_working(start_k, std::min(end_k, max_range_input_ - 1));
        }
    }

    void query_random() {
        static thread_local std::uniform_int_distribution< uint32_t > s_rand_range_generator{1, 100};

        auto const [start_k, end_k] = shadow_map_.pick_random_non_working_keys(s_rand_range_generator(re_));
        do_query(start_k, end_k, 79);
    }

    ////////////////////// All get operation variants ///////////////////////////////
    void get_all() const {
        shadow_map_.foreach ([this](K key, V value) {
            // get returns Expected<V, BtreeStatus>.
            auto result = bt_->get_one(key);
            ASSERT_TRUE(result.hasValue()) << "Missing key " << key << " in btree but present in shadow map";
            ASSERT_EQ(result.value(), value) << "Found value in btree doesn't return correct data for key=" << key;
        });
    }

    void get_specific(uint32_t k) const {
        K key = K{k};
        auto result = bt_->get_one(key);
        if (result.hasValue()) {
            shadow_map_.validate_data(key, result.value());
        } else {
            ASSERT_EQ(shadow_map_.exists(key), false) << "Node key " << k << " is missing in the btree";
        }
    }

    void get_any(uint32_t start_k, uint32_t end_k) const {
        // get_any returns Expected<pair<K,V>, BtreeStatus>.
        auto result = bt_->get_any(BtreeKeyRange< K >{K{start_k}, true, K{end_k}, true});
        if (result.hasValue()) {
            auto const& [out_k, out_v] = result.value();
            ASSERT_EQ(shadow_map_.exists_in_range(out_k, start_k, end_k), true)
                << "Get Any returned key=" << out_k << " which is not in range " << start_k << "-" << end_k
                << "according to shadow map";
            shadow_map_.validate_data(out_k, out_v);
        }
        // If get_any failed, we don't know which key it would have returned, so there's nothing to shadow-check.
    }

    void multi_op_execute(const std::vector< std::pair< std::string, int > >& op_list) {
        if (shadow_map_.size() == 0) {
            auto preload_size = options_.preload_size;
            if (preload_size > options_.num_entries / 2) {
                LOGWARN("Preload size={} is more than half of num_entries, setting preload_size to {}", preload_size,
                        options_.num_entries / 2);
                preload_size = options_.num_entries / 2;
            }
            preload(preload_size);
        }
        LOGINFO("{} IOs will be executed across {} workers in parallel", options_.num_ios, num_workers_);
        run_in_parallel(op_list);
        LOGINFO("{} IOs completed", options_.num_ios);
    }

    void dump_to_file(const std::string& file = "") const {
        bt_->dump(file);
    }
    void print_keys(const std::string& preamble = "") const {
        auto print_key_range = [](std::vector< std::pair< K, V > > const& kvs) -> std::string {
            uint32_t start = 0;
            std::string str;
            for (uint32_t i{1}; i <= kvs.size(); ++i) {
                if ((i == kvs.size()) || (kvs[i].first.key() != kvs[i - 1].first.key() + 1)) {
                    if ((i - start) > 1) {
                        fmt::format_to(std::back_inserter(str), "{}-{}{}", kvs[start].first.key(),
                                       kvs[i - 1].first.key(), (i == kvs.size()) ? "" : ", ");
                    } else {
                        fmt::format_to(std::back_inserter(str), "{}{}", kvs[start].first.key(),
                                       (i == kvs.size()) ? "" : ", ");
                    }
                    start = i;
                }
            }
            return str;
        };

        LOGINFO("{}{}", preamble.empty() ? "" : preamble + ":\n", bt_->to_custom_string(print_key_range));
    }
    void visualize_keys(const std::string& file) const {
        bt_->visualize_tree_keys(file);
    }

    void compare_files(const std::string& before, const std::string& after) {
        std::ifstream b(before, std::ifstream::ate);
        std::ifstream a(after, std::ifstream::ate);
        if (a.fail() || b.fail()) {
            LOGINFO("Failed to open file");
            assert(false);
        }
        if (a.tellg() != b.tellg()) {
            LOGINFO("Mismatch in btree files");
            assert(false);
        }

        int64_t pending = a.tellg();
        const int64_t batch_size = 4096;
        a.seekg(0, std::ifstream::beg);
        b.seekg(0, std::ifstream::beg);
        char a_buffer[batch_size], b_buffer[batch_size];
        while (pending > 0) {
            auto count = std::min(pending, batch_size);
            a.read(a_buffer, count);
            b.read(b_buffer, count);
            if (std::memcmp(a_buffer, b_buffer, count) != 0) {
                LOGINFO("Mismatch in btree files");
                assert(false);
            }
            pending -= count;
        }
    }

    ///////////////////////// All crash recovery methods ///////////////////////////////////
    void save_snapshot() {
        this->shadow_map_.save(shadow_filename_);
    }

    void reapply_after_crash() {
        ShadowMap< K, V > snapshot_map{shadow_map_.max_keys()};
        snapshot_map.load(shadow_filename_);
        LOGDEBUG("Snapshot before crash\n{}", snapshot_map.to_string());

        auto diff = shadow_map_.diff(snapshot_map);
        std::string dif_str;
        for (const auto& [k, delta] : diff) {
            dif_str += fmt::format("[{}-{}] ", k.key(), enum_name(delta));
        }
        LOGDEBUG("Diff between shadow map and snapshot map\n{}\n", dif_str);

        for (const auto& [k, delta] : diff) {
            if ((delta == ShadowMapDelta::Added) || (delta == ShadowMapDelta::Updated)) {
                this->put_delta(k.key());
            } else if (delta == ShadowMapDelta::Removed) {
                this->remove_one(k.key(), false);
            }
        }
    }

private:
    void do_put(uint64_t k, BtreePutType put_type, V const& value, bool expect_success = true) {
        auto existing_v = std::make_unique< V >();
        K key = K{k};
        // put_one returns Expected<PutStats, BtreeStatus>.  "done" means the put landed as the caller expected:
        // success path checks hasValue(); failure path checks that it was rejected (key_already_exists for INSERT,
        // not_found for UPDATE).
        auto result = bt_->put_one(key, value, put_type, existing_v.get());
        bool done = expect_success
            ? result.hasValue()
            : (!result.hasValue() &&
               (result.error() == BtreeStatus::key_already_exists || result.error() == BtreeStatus::key_not_found));

        if (put_type == BtreePutType::INSERT) {
            ASSERT_EQ(done, !shadow_map_.exists(key));
        } else if (put_type == BtreePutType::UPDATE) {
            ASSERT_EQ(done, shadow_map_.exists(key));
        }
        if (expect_success) {
            shadow_map_.put_and_check(key, value, *existing_v, done);
        }
    }

    void do_range_remove(uint64_t start_k, uint64_t end_k, bool all_existing) {
        K start_key = K{start_k};
        K end_key = K{end_k};

        // remove_range returns Expected<uint32_t /*removed_count*/, BtreeStatus>.
        auto result = bt_->remove_range(BtreeKeyRange< K >{start_key, true, end_key, true});
        if (all_existing) {
            shadow_map_.range_erase(start_key, end_key);
            ASSERT_TRUE(result.hasValue()) << "not a successful remove op for range " << start_k << "-" << end_k;
        } else if (start_k < max_range_input_) {
            K end_range{std::min(end_k, uint64_cast(max_range_input_ - 1))};
            shadow_map_.range_erase(start_key, end_range);
        }
    }

public:
    void run_in_parallel(const std::vector< std::pair< std::string, int > >& op_list) {
        std::atomic< size_t > test_count{num_workers_};
        const auto num_ios_per_worker = options_.num_ios / num_workers_;
        const auto extra_ios = options_.num_ios % num_workers_;

        num_ops_ = 0; // Reset the ops counter
        start_time_ = Clock::now();
        for (size_t worker_id = 0; worker_id < num_workers_; ++worker_id) {
            auto num_ios_this_worker = num_ios_per_worker + (worker_id < extra_ios ? 1 : 0);
            iomgr().spawn_detached(ReactorTarget::reactor(worker_id),
                                   [this, &test_count, op_list, num_ios_this_worker]() -> folly::coro::Task< void > {
                                       std::random_device g_rd{};
                                       std::default_random_engine re{g_rd()};
                                       std::vector< uint32_t > weights;
                                       std::transform(op_list.begin(), op_list.end(), std::back_inserter(weights),
                                                      [](const auto& pair) { return pair.second; });

                                       // Construct a weighted distribution based on the input frequencies
                                       std::discrete_distribution< uint32_t > s_rand_op_generator(weights.begin(),
                                                                                                  weights.end());
                                       auto time_to_stop = [this]() {
                                           return (get_elapsed_time_sec(start_time_) > options_.run_time_secs);
                                       };

                                       for (uint32_t i = 0; i < num_ios_this_worker && !time_to_stop(); i++) {
                                           uint32_t op_idx = s_rand_op_generator(re);
                                           (this->operations_[op_list[op_idx].first])();
                                           track_progress(options_.num_ios, "Workload");
                                       }
                                       {
                                           std::unique_lock lg(test_done_mtx_);
                                           if (test_count.fetch_sub(1) == 1) {
                                               test_done_cv_.notify_one();
                                           }
                                       }
                                       co_return;
                                   });
        }

        {
            std::unique_lock< std::mutex > lk(test_done_mtx_);
            test_done_cv_.wait(lk, [&]() { return test_count.load() == 0; });
        }
    }

    std::vector< std::pair< std::string, int > > build_op_list(std::vector< std::string > const& input_ops) {
        std::vector< std::pair< std::string, int > > ops;
        int total = std::accumulate(input_ops.begin(), input_ops.end(), 0, [](int sum, const auto& str) {
            std::vector< std::string > tokens;
            boost::split(tokens, str, boost::is_any_of(":"));
            if (tokens.size() == 2) {
                try {
                    return sum + std::stoi(tokens[1]);
                } catch (const std::exception&) {
                    // Invalid frequency, ignore this element
                }
            }
            return sum; // Ignore malformed strings
        });

        std::transform(input_ops.begin(), input_ops.end(), std::back_inserter(ops), [total](const auto& str) {
            std::vector< std::string > tokens;
            boost::split(tokens, str, boost::is_any_of(":"));
            if (tokens.size() == 2) {
                try {
                    return std::make_pair(tokens[0], (int)(100.0 * std::stoi(tokens[1]) / total));
                } catch (const std::exception&) {
                    // Invalid frequency, ignore this element
                }
            }
            return std::make_pair(std::string(), 0);
        });

        return ops;
    }
};
