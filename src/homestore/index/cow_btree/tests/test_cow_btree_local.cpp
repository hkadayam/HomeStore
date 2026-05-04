/*********************************************************************************
 * Copyright 2024-2026 Harihara Kadayam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *********************************************************************************/
//
// test_cow_btree_local — exercises COWBtree directly (not via Btree<K,V> mutate path).  Builds a flat collection
// of leaf SimpleNode<TestFixedKey, TestFixedValue> instances via cow_btree's UnderlyingBtree API, drives CP
// flushes (incremental + full, the latter forced via the `force_full_map_flush` flip), and validates persistence
// across stack restarts.
//
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <sisl/options/options.h>
#include <sisl/logging/logging.h>
#include <sisl/flip/flip.h>
#include <sisl/flip/flip_client.h>

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

#include "homestore/index/btree/btree.ipp"
#include "homestore/index/btree/node_variant/simple_node.hpp"
#include "index/cow_btree/cow_btree_mgr.h"
#include "index/cow_btree/cow_btree.h"
#include "index/cow_btree/cow_btree_mgr.ipp"
#include "homestore/index/btree/tests/btree_test_kvs.hpp"

using namespace homestore;
using namespace iomanager;

SISL_OPTION_GROUP(test_cow_btree_local,
                  (num_nodes, "", "num_nodes", "default node count for stress tests",
                   ::cxxopts::value< uint32_t >()->default_value("200"), "number"),
                  (kvs_per_node, "", "kvs_per_node", "K/V entries to pack per leaf",
                   ::cxxopts::value< uint32_t >()->default_value("50"), "number"),
                  (num_threads, "", "num_threads", "iomgr reactor count",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
                  (seed, "", "seed", "rng seed (0 = random)", ::cxxopts::value< uint64_t >()->default_value("0"),
                   "number"))

// ──────────────────────────────────────────── Stack constants ────────────────────────────────────────────────────────
static constexpr uint64_t DEV_SIZE = 1024ull * 1024 * 1024;     // 1 GB per device
static constexpr uint64_t META_VDEV_SIZE = 64ull * 1024 * 1024; // 64 MB
static constexpr uint64_t CHUNK_SIZE = 32ull * 1024 * 1024;     // 32 MB blob chunk
static constexpr uint32_t BLK_SIZE = 4096;
static constexpr size_t NUM_DEVS = 2;
static constexpr uint32_t NODE_SIZE = 4096;
static constexpr char BLOB_DEV_NAME[] = "test_cow_btree_local_blob_dev";
static constexpr char BTREE_NAME[] = "local_btree";

using K = TestFixedKey;
using V = TestFixedValue;
using LeafNode = SimpleNode< K, V >;
using TestBtree = Btree< K, V >;

// Per-process scratch dev paths.
static std::vector< std::string > g_dev_paths;

static std::vector< DevInfo > make_dev_infos() {
    std::vector< DevInfo > infos;
    infos.reserve(g_dev_paths.size());
    for (auto& p : g_dev_paths) {
        infos.emplace_back(p, HSDevType::Data, DEV_SIZE);
    }
    return infos;
}

// ──────────────────────────────────────────── Stack bootstrap / teardown ─────────────────────────────────────────────
// first_time_boot=true: format devices, create fresh managers.  false: open existing devices, recover all managers.
static folly::coro::Task< shared< BlobDev > > bootstrap_stack(bool first_time_boot) {
    shared< DeviceManager > dm;
    if (first_time_boot) {
        dm = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);
    } else {
        dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();
        co_await MetaBlkManager::load();
    }

    auto cpmgr = CPManager::create();
    co_await cpmgr->start(first_time_boot);

    ResourceMgr::start(dm->total_capacity());

    if (first_time_boot) {
        co_await BlobDevManager::create();
        co_await COWBtreeManager::create();
    } else {
        co_await BlobDevManager::load();
        co_await COWBtreeManager::load();
    }

    if (first_time_boot) {
        VDevParameters params;
        params.initial_chunk_size = CHUNK_SIZE;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;
        auto bd = co_await blob_dev_mgr().create_blob_dev(std::string{BLOB_DEV_NAME}, std::move(params));

        // commit_formatting flips the on-pdev "first-boot complete" marker.  Without it, a subsequent
        // load_devices() sees the devices as still first-boot and refuses to recover the metablks (so the
        // BlobDev / COWBtree disappear on restart).  test_cow_btree_io never restarts, so it doesn't need this.
        co_await dm->commit_formatting();
        co_return bd;
    } else {
        co_return blob_dev_mgr().get_blob_dev(BLOB_DEV_NAME);
    }
}

