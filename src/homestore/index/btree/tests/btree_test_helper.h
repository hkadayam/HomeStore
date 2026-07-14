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

#include "common/async.h"

#include "sisl/options/options.h"
#include "sisl/logging/logging.h"
#include "sisl/fds/enum.h"
#include "iomanager/iomanager.h"
#include "homestore/index/btree/btree.ipp"
#include "shadow_map.h"

static constexpr uint32_t g_node_size{4096};

// Mode-agnostic assertion helpers. CO_RETURN is `return` in sync btree mode and `co_return` in async mode, so these
// work in both helper functions returning `void` (sync) and `BtreeTask<void>` (async).
#define BTH_ASSERT_TRUE(cond)                                                                                          \
    do {                                                                                                               \
        EXPECT_TRUE(cond);                                                                                             \
        if (!(cond))                                                                                                   \
            CO_RETURN;                                                                                                 \
    } while (0)
#define BTH_ASSERT_FALSE(cond)                                                                                         \
    do {                                                                                                               \
        EXPECT_FALSE(cond);                                                                                            \
        if ((cond))                                                                                                    \
            CO_RETURN;                                                                                                 \
    } while (0)
#define BTH_ASSERT_EQ(a, b)                                                                                            \
    do {                                                                                                               \
        EXPECT_EQ(a, b);                                                                                               \
        if (!((a) == (b)))                                                                                             \
            CO_RETURN;                                                                                                 \
    } while (0)
#define BTH_ASSERT_NE(a, b)                                                                                            \
    do {                                                                                                               \
        EXPECT_NE(a, b);                                                                                               \
        if (!((a) != (b)))                                                                                             \
            CO_RETURN;                                                                                                 \
    } while (0)

// Tight co_await loops over fully-cached btree ops never suspend back to the executor, so the actor frames keep
// accumulating on the OS stack and overflow it after ~1000 iterations.  BTH_YIELD_PERIODIC() yields back to the
// executor on every Nth invocation (per-thread counter) to break the chain.  No-op in sync btree mode.
#ifdef BTREE_ASYNC_MODE
#define BTH_YIELD_PERIODIC()                                                                                           \
    do {                                                                                                               \
        thread_local uint32_t _bth_yield_counter{0};                                                                   \
        if ((++_bth_yield_counter & 0x7f) == 0)                                                                        \
            co_await iomgr().yield_now();                                                                              \
    } while (0)
