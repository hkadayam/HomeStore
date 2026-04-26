/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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
//
// test_cow_btree_io — mirrors test_mem_btree using the same BtreeTestHelper, but writes onto COWBtree (which exercises
// the BlobDev / VirtualDev / blkalloc IO path). Bootstraps a fresh stack per test
// (DeviceManager → MetaBlk → CPManager → ResourceMgr → BlobDevManager → COWBtreeManager → BlobDev → Btree<K,V>).
// No restart/recovery here — that goes in a separate test once recovery is wired up.
//
#include <filesystem>
#include <fstream>
#include <random>
#include <map>
#include <memory>

#include <gtest/gtest.h>
#include <sisl/options/options.h>
#include <sisl/logging/logging.h>
#include <sisl/fds/enum.h>
#include <boost/algorithm/string.hpp>

#include "iomanager/iomanager.h"
#include "base/test_defs.h"
#include "base/resource_mgr.hpp"

#include "common/defs.h"
#include "device/device_manager.h"
#include "meta/meta_blk_manager.h"
#include "managers.h"

#include <homestore/checkpoint/cp_mgr.h>
#include "blob/blob_dev.h"
#include "blob/blob_dev_mgr.h"

#include "homestore/index/btree/node_variant/simple_node.hpp"
#include "homestore/index/btree/node_variant/varlen_node.hpp"
#include "index/cow_btree/cow_btree_mgr.h"
#include "index/cow_btree/cow_btree.h"
#include "index/cow_btree/cow_btree_mgr.ipp"
#include "homestore/index/btree/tests/btree_test_helper.hpp"

using namespace homestore;
using namespace iomanager;
using sisl::IOBuffer;