static folly::coro::Task< void > shutdown_stack() {
    co_await cp_mgr().shutdown();
    blob_dev_mgr().shutdown();
    cow_btree_mgr().shutdown();
    co_await device_mgr().close_devices();
    Managers::reset();
    ResourceMgr::stop();
}

// ──────────────────────────────────────────── Test fixture ───────────────────────────────────────────────────────────
//
// Shadow model: shadow_ pairs node id with the (key,value) list expected on read-back.  fill_node / rmw_node /
// remove_node update the shadow alongside the cow_btree mutation.  verify_all() rereads every tracked node and
// diffs its leaf contents (via SimpleNode<K,V>::get_nth_key/get_nth_value) against the shadow.  Note: gtest's
// ASSERT_* macros expand to `return;` and so cannot be used inside a coroutine that returns Task<void> — the
// helpers below therefore use raw EXPECT/ADD_FAILURE outside the coroutine, or set a status flag that the test
// inspects after the coroutine completes.
//
class CowBtreeLocalTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < NUM_DEVS; ++i) {
            auto path = fmt::format("/tmp/hs_test_cow_btree_local_{}_{}", ::getpid(), i);
            g_dev_paths.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }

        bringup(/*first_time_boot=*/true);
    }

    void TearDown() override {
        teardown();
        for (auto& p : g_dev_paths) {
            std::filesystem::remove(p);
        }
        g_dev_paths.clear();
    }

    /// Restart the stack: shutdown, then bootstrap with first_time_boot=false and reload our btree from the
    /// recovered super_blk list.  The cache is empty after this — every subsequent read_node hits disk via the
    /// recovered bnode_map (and thus exercises full-map / incr-map replay paths).
    void restart() {
        teardown();
        bringup(/*first_time_boot=*/false);
    }

    // ── Node-level operations ─────────────────────────────────────────────────────────────────────────────────────
    /// Allocate a fresh leaf, fill it with `kvs_per_node` random K/V pairs, mark it dirty for the current CP, and
    /// record the contents in the shadow.  Returns the node id.
    bnodeid_t create_leaf(uint32_t kvs_per_node) {
        bnodeid_t id{empty_bnodeid};
        std::vector< std::pair< uint64_t, uint32_t > > pairs;
        auto* self = this;
        iomgr().spawn_and_block(ReactorTarget::any(), [self, kvs_per_node, &id, &pairs]() -> folly::coro::Task< void > {
            auto* cow = COWBtree::cast_to(self->bt_.get());
            auto node = cow->create_node(/*is_leaf=*/true);
            self->fill_node(node, kvs_per_node, pairs);
            auto st = cow->prepare_for_write(node);
            HS_REL_ASSERT_EQ(st, BtreeStatus::success, "prepare_for_write on fresh node");
            cow->write_node(node);
            id = node->node_id();
            co_return;
        }());
        shadow_[id] = std::move(pairs);
        return id;
    }

    /// Read-modify-write: replace the value for the kth (key, value) slot with a new randomly-generated value.
    void rmw(bnodeid_t id, uint32_t slot_idx) {
        ASSERT_TRUE(shadow_.count(id)) << "rmw on unknown node " << id;
        ASSERT_LT(slot_idx, shadow_[id].size());
        auto const new_val_raw = next_value();
        auto* self = this;
        iomgr().spawn_and_block(ReactorTarget::any(), [self, id, slot_idx, new_val_raw]() -> folly::coro::Task< void > {
            auto* cow = COWBtree::cast_to(self->bt_.get());
            auto result = co_await cow->read_node(id, LockType::Write);
            HS_REL_ASSERT(result.hasValue(), "read_node failed for id={}", id);
            auto node = std::move(result.value());
            auto st = cow->prepare_for_write(node);
            HS_REL_ASSERT_EQ(st, BtreeStatus::success, "prepare_for_write rmw");
            auto* leaf = static_cast< LeafNode* >(node.operator->());
            V val{new_val_raw};
            leaf->update(slot_idx, val);
            cow->write_node(node);
            co_return;
        }());
        shadow_[id][slot_idx].second = new_val_raw;
    }

    /// Remove the node from cow_btree and drop it from the shadow.
    void remove(bnodeid_t id) {
        ASSERT_TRUE(shadow_.count(id)) << "remove on unknown node " << id;
        auto* self = this;
        iomgr().spawn_and_block(ReactorTarget::any(), [self, id]() -> folly::coro::Task< void > {
            auto* cow = COWBtree::cast_to(self->bt_.get());
            auto result = co_await cow->read_node(id, LockType::Write);
            HS_REL_ASSERT(result.hasValue(), "read_node for remove id={}", id);
            auto node = std::move(result.value());
            cow->remove_node(node);
            co_return;
        }());
        shadow_.erase(id);
    }

    /// Re-read every tracked node and verify its leaf contents match the shadow.  Returns the count of mismatches
    /// (0 == clean).  Logs each mismatch via gtest's ADD_FAILURE so you see all of them.
    void verify_all() {
        std::vector< bnodeid_t > ids;
        ids.reserve(shadow_.size());
        for (auto const& [id, _] : shadow_) {
            ids.push_back(id);
        }

        // Snapshot the shadow per-node for the coroutine to compare against (avoids holding shadow_ across awaits).
        auto* self = this;
        struct Mismatch {
            bnodeid_t id;
            std::string detail;
        };
        std::vector< Mismatch > mismatches;
        iomgr().spawn_and_block(
            ReactorTarget::any(), [self, ids = std::move(ids), &mismatches]() -> folly::coro::Task< void > {
                auto* cow = COWBtree::cast_to(self->bt_.get());
                for (auto id : ids) {
                    auto result = co_await cow->read_node(id, LockType::Read);
                    if (!result.hasValue()) {
                        mismatches.push_back({id, "read_node failed"});
                        continue;
                    }
                    auto& node = result.value();
                    auto* leaf = static_cast< LeafNode const* >(node.operator->());
                    auto const& expected = self->shadow_.at(id);
                    if (leaf->total_entries() != expected.size()) {
                        mismatches.push_back(
                            {id, fmt::format("entry count: got {} want {}", leaf->total_entries(), expected.size())});
                        continue;
                    }
                    for (uint32_t i = 0; i < expected.size(); ++i) {
                        K key = leaf->template get_nth_key< K >(i, /*copy=*/false);
                        V val;
                        leaf->get_nth_value(i, &val, /*copy=*/false);
                        if (key.key() != expected[i].first || val.value() != expected[i].second) {
                            mismatches.push_back({id,
                                                  fmt::format("slot {}: got ({},{}) want ({},{})", i, key.key(),
                                                              val.value(), expected[i].first, expected[i].second)});
                        }
                    }
                }
                co_return;
            }());

        for (auto const& m : mismatches) {
            ADD_FAILURE() << "node " << m.id << ": " << m.detail;
        }
    }

    // ── CP flush controls ─────────────────────────────────────────────────────────────────────────────────────────
    void flush_incremental() { trigger_cp(/*full_map=*/false); }
    void flush_full() { trigger_cp(/*full_map=*/true); }

