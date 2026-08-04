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
#include "common/async.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include "sisl/options/options.h"
#include "sisl/logging/logging.h"
#include "sisl/flip/flip.h"
#include "sisl/flip/flip_client.h"

#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "common/defs.h"
#include "homestore/device/device_manager.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/managers.h"

#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"

#include "homestore/index/btree/btree.ipp"
#include "homestore/index/btree/node_variant/simple_node.h"
#include "homestore/index/cow_btree/cow_btree_mgr.h"
#include "homestore/index/cow_btree/cow_btree.h"
#include "homestore/index/cow_btree/cow_btree_mgr.ipp"
#include "homestore/index/btree/tests/btree_test_kvs.h"

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
static constexpr uint64_t CACHE_SIZE = 256ull * 1024 * 1024; // 256 MB evictor budget (prod: ResourceMgr-provided)
// Short on purpose: the device name is the prefix of every per-chunk MetaBlk name ("<dev>_<type>_<sid>_<cid>_<bsz>"),
// which must fit MetaBlkHeader's 31-char name field even as stream ids grow across btree create/destroy churn.
static constexpr char BLOB_DEV_NAME[] = "cbl";
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
static Async< shared< BlobDev > > bootstrap_stack(bool first_time_boot) {
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

    if (first_time_boot) {
        co_await BlobDevManager::create();
        co_await COWBtreeManager::create(CACHE_SIZE);
    } else {
        co_await BlobDevManager::load();
        co_await COWBtreeManager::load(CACHE_SIZE);
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

static Async< void > shutdown_stack() {
    co_await cp_mgr().shutdown();
    blob_dev_mgr().shutdown();
    cow_btree_mgr().shutdown();
    co_await device_mgr().close_devices();
    Managers::reset();
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

        // Per-test fresh reactors so CPManager's t_cp_info_ thread_local cache doesn't dangle into the freed
        // CPManager from the previous test.
        iomanager::init_iomgr(std::max< uint32_t >(
            2, SISL_OPTIONS["num_threads"].as< uint32_t >())); // tests target reactors 0 and 1 explicitly
        bringup(/*first_time_boot=*/true);
    }

    void TearDown() override {
        teardown();
        iomanager::stop_iomgr();
        for (auto& p : g_dev_paths) {
            std::filesystem::remove(p);
        }
        g_dev_paths.clear();
    }

    /// Restart the stack: shutdown, recycle iomgr, then bootstrap with first_time_boot=false and reload our btree
    /// from the recovered super_blk list.  Bouncing iomgr is what makes this a true restart: reactor threads die
    /// and their thread-local state (e.g. CPGuard's cached per-thread cp stack pointer into the old CPManager) is
    /// wiped — without this, a stale thread_local would dereference freed manager-owned memory on the next access.
    /// The cache is empty after this; every subsequent read_node hits disk via the recovered bnode_map (and thus
    /// exercises full-map / incr-map replay paths).
    void restart() {
        teardown();
        iomanager::stop_iomgr();
        iomanager::init_iomgr(std::max< uint32_t >(
            2, SISL_OPTIONS["num_threads"].as< uint32_t >())); // tests target reactors 0 and 1 explicitly
        bringup(/*first_time_boot=*/false);
    }

    // ── Node-level operations ─────────────────────────────────────────────────────────────────────────────────────
    /// Allocate a fresh leaf, fill it with `kvs_per_node` random K/V pairs, mark it dirty for the current CP, and
    /// record the contents in the shadow.  Returns the node id.
    bnodeid_t create_leaf(uint32_t kvs_per_node) {
        bnodeid_t id{empty_bnodeid};
        std::vector< std::pair< uint64_t, uint32_t > > pairs;
        auto* self = this;
        iomgr().spawn_and_block(ReactorTarget::any(), [self, kvs_per_node, &id, &pairs]() -> Async< void > {
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
        iomgr().spawn_and_block(ReactorTarget::any(), [self, id, slot_idx, new_val_raw]() -> Async< void > {
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
        iomgr().spawn_and_block(ReactorTarget::any(), [self, id]() -> Async< void > {
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
        iomgr().spawn_and_block(ReactorTarget::any(), [self, ids = std::move(ids), &mismatches]() -> Async< void > {
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

    // ── Concurrency helpers ───────────────────────────────────────────────────────────────────────────────────────
    using LeafContent = std::vector< std::pair< uint64_t, uint32_t > >;
    using LeafSet = std::vector< std::pair< bnodeid_t, LeafContent > >;

    /// Worker body for the concurrency tests: insert `n_leaves` fresh leaves into `cow`, each holding `kvs` pairs
    /// drawn from the caller-reserved disjoint key/value ranges.  Each leaf goes through its own spawn_and_block, so
    /// calls from several OS threads interleave on the reactors.  Touches no fixture state — the caller merges the
    /// returned set into shadow_ (or verifies it directly via verify_leaves) after all workers have joined.
    LeafSet insert_leaves_worker(COWBtree* cow, uint32_t n_leaves, uint32_t kvs, uint64_t key_base, uint32_t val_base) {
        LeafSet out;
        out.reserve(n_leaves);
        uint64_t key = key_base;
        uint32_t val = val_base;
        for (uint32_t l = 0; l < n_leaves; ++l) {
            bnodeid_t id{empty_bnodeid};
            LeafContent pairs;
            pairs.reserve(kvs);
            iomgr().spawn_and_block(ReactorTarget::any(), [cow, kvs, &id, &pairs, &key, &val]() -> Async< void > {
                auto node = cow->create_node(/*is_leaf=*/true);
                auto* leaf = static_cast< LeafNode* >(node.operator->());
                for (uint32_t i = 0; i < kvs; ++i) {
                    K k{key};
                    V v{val};
                    auto st = leaf->insert(i, k, v);
                    HS_REL_ASSERT_EQ(st, BtreeStatus::success, "insert_leaves_worker: leaf insert");
                    pairs.emplace_back(key, val);
                    ++key;
                    ++val;
                }
                // A CP switchover may land between create_node's dirty registration and this call — retry is the
                // legal outcome and re-registers against the new CP.
                while (true) {
                    auto st = cow->prepare_for_write(node);
                    if (st == BtreeStatus::success) {
                        break;
                    }
                    HS_REL_ASSERT_EQ(st, BtreeStatus::retry, "insert_leaves_worker: prepare_for_write");
                }
                cow->write_node(node);
                id = node->node_id();
                co_return;
            }());
            out.emplace_back(id, std::move(pairs));
        }
        return out;
    }

    /// Verify `expected` leaf contents read back from `cow`.  Returns the mismatch count (0 == clean).  Usable for
    /// btrees other than bt_ (which verify_all covers via shadow_).
    uint32_t verify_leaves(COWBtree* cow, LeafSet const& expected) {
        uint32_t mismatches = 0;
        iomgr().spawn_and_block(ReactorTarget::any(), [cow, &expected, &mismatches]() -> Async< void > {
            for (auto const& [id, pairs] : expected) {
                auto result = co_await cow->read_node(id, LockType::Read);
                if (!result.hasValue()) {
                    ++mismatches;
                    continue;
                }
                auto& node = result.value();
                auto* leaf = static_cast< LeafNode const* >(node.operator->());
                if (leaf->total_entries() != pairs.size()) {
                    ++mismatches;
                    continue;
                }
                for (uint32_t i = 0; i < pairs.size(); ++i) {
                    K key = leaf->template get_nth_key< K >(i, /*copy=*/false);
                    V val;
                    leaf->get_nth_value(i, &val, /*copy=*/false);
                    if (key.key() != pairs[i].first || val.value() != pairs[i].second) {
                        ++mismatches;
                    }
                }
            }
            co_return;
        }());
        return mismatches;
    }

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
        iomgr().spawn_and_block(ReactorTarget::any(), []() -> Async< void > {
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

// A blob_dev may host more than one btree over its lifetime (user-controlled, not enforced): destroy a btree, then
// create a fresh one in the SAME blob_dev, persist it, and restart.  The recovered store must contain exactly the
// second btree — the destroyed one must leave no dangling SB, and the second btree's streams must persist.
TEST_F(CowBtreeLocalTest, DestroyThenRecreateSameBlobDevThenRestart) {
    for (uint32_t i = 0; i < 10; ++i) {
        create_leaf(25);
    }
    flush_incremental();

    // Destroy btree A.
    {
        cshared< BtreeBase > base = bt_;
        iomgr().spawn_and_block(ReactorTarget::any(), cow_btree_mgr().destroy_cow_btree(base));
    }
    bt_.reset();
    shadow_.clear();

    // Create btree B in the same blob_dev and persist it.
    bt_ = iomgr().spawn_and_block(ReactorTarget::any(), cow_btree_mgr().create_cow_btree< K, V >(cfg_, blob_dev_));
    for (uint32_t i = 0; i < 10; ++i) {
        create_leaf(25);
    }
    flush_incremental();

    // B must recover cleanly (bringup asserts exactly one persisted btree, then verify_all rereads it).
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

// ─────────────────────────────────────── Concurrency tests ───────────────────────────────────────────────────────────
//
// These run REAL concurrency: several OS threads driving btree mutations (each op through its own spawn_and_block,
// interleaving on the iomgr reactors) while — where the test calls for it — another thread triggers CP flushes.  The
// fixture's key/value counters and shadow_ are single-threaded, so every worker generates keys from its own disjoint
// range and collects results locally; shadow_ is merged only after all workers have joined.

TEST_F(CowBtreeLocalTest, MultiThreadedInsert) {
    static constexpr uint32_t n_threads = 4;
    static constexpr uint32_t leaves_per_thread = 25;
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();

    auto* cow = COWBtree::cast_to(bt_.get());
    std::vector< LeafSet > results{n_threads};
    std::vector< std::thread > workers;
    for (uint32_t t = 0; t < n_threads; ++t) {
        workers.emplace_back([this, cow, t, KVS, &results]() {
            // Disjoint key/value space per thread — no shared mutable state while running.
            results[t] = insert_leaves_worker(cow, leaves_per_thread, KVS, 1'000'000ull * (t + 1), 10'000u * (t + 1));
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    for (auto& per_thread : results) {
        for (auto& [id, pairs] : per_thread) {
            shadow_[id] = std::move(pairs);
        }
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ConcurrentCpAndInsert) {
    static constexpr uint32_t n_threads = 3;
    static constexpr uint32_t leaves_per_thread = 20;
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();

    // CP thread: keep forcing incremental CP flushes the whole time the writers are inserting, so node registration
    // (prepare_for_write) races CP switchover and flush pinning — the overlap a sequential suite never exercises.
    std::atomic< bool > writers_done{false};
    std::thread cp_thread{[this, &writers_done]() {
        while (!writers_done.load(std::memory_order_acquire)) {
            flush_incremental();
            std::this_thread::sleep_for(std::chrono::milliseconds{3});
        }
    }};

    auto* cow = COWBtree::cast_to(bt_.get());
    std::vector< LeafSet > results{n_threads};
    std::vector< std::thread > workers;
    for (uint32_t t = 0; t < n_threads; ++t) {
        workers.emplace_back([this, cow, t, KVS, &results]() {
            results[t] = insert_leaves_worker(cow, leaves_per_thread, KVS, 1'000'000ull * (t + 1), 10'000u * (t + 1));
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    writers_done.store(true, std::memory_order_release);
    cp_thread.join();

    for (auto& per_thread : results) {
        for (auto& [id, pairs] : per_thread) {
            shadow_[id] = std::move(pairs);
        }
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ConcurrentRmwWithCp) {
    static constexpr uint32_t n_nodes = 8;
    static constexpr uint32_t n_threads = 4;
    static constexpr uint32_t rounds = 5;
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();
    ASSERT_GE(KVS, n_threads) << "need at least one slot per thread";

    // Baseline: nodes built single-threaded, then CP-flushed so the rmw's below hit the copy-on-flush path (their
    // phys bufs are pinned by flush entries) as well as plain in-place updates.
    std::vector< bnodeid_t > ids;
    ids.reserve(n_nodes);
    for (uint32_t i = 0; i < n_nodes; ++i) {
        ids.push_back(create_leaf(KVS));
    }
    flush_incremental();

    std::atomic< bool > writers_done{false};
    std::thread cp_thread{[this, &writers_done]() {
        while (!writers_done.load(std::memory_order_acquire)) {
            flush_incremental();
            std::this_thread::sleep_for(std::chrono::milliseconds{3});
        }
    }};

    // Every thread rmw's the SAME nodes (real write-lock contention on shared nodes) but its own disjoint slot range
    // within each node, so the final expected value per slot stays deterministic.
    uint32_t const slots_per_thread = KVS / n_threads;
    std::vector< std::thread > workers;
    for (uint32_t t = 0; t < n_threads; ++t) {
        workers.emplace_back([this, t, slots_per_thread, &ids]() {
            uint32_t const slot_lo = t * slots_per_thread;
            for (uint32_t round = 0; round < rounds; ++round) {
                for (auto const id : ids) {
                    for (uint32_t s = slot_lo; s < slot_lo + slots_per_thread; ++s) {
                        uint32_t const new_val = 100'000u * (t + 1) + 1'000u * round + s;
                        iomgr().spawn_and_block(ReactorTarget::any(), [this, id, s, new_val]() -> Async< void > {
                            auto* cow = COWBtree::cast_to(bt_.get());
                            while (true) {
                                auto result = co_await cow->read_node(id, LockType::Write);
                                if (!result.hasValue()) {
                                    // read_node's internal prepare_for_write lost a CP-switchover race — retry.
                                    HS_REL_ASSERT_EQ(result.error(), BtreeStatus::retry,
                                                     "ConcurrentRmwWithCp: read_node");
                                    continue;
                                }
                                auto node = std::move(result.value());
                                auto st = cow->prepare_for_write(node);
                                if (st == BtreeStatus::retry) {
                                    continue; // node unlocks via RAII; re-enter under the new CP
                                }
                                HS_REL_ASSERT_EQ(st, BtreeStatus::success, "ConcurrentRmwWithCp: prepare_for_write");
                                auto* leaf = static_cast< LeafNode* >(node.operator->());
                                V val{new_val};
                                leaf->update(s, val);
                                cow->write_node(node);
                                break;
                            }
                            co_return;
                        }());
                    }
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    writers_done.store(true, std::memory_order_release);
    cp_thread.join();

    // Fold the deterministic final values (last round wins per slot) into the shadow, then verify live and across
    // a restart.
    for (auto const id : ids) {
        for (uint32_t t = 0; t < n_threads; ++t) {
            for (uint32_t s = t * slots_per_thread; s < (t + 1) * slots_per_thread; ++s) {
                shadow_[id][s].second = 100'000u * (t + 1) + 1'000u * (rounds - 1) + s;
            }
        }
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ConcurrentCpAndDestroy) {
    static constexpr uint32_t churn_iters = 6;
    static constexpr uint32_t churn_leaves = 10;
    static constexpr uint32_t traffic_leaves = 40;
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();

    // CP thread flushes continuously — every btree create/fill/destroy below races a flush.
    std::atomic< bool > done{false};
    std::thread cp_thread{[this, &done]() {
        while (!done.load(std::memory_order_acquire)) {
            flush_incremental();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }};

    // Steady insert traffic on the primary btree for the whole run, so destroys never happen on a quiet system.
    LeafSet traffic;
    std::thread traffic_thread{[this, KVS, &traffic]() {
        traffic = insert_leaves_worker(COWBtree::cast_to(bt_.get()), traffic_leaves, KVS, 1'000'000ull, 10'000u);
    }};

    // Churn thread: repeatedly create a second btree on the SAME blob_dev + shared node cache, fill it, verify it,
    // destroy it.  Each destroy races the CP thread mid-flush, and each recreate reuses the freed ordinal while the
    // primary's nodes are hot in the shared cache — the exact lifecycle that previously leaked stale cache entries.
    std::thread churn_thread{[this, KVS]() {
        for (uint32_t it = 0; it < churn_iters; ++it) {
            auto cfg2 = cfg_;
            cfg2.btree_name_ = fmt::format("{}_churn", BTREE_NAME);
            shared< TestBtree > extra = iomgr().spawn_and_block(
                ReactorTarget::any(), cow_btree_mgr().create_cow_btree< K, V >(cfg2, blob_dev_));
            auto contents = insert_leaves_worker(COWBtree::cast_to(extra.get()), churn_leaves, KVS,
                                                 100'000'000ull * (it + 1), 1'000'000u * (it + 1));
            EXPECT_EQ(verify_leaves(COWBtree::cast_to(extra.get()), contents), 0u) << "churn btree iter " << it;
            cshared< BtreeBase > base = extra;
            iomgr().spawn_and_block(ReactorTarget::any(), cow_btree_mgr().destroy_cow_btree(base));
        }
    }};

    churn_thread.join();
    traffic_thread.join();
    done.store(true, std::memory_order_release);
    cp_thread.join();

    for (auto& [id, pairs] : traffic) {
        shadow_[id] = std::move(pairs);
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, MultipleBtreesConcurrentOps) {
    static constexpr uint32_t n_extra = 3;
    static constexpr uint32_t leaves_per_btree = 20;
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();

    // Extra btrees sharing the primary's blob_dev and node cache — the "2 btrees on same blob_dev must work" design
    // invariant, exercised under concurrency rather than sequentially.
    std::vector< shared< TestBtree > > extras;
    for (uint32_t b = 0; b < n_extra; ++b) {
        auto cfg2 = cfg_;
        cfg2.btree_name_ = fmt::format("{}_multi{}", BTREE_NAME, b);
        extras.push_back(
            iomgr().spawn_and_block(ReactorTarget::any(), cow_btree_mgr().create_cow_btree< K, V >(cfg2, blob_dev_)));
    }

    std::atomic< bool > done{false};
    std::thread cp_thread{[this, &done]() {
        while (!done.load(std::memory_order_acquire)) {
            flush_incremental();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }};

    // One worker per btree (primary + extras), all inserting concurrently against the shared cache/blob_dev while
    // the CP thread flushes every btree's dirty set.
    LeafSet primary;
    std::vector< LeafSet > extra_contents{n_extra};
    std::vector< std::thread > workers;
    workers.emplace_back([this, KVS, &primary]() {
        primary = insert_leaves_worker(COWBtree::cast_to(bt_.get()), leaves_per_btree, KVS, 1'000'000ull, 10'000u);
    });
    for (uint32_t b = 0; b < n_extra; ++b) {
        workers.emplace_back([this, b, KVS, &extras, &extra_contents]() {
            extra_contents[b] = insert_leaves_worker(COWBtree::cast_to(extras[b].get()), leaves_per_btree, KVS,
                                                     1'000'000ull * (b + 2), 10'000u * (b + 2));
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    done.store(true, std::memory_order_release);
    cp_thread.join();

    // Verify every btree while all are alive (cross-btree cache interference would show here), then destroy the
    // extras — each destroy evicting its nodes from the cache that still holds the other btrees' hot nodes.
    for (uint32_t b = 0; b < n_extra; ++b) {
        EXPECT_EQ(verify_leaves(COWBtree::cast_to(extras[b].get()), extra_contents[b]), 0u) << "extra btree " << b;
    }
    for (auto& extra : extras) {
        cshared< BtreeBase > base = extra;
        iomgr().spawn_and_block(ReactorTarget::any(), cow_btree_mgr().destroy_cow_btree(base));
    }
    extras.clear();

    for (auto& [id, pairs] : primary) {
        shadow_[id] = std::move(pairs);
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, ConcurrentLockUpgradeWithCp) {
    static constexpr uint32_t n_nodes = 8;
    static constexpr uint32_t n_threads = 4;
    static constexpr uint32_t rounds = 5;
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();
    ASSERT_GE(KVS, n_threads) << "need at least one slot per thread";

    std::vector< bnodeid_t > ids;
    ids.reserve(n_nodes);
    for (uint32_t i = 0; i < n_nodes; ++i) {
        ids.push_back(create_leaf(KVS));
    }
    flush_incremental();

    std::atomic< bool > done{false};
    std::thread cp_thread{[this, &done]() {
        while (!done.load(std::memory_order_acquire)) {
            flush_incremental();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }};

    // Mimics BtreeBase::upgrade_node_locks (btree_base.cpp:82) on pairs of shared nodes: read-lock both, snapshot
    // gens, release both, re-acquire as write, prepare_for_write, then revalidate gens.  A competing thread's
    // update landing inside the released window bumps the gen (SimpleNode::update → inc_gen) and forces the retry
    // — the same geometry as a concurrent do_put descent racing CP flushes.  All acquisitions (read and write
    // phase) go in ascending node order and pairs never wrap, so no lock cycle is possible.
    uint32_t const slots_per_thread = KVS / n_threads;
    std::atomic< uint64_t > gen_retries{0};
    std::atomic< uint64_t > cp_retries{0};
    std::vector< std::thread > workers;
    for (uint32_t t = 0; t < n_threads; ++t) {
        workers.emplace_back([this, t, slots_per_thread, &ids, &gen_retries, &cp_retries]() {
            uint32_t const slot_lo = t * slots_per_thread;
            for (uint32_t round = 0; round < rounds; ++round) {
                for (uint32_t i = 0; i + 1 < n_nodes; ++i) {
                    bnodeid_t const parent_id = ids[i];
                    bnodeid_t const child_id = ids[i + 1];
                    for (uint32_t s = slot_lo; s < slot_lo + slots_per_thread; ++s) {
                        uint32_t const new_val = 500'000u * (t + 1) + 1'000u * round + s;
                        iomgr().spawn_and_block(
                            ReactorTarget::any(),
                            [this, parent_id, child_id, s, new_val, &gen_retries, &cp_retries]() -> Async< void > {
                                auto* cow = COWBtree::cast_to(bt_.get());
                                while (true) {
                                    auto pres = co_await cow->read_node(parent_id, LockType::Read);
                                    HS_REL_ASSERT(pres.hasValue(), "read_node(parent) failed");
                                    auto parent = std::move(pres.value());
                                    auto cres = co_await cow->read_node(child_id, LockType::Read);
                                    HS_REL_ASSERT(cres.hasValue(), "read_node(child) failed");
                                    auto child = std::move(cres.value());

                                    auto const parent_gen = parent->node_gen();
                                    auto const child_gen = child->node_gen();

                                    // The dangerous window upgrade_node_locks opens: both locks dropped.
                                    child.release();
                                    parent.release();

                                    co_await parent.acquire(LockType::Write);
                                    if (auto st = cow->prepare_for_write(parent); st != BtreeStatus::success) {
                                        HS_REL_ASSERT_EQ(st, BtreeStatus::retry, "prepare_for_write(parent)");
                                        cp_retries.fetch_add(1, std::memory_order_relaxed);
                                        parent.release();
                                        continue;
                                    }
                                    co_await child.acquire(LockType::Write);
                                    if (auto st = cow->prepare_for_write(child); st != BtreeStatus::success) {
                                        HS_REL_ASSERT_EQ(st, BtreeStatus::retry, "prepare_for_write(child)");
                                        cp_retries.fetch_add(1, std::memory_order_relaxed);
                                        child.release();
                                        parent.release();
                                        continue;
                                    }

                                    if (parent->is_node_deleted() || (parent_gen != parent->node_gen()) ||
                                        child->is_node_deleted() || (child_gen != child->node_gen())) {
                                        gen_retries.fetch_add(1, std::memory_order_relaxed);
                                        child.release();
                                        parent.release();
                                        continue;
                                    }

                                    auto* leaf = static_cast< LeafNode* >(child.operator->());
                                    V val{new_val};
                                    leaf->update(s, val);
                                    cow->write_node(child);
                                    break; // both nodes unlock via RAII on scope exit
                                }
                                co_return;
                            }());
                    }
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    done.store(true, std::memory_order_release);
    cp_thread.join();
    LOGINFO("ConcurrentLockUpgradeWithCp: gen_retries={} cp_retries={}", gen_retries.load(), cp_retries.load());

    // Each slot has a single writer thread whose last write is round rounds-1; every node in ids[1..n-1] was a
    // "child" (mutated) node.  ids[0] is only ever a parent — its shadow stays at the create_leaf values.
    for (uint32_t i = 1; i < n_nodes; ++i) {
        for (uint32_t t = 0; t < n_threads; ++t) {
            for (uint32_t s = t * slots_per_thread; s < (t + 1) * slots_per_thread; ++s) {
                shadow_[ids[i]][s].second = 500'000u * (t + 1) + 1'000u * (rounds - 1) + s;
            }
        }
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

// One-liner delay-flip arming (mirrors HSTestHelper::set_delay_flip; local because this test boots its own stack).
static void set_delay_flip(std::string const& name, uint64_t delay_usec, uint32_t count = 1, uint32_t percent = 100) {
    flip::FlipFrequencyT freq;
    freq.count = count;
    flip::PercentFrequencyT pf;
    pf.v = percent;
    freq.kind.Set(pf);
    flip::FlipClient::instance().inject_delay_flip(name, {}, freq, delay_usec);
}

TEST_F(CowBtreeLocalTest, PrepareRetryUnderCpSwitchover) {
    // Covers prepare_for_write's newer-cp retry (mod_cp_id > cur_cp_id).  The SAME job runs concurrently on every
    // reactor (spawn_waitable_all_parallel): loop read+write of one shared node until the deadline, firing a CP
    // every few iterations.  The one-shot delay flip stalls whichever job's read hits it first for 100ms — which
    // reactor is immaterial — while that job still holds its CP entry.  The other job keeps writing, and once one
    // of its CP triggers switches the CP its writes land under a newer cp.  When the stalled job resumes, its
    // prepare_for_write sees the newer modification and must retry.
    auto const KVS = SISL_OPTIONS["kvs_per_node"].as< uint32_t >();
    ASSERT_GE(KVS, iomgr().num_reactors()) << "one slot per reactor job";
    bnodeid_t const id = create_leaf(KVS);
    flush_incremental();

    set_delay_flip("cow_read_node_delay", 100'000 /* 100ms */);

    auto* cow = COWBtree::cast_to(bt_.get());
    std::atomic< uint32_t > cp_retries{0};
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};

    // CP cadence from its own thread: trigger_cp_flush is thread-safe, and from a non-reactor thread its internal
    // guard always operates on the genuine current CP (no op pin to nest into).
    std::atomic< bool > stop_cp{false};
    std::thread cp_thread{[&stop_cp]() {
        while (!stop_cp.load(std::memory_order_acquire)) {
            auto fut = cp_mgr().trigger_cp_flush(/*force=*/true, CPTriggerReason::UserDriven);
            (void)fut; // fire-and-forget; the final flush_incremental below settles everything
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }};

    blocking_wait(spawn_waitable_all_parallel([&, this](size_t rid) -> Async< void > {
        while (std::chrono::steady_clock::now() < deadline) {
            auto og = cow->enter_op(); // fresh CP entry per attempt — put's goto-retry shape
            auto res = co_await cow->read_node(id, LockType::Write); // one-shot flip stalls one of us here
            if (!res.hasValue()) {
                HS_REL_ASSERT_EQ(res.error(), BtreeStatus::retry, "read_node");
                cp_retries.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            auto node = std::move(res.value());
            auto st = cow->prepare_for_write(node);
            if (st == BtreeStatus::retry) {
                cp_retries.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            HS_REL_ASSERT_EQ(st, BtreeStatus::success, "prepare_for_write");
            auto* leaf = static_cast< LeafNode* >(node.operator->());
            V val{100'000u + to_u32(rid)};
            leaf->update(to_u32(rid), val); // slot = reactor id: disjoint, shadow stays deterministic
            cow->write_node(node);
        }
        co_return;
    }));

    stop_cp.store(true, std::memory_order_release);
    cp_thread.join();
    flip::Flip::instance().remove("cow_read_node_delay");
    LOGINFO("PrepareRetryUnderCpSwitchover: cp_retries={}", cp_retries.load());
    EXPECT_GE(cp_retries.load(), 1u) << "the flip-stalled job must retry against a newer-cp write";

    for (size_t rid = 0; rid < iomgr().num_reactors(); ++rid) {
        shadow_[id][rid].second = 100'000u + to_u32(rid);
    }
    flush_incremental();
    verify_all();
    restart();
    verify_all();
}

TEST_F(CowBtreeLocalTest, GenericPutRestartLoop) {
    // Drives the ROOTED tree through the generic put/get path across restart rounds: each round inserts enough
    // keys through put_one to force leaf/root splits, checkpoints, restarts, and re-validates EVERY key so far
    // through get_one (descending from the recovered root).  Covers the recover→put→flush→recover composition
    // that per-node-id verification cannot see.
    constexpr uint32_t kRounds = 3;
    constexpr uint64_t kKeysPerRound = 300;
    auto value_for = [](uint64_t k) { return to_u32((k * 2654435761ull + 1) & 0xFFFFFFFFull); };

    uint64_t total = 0;
    auto* self = this;
    for (uint32_t r = 0; r < kRounds; ++r) {
        LOGINFO("GenericPutRestartLoop round {}/{}: inserting keys [{}, {})", r + 1, kRounds, total,
                total + kKeysPerRound);
        iomgr().spawn_and_block(ReactorTarget::any(), [self, total, &value_for]() -> Async< void > {
            for (uint64_t k = total; k < total + kKeysPerRound; ++k) {
                K key{k};
                V val{value_for(k)};
                auto res = co_await self->bt_->put_one(key, val, BtreePutType::UPSERT, nullptr, nullptr);
                HS_REL_ASSERT(res.hasValue(), "put_one failed for key={}", k);
            }
            co_return;
        }());
        total += kKeysPerRound;

        flush_incremental();
        restart();

        uint64_t mismatches = 0;
        uint64_t first_bad = 0;
        iomgr().spawn_and_block(ReactorTarget::any(),
                                [self, total, &value_for, &mismatches, &first_bad]() -> Async< void > {
                                    for (uint64_t k = 0; k < total; ++k) {
                                        K key{k};
                                        auto res = co_await self->bt_->get_one(key);
                                        if (!res.hasValue() || (res.value().value() != value_for(k))) {
                                            if (mismatches == 0) {
                                                first_bad = k;
                                            }
                                            ++mismatches;
                                        }
                                    }
                                    co_return;
                                }());
        ASSERT_EQ(mismatches, 0u) << "round " << r + 1 << ": " << mismatches << " of " << total
                                  << " keys wrong after restart, first bad key=" << first_bad;
    }
}

// ──────────────────────────────────────────── main ───────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_cow_btree_local");
    spdlog::set_pattern("[%D %T.%e%z] [%^%L%$] [%t] %v"); // .%e = milliseconds — needed to see flip delays / races

    auto const seed = SISL_OPTIONS["seed"].as< uint64_t >();
    g_re.seed(seed ? seed : std::chrono::system_clock::now().time_since_epoch().count());

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown.
    return RUN_ALL_TESTS();
}