SISL_OPTION_GROUP(
    test_cow_btree_io,
    (num_iters, "", "num_iters", "number of iterations for rand ops",
     ::cxxopts::value< uint32_t >()->default_value("100"), "number"),
    (num_entries, "", "num_entries", "number of entries to test with",
     ::cxxopts::value< uint32_t >()->default_value("10000"), "number"),
    (disable_merge, "", "disable_merge", "disable_merge", ::cxxopts::value< bool >()->default_value("0"), ""),
    (num_threads, "", "num_threads", "number of threads", ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
    (num_fibers, "", "num_fibers", "number of fibers", ::cxxopts::value< uint32_t >()->default_value("10"), "number"),
    (operation_list, "", "operation_list", "operation list instead of default created following by percentage",
     ::cxxopts::value< std::vector< std::string > >(), "operations [...]"),
    (preload_size, "", "preload_size", "number of entries to preload tree with",
     ::cxxopts::value< uint32_t >()->default_value("1000"), "number"),
    (max_keys_in_node, "", "max_keys_in_node", "max_keys_in_node", ::cxxopts::value< uint32_t >()->default_value("0"),
     ""),
    (seed, "", "seed", "random engine seed, use random if not defined",
     ::cxxopts::value< uint64_t >()->default_value("0"), "number"),
    (run_time, "", "run_time", "run time for io", ::cxxopts::value< uint32_t >()->default_value("360000"), "seconds"))

// Each test creates one COWBtree which provisions 5 streams (node=128M, overflow=64M, incr_map=1M, 2× full_map=1M
// each = 195 MB minimum). Plus meta_vdev (64 MB), so ~260 MB minimum. We give ourselves comfortable headroom.
static constexpr uint64_t DEV_SIZE = 1024 * 1024 * 1024;     // 1 GB per device
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev
static constexpr uint64_t CHUNK_SIZE = 32 * 1024 * 1024;     // 32 MB initial blob chunk size (streams override)
static constexpr uint32_t BLK_SIZE = 4096;
static constexpr size_t NUM_DEVS = 2;

struct FixedLenBtreeTest {
    using KeyType = TestFixedKey;
    using ValueType = TestFixedValue;
    static constexpr BtreeNodeType leaf_node_type = BtreeNodeType::FIXED;
    static constexpr BtreeNodeType interior_node_type = BtreeNodeType::FIXED;
};

struct VarKeySizeBtreeTest {
    using KeyType = TestVarLenKey;
    using ValueType = TestFixedValue;
    static constexpr BtreeNodeType leaf_node_type = BtreeNodeType::VAR_KEY;
    static constexpr BtreeNodeType interior_node_type = BtreeNodeType::VAR_KEY;
};

struct VarValueSizeBtreeTest {
    using KeyType = TestFixedKey;
    using ValueType = TestVarLenValue;
    static constexpr BtreeNodeType leaf_node_type = BtreeNodeType::VAR_VALUE;
    static constexpr BtreeNodeType interior_node_type = BtreeNodeType::FIXED;
};

struct VarObjSizeBtreeTest {
    using KeyType = TestVarLenKey;
    using ValueType = TestVarLenValue;
    static constexpr BtreeNodeType leaf_node_type = BtreeNodeType::VAR_OBJECT;
    static constexpr BtreeNodeType interior_node_type = BtreeNodeType::VAR_OBJECT;
};

static BtreeTestOptions make_options() {
    return BtreeTestOptions{
        .num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >(),
        .preload_size = SISL_OPTIONS["preload_size"].as< uint32_t >(),
        .num_ios = SISL_OPTIONS["num_iters"].as< uint32_t >(),
        .run_time_secs = SISL_OPTIONS["run_time"].as< uint32_t >(),
        .disable_merge = SISL_OPTIONS["disable_merge"].as< bool >(),
    };
}

// Per-process scratch dev paths. SetUp creates the files, TearDown removes them.
static std::vector< std::string > g_dev_paths;

static std::vector< DevInfo > make_dev_infos() {
    std::vector< DevInfo > infos;
    for (auto& p : g_dev_paths) {
        infos.emplace_back(p, HSDevType::Data, DEV_SIZE);
    }
    return infos;
}

static folly::coro::Task< shared< BlobDev > > bootstrap_stack() {
    auto dm = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    co_await MetaBlkManager::create(META_VDEV_SIZE);

    auto cpmgr = CPManager::create();
    co_await cpmgr->start(true /* first_time_boot */);

    ResourceMgr::start(dm->total_capacity());
    co_await BlobDevManager::create();
    co_await COWBtreeManager::create();

    VDevParameters params;
    params.initial_chunk_size = CHUNK_SIZE;
    params.blk_size = BLK_SIZE;
    params.dev_type = HSDevType::Data;
    params.alloc_type = BlkAllocatorType::SlabCompact;
    params.chunk_sel_type = ChunkSelectorType::RoundRobin;

    using namespace std::string_literals;
    co_return co_await blob_dev_mgr().create_blob_dev("test_cow_btree_io_blob_dev"s, std::move(params));
}

static folly::coro::Task< void > shutdown_stack() {
    co_await cp_mgr().shutdown();
    blob_dev_mgr().shutdown();
    cow_btree_mgr().shutdown();
    co_await device_mgr().close_devices();
    Managers::reset();
    ResourceMgr::stop();
}

template < typename TestType >
struct BtreeTest : public BtreeTestHelper< TestType >, public ::testing::Test {
    using T = TestType;
    using K = typename TestType::KeyType;
    using V = typename TestType::ValueType;

    BtreeTest() : BtreeTestHelper< TestType >(make_options()), ::testing::Test() {}

    void SetUp() override {
        // Create dev files for this test.
        for (size_t i = 0; i < NUM_DEVS; ++i) {
            auto path = fmt::format("/tmp/hs_test_cow_btree_io_{}_{}", ::getpid(), i);
            g_dev_paths.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }

        // Bring up the full COW stack and create one BlobDev for this test.
        blob_dev_ = iomgr().spawn_and_block(iomanager::ReactorTarget::any(), bootstrap_stack());

        // Build the COWBtree behind a Btree<K,V> wrapper.
        this->cfg_.node_size_ = g_node_size;
        this->cfg_.finalize(sizeof(NodeCore::PersistentHeader));
        auto bt = iomgr().spawn_and_block(iomanager::ReactorTarget::any(),
                                          cow_btree_mgr().create_cow_btree< K, V >(this->cfg_, blob_dev_));
        bt->route_tracer().enable_all();
        BtreeTestHelper< TestType >::SetUp(std::move(bt), /*load=*/false, /*is_multi_threaded=*/true);
    }

    void TearDown() override {
        BtreeTestHelper< TestType >::TearDown();
        this->bt_.reset();
        blob_dev_.reset();
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), shutdown_stack());

        for (auto& p : g_dev_paths) {
            std::filesystem::remove(p);
        }
        g_dev_paths.clear();
    }

    shared< BlobDev > blob_dev_;
};