protected:
    shared< TestBtree > bt_;
    shared< BlobDev > blob_dev_;
    BtreeConfig cfg_;
    // shadow_ holds the K/V vector we expect to read back per node id.  TestFixedValue stores a uint32_t — its
    // (uint64_t/bnodeid_t) ctor explicitly assert(0)s, so values are tracked as uint32_t end-to-end.
    std::unordered_map< bnodeid_t, std::vector< std::pair< uint64_t, uint32_t > > > shadow_;
    uint32_t value_counter_{1};
    uint64_t key_counter_{1};

private:
    void bringup(bool first_time_boot) {
        blob_dev_ = iomgr().spawn_and_block(ReactorTarget::any(), bootstrap_stack(first_time_boot));
        HS_REL_ASSERT(blob_dev_, "blob_dev_ null after bootstrap (first_time_boot={})", first_time_boot);

        cfg_ = BtreeConfig{};
        cfg_.btree_name_ = BTREE_NAME;
        cfg_.node_size_ = NODE_SIZE;
        cfg_.leaf_node_type_ = BtreeNodeType::FIXED;
        cfg_.int_node_type_ = BtreeNodeType::FIXED;
        cfg_.finalize(sizeof(NodeCore::PersistentHeader));

        if (first_time_boot) {
            bt_ = iomgr().spawn_and_block(ReactorTarget::any(),
                                          cow_btree_mgr().create_cow_btree< K, V >(cfg_, blob_dev_));
        } else {
            auto sbs = cow_btree_mgr().list_persisted_btrees();
            HS_REL_ASSERT_EQ(sbs.size(), 1u, "Expected exactly one persisted btree on recovery");
            bt_ = iomgr().spawn_and_block(ReactorTarget::any(),
                                          cow_btree_mgr().load_cow_btree< K, V >(cfg_, blob_dev_, *sbs[0]));
        }
    }

    void teardown() {
        bt_.reset();
        blob_dev_.reset();
        iomgr().spawn_and_block(ReactorTarget::any(), shutdown_stack());
    }

    void trigger_cp(bool full_map) {
        if (full_map) {
            flip::FlipFrequencyT freq;
            freq.count = 1;
            flip::PercentFrequencyT pf;
            pf.v = 100;
            freq.kind.Set(pf);
            flip::FlipClient::instance().inject_noreturn_flip("force_full_map_flush", {}, freq);
        }
        iomgr().spawn_and_block(ReactorTarget::any(), []() -> folly::coro::Task< void > {
            auto fut = cp_mgr().trigger_cp_flush(/*force=*/true, CPTriggerReason::UserDriven);
            co_await std::move(fut).via(co_await folly::coro::co_current_executor);
            co_return;
        }());
        if (full_map) {
            flip::Flip::instance().remove("force_full_map_flush");
        }
    }

    /// Insert random monotonically-increasing K/V pairs into the SimpleNode and capture them in `pairs`.  Keys
    /// must be inserted in sorted order (SimpleNode's in-place insert at index `i` requires it).
    void fill_node(Node& node, uint32_t kvs_per_node, std::vector< std::pair< uint64_t, uint32_t > >& pairs) {
        pairs.reserve(kvs_per_node);
        auto* leaf = static_cast< LeafNode* >(node.operator->());
        for (uint32_t i = 0; i < kvs_per_node; ++i) {
            uint64_t k = key_counter_++;
            uint32_t v = next_value();
            K key{k};
            V val{v};
            auto st = leaf->insert(i, key, val);
            HS_REL_ASSERT_EQ(st, BtreeStatus::success, "fill_node insert");
            pairs.emplace_back(k, v);
        }
    }

    uint32_t next_value() { return value_counter_++; }
};