#else
#define BTH_YIELD_PERIODIC() ((void)0)
#endif

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
    using op_func_t = std::function< BtreeTask< void >() >;

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
            // One coroutine per reactor; CO_AWAIT'd put() works in both sync and async btree modes.
            iomgr().spawn_detached(iomanager::ReactorTarget::reactor(i),
                                   [this, start_range, end_range, &test_count, preload_size]() -> Async< void > {
                                       for (uint32_t k = start_range; k < end_range; k++) {
                                           CO_AWAIT put(k, BtreePutType::INSERT);
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
    BtreeTask< void > put(uint64_t k, BtreePutType put_type, bool expect = true) {
        CO_RETURN CO_AWAIT do_put(k, put_type, V::generate_rand(), expect);
    }

    BtreeTask< void > put_random() {
        auto [start_k, end_k] = shadow_map_.pick_random_non_existing_keys(1);
        RELEASE_ASSERT_EQ(start_k, end_k, "Range scheduler pick_random_non_existing_keys issue");

        CO_RETURN CO_AWAIT do_put(start_k, BtreePutType::INSERT, V::generate_rand());
    }

    BtreeTask< void > force_upsert(uint64_t k) {
        auto existing_v = std::make_unique< V >();
        K key = K{k};
        V value = V::generate_rand();

        auto result = CO_AWAIT bt_->put_one(key, value, BtreePutType::UPSERT, existing_v.get());
        BTH_ASSERT_TRUE(result.hasValue());
        shadow_map_.force_put(k, value);
        CO_RETURN;
    }

    BtreeTask< void > put_delta(uint64_t k) {
        K key{k};
        auto it = shadow_map_.map_const().find(key);
        BTH_ASSERT_TRUE(it != shadow_map_.map_const().cend());

        auto existing_v = std::make_unique< V >();
        auto result = CO_AWAIT bt_->put_one(key, it->second, BtreePutType::UPSERT, existing_v.get());
        BTH_ASSERT_TRUE(result.hasValue());
        CO_RETURN;
    }

    BtreeTask< void > range_put(uint32_t start_k, uint32_t end_k, V const& value, bool update) {
        K start_key = K{start_k};
        K end_key = K{end_k};
        auto const nkeys = end_k - start_k + 1;

        auto result = CO_AWAIT bt_->put_range(BtreeKeyRange< K >{start_key, true, end_key, true},
                                              update ? BtreePutType::UPDATE : BtreePutType::UPSERT, value);
        BTH_ASSERT_TRUE(result.hasValue());

        if (update) {
            shadow_map_.range_update(start_key, nkeys, value);
        } else {
            shadow_map_.range_upsert(start_k, nkeys, value);
        }
        CO_RETURN;
    }

    BtreeTask< void > range_put_random() {
        bool is_update{true};
        if constexpr (std::is_same_v< V, TestIntervalValue >) {
            is_update = false;
        }

        static thread_local std::uniform_int_distribution< uint32_t > s_rand_range_generator{1, 50};

        auto const [start_k, end_k] = is_update
            ? shadow_map_.pick_random_existing_keys(s_rand_range_generator(g_re))
            : shadow_map_.pick_random_non_working_keys(s_rand_range_generator(g_re));
        if (start_k == UINT32_MAX) {
            CO_RETURN;
        } // no keys available — skip

        CO_RETURN CO_AWAIT range_put(start_k, end_k, V::generate_rand(), is_update);
    }

    ////////////////////// All remove operation variants ///////////////////////////////
    BtreeTask< void > remove_one(uint32_t k, bool care_success = true) {
        auto pk = std::make_unique< K >(k);

        // New API: remove_one returns Expected<V, BtreeStatus> — value on success holds the removed V.
        auto result = CO_AWAIT bt_->remove_one(*pk);
        bool removed = result.hasValue();
        if (care_success) {
            BTH_ASSERT_EQ(removed, shadow_map_.exists(*pk));
            if (removed) {
                shadow_map_.remove_and_check(*pk, result.value());
            }
        } else {
            // Do not care if the key is not present in the btree, just cleanup the shadow map
            shadow_map_.erase(*pk);
        }
        BTH_YIELD_PERIODIC();
        CO_RETURN;
    }

    BtreeTask< void > remove_random() {
        auto const [start_k, end_k] = shadow_map_.pick_random_existing_keys(1);
        if (start_k == UINT32_MAX) {
            CO_RETURN;
        } // all keys removed or in-flight — skip
        RELEASE_ASSERT_EQ(start_k, end_k, "Range scheduler pick_random_existing_keys issue");

        CO_RETURN CO_AWAIT remove_one(start_k);
    }

    BtreeTask< void > range_remove_existing(uint32_t start_k, uint32_t count) {
        auto [start_key, end_key] = shadow_map_.pick_existing_range(K{start_k}, count);
        CO_RETURN CO_AWAIT do_range_remove(start_k, end_key.key(), true /* removing_all_existing */);
    }

    BtreeTask< void > range_remove_existing_random() {
        static std::uniform_int_distribution< uint32_t > s_rand_range_generator{2, 50};

        auto const [start_k, end_k] = shadow_map_.pick_random_existing_keys(s_rand_range_generator(g_re));
        if (start_k == UINT32_MAX) {
            CO_RETURN; // all keys removed — skip
        }
        CO_RETURN CO_AWAIT do_range_remove(start_k, end_k, true /* only_existing */);
    }

    BtreeTask< void > range_remove_any(uint32_t start_k, uint32_t end_k) {
        CO_RETURN CO_AWAIT do_range_remove(start_k, end_k, false /* removing_all_existing */);
    }

    ////////////////////// All query operation variants ///////////////////////////////
    BtreeTask< void > query_all() {
        CO_RETURN CO_AWAIT do_query(0u, options_.num_entries - 1, UINT32_MAX);
    }

    BtreeTask< void > query_all_paginate(uint32_t batch_size) {
        CO_RETURN CO_AWAIT do_query(0u, options_.num_entries - 1, batch_size);
    }

    BtreeTask< void > do_query(uint32_t start_k, uint32_t end_k, uint32_t batch_size) {
        // Snapshot the expected (key,value) pairs under shadow lock before issuing async queries (we can't hold a
        // sync mutex across CO_AWAIT in async mode).
        std::vector< std::pair< K, V > > expected;
        uint32_t remaining;
        {
            std::lock_guard shadow_lock{shadow_map_.guard()};
            remaining = shadow_map_.num_elems_in_range(start_k, end_k);
            auto it = shadow_map_.map_const().lower_bound(K{start_k});
            expected.reserve(remaining);
            for (uint32_t i = 0; i < remaining && it != shadow_map_.map_const().cend(); ++i, ++it) {
                expected.emplace_back(it->first, it->second);
            }
        }

        auto result = CO_AWAIT bt_->query(BtreeKeyRange< K >{K{start_k}, true, K{end_k}, true}, batch_size);
        size_t expected_idx = 0;
        while (remaining > 0) {
            BTH_ASSERT_TRUE(result.hasValue());
            auto const& out_vector = result.value().results;
            auto const expected_count = std::min(remaining, batch_size);
            BTH_ASSERT_EQ(out_vector.size(), expected_count);

            if (remaining > batch_size) {
                BTH_ASSERT_TRUE(result.value().has_more());
            }
            remaining -= expected_count;

            for (size_t idx{0}; idx < out_vector.size(); ++idx) {
                BTH_ASSERT_EQ(out_vector[idx].first, expected[expected_idx].first);
                BTH_ASSERT_EQ(out_vector[idx].second, expected[expected_idx].second);
                ++expected_idx;
            }
            if (remaining == 0) {
                break;
            }

            result = CO_AWAIT bt_->query_next_batch(std::move(result.value()));
            BTH_YIELD_PERIODIC();
        }

        if (start_k < max_range_input_) {
            shadow_map_.remove_keys_from_working(start_k, std::min(end_k, max_range_input_ - 1));
        }
        CO_RETURN;
    }

    BtreeTask< void > query_random() {
        static thread_local std::uniform_int_distribution< uint32_t > s_rand_range_generator{1, 100};

        auto const [start_k, end_k] = shadow_map_.pick_random_non_working_keys(s_rand_range_generator(g_re));
        CO_RETURN CO_AWAIT do_query(start_k, end_k, 79);
    }

    ////////////////////// All get operation variants ///////////////////////////////
    BtreeTask< void > get_all() const {
        // Snapshot the (key,value) pairs first; we can't hold the foreach lambda across CO_AWAIT.
        std::vector< std::pair< K, V > > entries;
        shadow_map_.foreach ([&entries](K key, V value) { entries.emplace_back(std::move(key), std::move(value)); });

        for (auto const& [key, value] : entries) {
            auto result = CO_AWAIT bt_->get_one(key);
            BTH_ASSERT_TRUE(result.hasValue());
            BTH_ASSERT_EQ(result.value(), value);
            BTH_YIELD_PERIODIC();
        }
        CO_RETURN;
    }

    BtreeTask< void > get_specific(uint32_t k) const {
        K key = K{k};
        auto result = CO_AWAIT bt_->get_one(key);
        if (result.hasValue()) {
            shadow_map_.validate_data(key, result.value());
        } else {
            BTH_ASSERT_FALSE(shadow_map_.exists(key));
        }
        BTH_YIELD_PERIODIC();
        CO_RETURN;
    }

    BtreeTask< void > get_any(uint32_t start_k, uint32_t end_k) const {
        // get_any returns Expected<pair<K,V>, BtreeStatus>.
        auto result = CO_AWAIT bt_->get_any(BtreeKeyRange< K >{K{start_k}, true, K{end_k}, true});
        if (result.hasValue()) {
            auto const& [out_k, out_v] = result.value();
            BTH_ASSERT_TRUE(shadow_map_.exists_in_range(out_k, start_k, end_k));
            shadow_map_.validate_data(out_k, out_v);
        }
        // If get_any failed, we don't know which key it would have returned, so there's nothing to shadow-check.
        CO_RETURN;
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
        folly::coro::blockingWait(bt_->dump(file));
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

        LOGINFO("{}{}", preamble.empty() ? "" : preamble + ":\n",
                folly::coro::blockingWait(bt_->to_custom_string(print_key_range)));
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

    BtreeTask< void > reapply_after_crash() {
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
                CO_AWAIT this->put_delta(k.key());
            } else if (delta == ShadowMapDelta::Removed) {
                CO_AWAIT this->remove_one(k.key(), false);
            }
        }
        CO_RETURN;
    }

private:
    BtreeTask< void > do_put(uint64_t k, BtreePutType put_type, V const& value, bool expect_success = true) {
        auto existing_v = std::make_unique< V >();
        K key = K{k};
        // put_one returns Expected<PutStats, BtreeStatus>.  "done" means the put landed as the caller expected:
        // success path checks hasValue(); failure path checks that it was rejected (key_already_exists for INSERT,
        // not_found for UPDATE).
        auto result = CO_AWAIT bt_->put_one(key, value, put_type, existing_v.get());
        bool done = expect_success
            ? result.hasValue()
            : (!result.hasValue() &&
               (result.error() == BtreeStatus::key_already_exists || result.error() == BtreeStatus::key_not_found));

        if (put_type == BtreePutType::INSERT) {
            BTH_ASSERT_EQ(done, !shadow_map_.exists(key));
        } else if (put_type == BtreePutType::UPDATE) {
            BTH_ASSERT_EQ(done, shadow_map_.exists(key));
        }
        if (expect_success) {
            shadow_map_.put_and_check(key, value, *existing_v, done);
        }
        BTH_YIELD_PERIODIC();
        CO_RETURN;
    }

    BtreeTask< void > do_range_remove(uint64_t start_k, uint64_t end_k, bool all_existing) {
        K start_key = K{start_k};
        K end_key = K{end_k};

        // remove_range returns Expected<uint32_t /*removed_count*/, BtreeStatus>.
        auto result = CO_AWAIT bt_->remove_range(BtreeKeyRange< K >{start_key, true, end_key, true});
        if (all_existing) {
            shadow_map_.range_erase(start_key, end_key);
            BTH_ASSERT_TRUE(result.hasValue());
        } else if (start_k < max_range_input_) {
            K end_range{std::min(end_k, uint64_cast(max_range_input_ - 1))};
            shadow_map_.range_erase(start_key, end_range);
        }
        BTH_YIELD_PERIODIC();
        CO_RETURN;
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
            iomgr().spawn_detached(iomanager::ReactorTarget::reactor(worker_id),
                                   [this, &test_count, op_list, num_ios_this_worker]() -> Async< void > {
                                       // Seed from the process-wide g_re so --seed reproduces this worker's op
                                       // sequence.
                                       std::default_random_engine re{g_re()};
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
                                           CO_AWAIT(this->operations_[op_list[op_idx].first])();
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