// TODO Enable PrefixIntervalBtreeTest later once the variant-node port lands.
using BtreeTypes =
    testing::Types< FixedLenBtreeTest, VarKeySizeBtreeTest, VarValueSizeBtreeTest, VarObjSizeBtreeTest >;
TYPED_TEST_SUITE(BtreeTest, BtreeTypes);

TYPED_TEST(BtreeTest, SequentialInsert) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();
        const auto entries_iter1 = num_entries / 2;
        LOGINFO("Step 1: forward sequential insert for {} entries", entries_iter1);
        for (uint32_t i = 0; i < entries_iter1; ++i) {
            co_await self->put(i, BtreePutType::INSERT);
        }
        LOGINFO("Step 2: query {} entries with pagination of 75", entries_iter1);
        co_await self->do_query(0, entries_iter1 - 1, 75);

        const auto entries_iter2 = num_entries - entries_iter1;
        LOGINFO("Step 3: reverse sequential insert of remaining {} entries", entries_iter2);
        for (uint32_t i = num_entries - 1; i >= entries_iter1; --i) {
            co_await self->put(i, BtreePutType::INSERT);
        }
        LOGINFO("Step 4: query {} entries with pagination of 90", entries_iter2);
        co_await self->do_query(entries_iter1, num_entries - 1, 90);

        LOGINFO("Step 5: query all entries with no pagination");
        co_await self->query_all();

        LOGINFO("Step 6: query all entries with pagination of 80");
        co_await self->query_all_paginate(80);

        LOGINFO("Step 7: get all entries 1-by-1");
        co_await self->get_all();
        co_await self->get_any(num_entries - 3, num_entries + 1);

        LOGINFO("Step 8: incorrect input — validate errors");
        co_await self->do_query(num_entries + 100, num_entries + 500, 5);
        co_await self->get_any(num_entries + 1, num_entries + 2);
        co_return;
    }());
}

TYPED_TEST(BtreeTest, SequentialRemove) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();
        LOGINFO("Step 1: forward sequential insert for {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->put(i, BtreePutType::INSERT);
        }
        LOGINFO("Step 2: query {} entries with pagination of 75", num_entries);
        co_await self->do_query(0, num_entries - 1, 75);

        const auto entries_iter1 = num_entries / 2;
        LOGINFO("Step 3: forward sequential remove for {} entries", entries_iter1);
        for (uint32_t i = 0; i < entries_iter1; ++i) {
            co_await self->remove_one(i);
        }
        LOGINFO("Step 4: query {} entries with pagination of 75", entries_iter1);
        co_await self->do_query(0, entries_iter1 - 1, 75);

        const auto entries_iter2 = num_entries - entries_iter1;
        LOGINFO("Step 5: reverse sequential remove of remaining {} entries", entries_iter2);
        for (uint32_t i = num_entries - 1; i >= entries_iter1; --i) {
            co_await self->remove_one(i);
        }

        LOGINFO("Step 6: query the empty tree");
        co_await self->do_query(0, num_entries, 75);
        co_await self->get_any(0, 1);
        co_await self->get_specific(0);
        co_return;
    }());
}

TYPED_TEST(BtreeTest, RandomInsert) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();
        std::vector< uint32_t > vec(num_entries);
        std::iota(vec.begin(), vec.end(), 0);
        std::random_shuffle(vec.begin(), vec.end());
        LOGINFO("Step 1: random insert for {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->put(vec[i], BtreePutType::INSERT);
        }
        co_await self->get_all();
        co_return;
    }());
}