// ──────────────────────────────────────────── Tests ──────────────────────────────────────────────────────────────────

TEST_F(CowBtreeLocalTest, CreateRead) {
    constexpr uint32_t N = 20;
    constexpr uint32_t KVS = 30;
    for (uint32_t i = 0; i < N; ++i) {
        create_leaf(KVS);
    }
    flush_incremental();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ReadModifyWriteOneCp) {
    constexpr uint32_t N = 20;
    constexpr uint32_t KVS = 30;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(KVS));
    }
    flush_incremental();

    for (size_t i = 0; i < ids.size(); i += 2) {
        rmw(ids[i], /*slot=*/0);
    }
    flush_incremental();
    verify_all();
}

TEST_F(CowBtreeLocalTest, RemoveSubset) {
    constexpr uint32_t N = 20;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(20));
    }
    flush_incremental();

    for (size_t i = 0; i < ids.size(); i += 3) {
        remove(ids[i]);
    }
    flush_incremental();
    verify_all();
}

TEST_F(CowBtreeLocalTest, RmwAcrossManyIncrementalCps) {
    constexpr uint32_t N = 30;
    constexpr uint32_t KVS = 40;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(KVS));
    }
    flush_incremental();

    constexpr uint32_t ROUNDS = 8;
    for (uint32_t r = 0; r < ROUNDS; ++r) {
        for (auto id : ids) {
            rmw(id, /*slot=*/r % KVS);
        }
        flush_incremental();
    }
    verify_all();
}

TEST_F(CowBtreeLocalTest, IncrementalThenFullThenIncremental) {
    constexpr uint32_t N = 20;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(25));
    }
    flush_incremental();
    verify_all();

    for (auto id : ids) {
        rmw(id, 0);
    }
    flush_incremental();
    verify_all();

    flush_full();
    verify_all();

    for (auto id : ids) {
        rmw(id, 1);
    }
    flush_incremental();
    verify_all();
}

TEST_F(CowBtreeLocalTest, RestartAfterIncremental) {
    constexpr uint32_t N = 30;
    for (uint32_t i = 0; i < N; ++i) {
        create_leaf(25);
    }
    flush_incremental();

    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, RestartAfterFull) {
    constexpr uint32_t N = 30;
    for (uint32_t i = 0; i < N; ++i) {
        create_leaf(25);
    }
    flush_full();

    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, RestartMixedFlushes) {
    constexpr uint32_t N = 30;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(25));
    }
    flush_incremental();
    for (auto id : ids) {
        rmw(id, 0);
    }
    flush_incremental();
    flush_full();
    for (auto id : ids) {
        rmw(id, 1);
    }
    flush_incremental();

    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, WriteAfterRestart) {
    constexpr uint32_t N = 20;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(25));
    }
    flush_incremental();
    restart();
    verify_all();

    for (uint32_t i = 0; i < N; ++i) {
        create_leaf(25);
    }
    for (auto id : ids) {
        rmw(id, 0);
    }
    flush_incremental();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, RestartBetweenEveryCp) {
    constexpr uint32_t ROUNDS = 4;
    for (uint32_t r = 0; r < ROUNDS; ++r) {
        for (uint32_t i = 0; i < 10; ++i) {
            create_leaf(20);
        }
        if ((r % 2) == 0) {
            flush_incremental();
        } else {
            flush_full();
        }
        restart();
        verify_all();
    }
}

TEST_F(CowBtreeLocalTest, RemoveAfterRestart) {
    constexpr uint32_t N = 30;
    std::vector< bnodeid_t > ids;
    for (uint32_t i = 0; i < N; ++i) {
        ids.push_back(create_leaf(25));
    }
    flush_incremental();
    restart();

    for (size_t i = 0; i < ids.size(); i += 2) {
        remove(ids[i]);
    }
    flush_incremental();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ManyNodesIncrementalRestart) {
    auto const N = SISL_OPTIONS["num_nodes"].as< uint32_t >();
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();

    for (uint32_t i = 0; i < N; ++i) {
        create_leaf(KVS);
        if ((i + 1) % 50 == 0) {
            flush_incremental();
        }
    }
    flush_incremental();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ManyNodesFullRestart) {
    auto const N = SISL_OPTIONS["num_nodes"].as< uint32_t >();
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();

    for (uint32_t i = 0; i < N; ++i) {
        create_leaf(KVS);
    }
    flush_incremental();
    flush_full();
    restart();
    verify_all();
}

// ──────────────────────────────────────────── main ───────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_cow_btree_local");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    auto const seed = SISL_OPTIONS["seed"].as< uint64_t >();
    g_re.seed(seed ? seed : std::chrono::system_clock::now().time_since_epoch().count());

    iomanager::init_iomgr(SISL_OPTIONS["num_threads"].as< uint32_t >());
    auto ret = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return ret;
}