TYPED_TEST(BtreeTest, RangeUpdate) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();
        LOGINFO("Step 1: forward sequential insert for {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->put(i, BtreePutType::INSERT);
        }

        LOGINFO("Step 2: range update of random intervals between [1-50] for 100 times");
        for (uint32_t i = 0; i < 100; ++i) {
            co_await self->range_put_random();
        }

        LOGINFO("Step 3: query {} entries with pagination of 75", num_entries);
        co_await self->do_query(0, num_entries - 1, 75);
        co_return;
    }());
}

TYPED_TEST(BtreeTest, SimpleRemoveRange) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = 20u;
        LOGINFO("Step 1: forward sequential insert for {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->put(i, BtreePutType::INSERT);
        }
        LOGINFO("Step 2: range remove sequence");
        co_await self->range_remove_any(5, 10);
        co_await self->range_remove_any(0, 2);
        co_await self->range_remove_any(18, 19);
        co_await self->range_remove_any(17, 17);
        co_await self->range_remove_any(1, 5);
        co_await self->range_remove_any(1, 20);
        co_await self->query_all();
        co_return;
    }());
}

TYPED_TEST(BtreeTest, RandomRemove) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();

        LOGINFO("Step 1: forward sequential insert for {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->put(i, BtreePutType::INSERT);
        }

        std::vector< uint32_t > vec(num_entries);
        std::iota(vec.begin(), vec.end(), 0);
        std::random_shuffle(vec.begin(), vec.end());
        LOGINFO("Step 2: random remove of {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->remove_one(vec[i]);
        }

        co_await self->get_all();
        co_return;
    }());
}

TYPED_TEST(BtreeTest, RandomRemoveRange) {
    auto* self = this;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [self]() -> folly::coro::Task< void > {
        const auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();
        const auto num_iters = SISL_OPTIONS["num_iters"].as< uint32_t >();

        LOGINFO("Step 1: forward sequential insert for {} entries", num_entries);
        for (uint32_t i = 0; i < num_entries; ++i) {
            co_await self->put(i, BtreePutType::INSERT);
        }
        static thread_local std::uniform_int_distribution< uint32_t > s_rand_key_generator{0, num_entries};
        LOGINFO("Step 2: range remove for {} iterations", num_iters);
        for (uint32_t i = 0; (i < num_iters) && self->shadow_map_.size(); ++i) {
            uint32_t key1 = s_rand_key_generator(g_re);
            uint32_t key2 = s_rand_key_generator(g_re);
            co_await self->range_remove_any(std::min(key1, key2), std::max(key1, key2));
        }

        co_await self->query_all();
        co_return;
    }());
}

template < typename TestType >
struct BtreeConcurrentTest : public BtreeTest< TestType > {};

TYPED_TEST_SUITE(BtreeConcurrentTest, BtreeTypes);

TYPED_TEST(BtreeConcurrentTest, ConcurrentAllOps) {
    std::vector< std::string > input_ops = {"put:20", "remove:20", "range_put:20", "range_remove:20", "query:20"};
    if (SISL_OPTIONS.count("operation_list")) {
        input_ops = SISL_OPTIONS["operation_list"].as< std::vector< std::string > >();
    }
    auto ops = this->build_op_list(input_ops);
    this->multi_op_execute(ops);
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_cow_btree_io");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    if (SISL_OPTIONS.count("seed") && SISL_OPTIONS["seed"].as< uint64_t >() != 0) {
        auto seed = SISL_OPTIONS["seed"].as< uint64_t >();
        LOGINFO("Using seed {} to sow the random generation", seed);
        g_re.seed(seed);
    } else {
        auto seed = std::chrono::system_clock::now().time_since_epoch().count();
        LOGINFO("No seed provided. Using randomly generated seed: {}", seed);
        g_re.seed(seed);
    }

    iomanager::init_iomgr(SISL_OPTIONS["num_threads"].as< uint32_t >());
    auto ret = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return ret;
}
