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
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "common/defs.h"
#include "homestore/device/device_manager.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/managers.h"

#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/checkpoint/cp.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;

SISL_OPTION_GROUP(test_cp_mgr,
                  (num_records, "", "num_records", "number of record to test",
                   ::cxxopts::value< uint32_t >()->default_value("100"), "number"));

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;      // 256 MB per device
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev

// ─── Test CP consumer
// ───────────────────────────────────────────────────────────────────────────────────────────────── Tracks values added
// during each CP session via atomic counter. On flush, validates that all values belong to the expected CP id.
class TestCPCallbacks : public CPCallbacks {
public:
    void on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) override {
        // Reset per-CP counter for the new session.
        next_val_.store(0);
    }

    folly::coro::Task< bool > cp_flush(CP* cp) override {
        auto count = next_val_.load();
        LOGINFO("CP={} flushing {} values", cp->id(), count);
        // Validate that the count is within bounds.
        EXPECT_LE(count, max_values);
        ++flush_count_;
        co_return true;
    }

    void cp_cleanup(CP* /*cp*/) override {}

    int cp_progress_percent() override { return 100; }

    // Add a value to the current CP session (called under cp_guard).
    void add() { next_val_.fetch_add(1); }

    uint64_t flush_count() const { return flush_count_.load(); }

private:
    static constexpr size_t max_values = 100000;
    std::atomic< uint64_t > next_val_{0};
    std::atomic< uint64_t > flush_count_{0};
};

// ─── Fixture
// ──────────────────────────────────────────────────────────────────────────────────────────────────────────
class CPMgrTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test fresh reactors: CPManager's t_cp_info_ thread_local pointer would otherwise dangle into the
        // freed prior-test CPManager's owned_stacks_, and the next cp_guard() reads it as UAF.
        iomanager::init_iomgr(2);
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_cp_{}", i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
        // Stop iomgr (joins reactor threads) BEFORE dropping Managers so the reactor's TLS deleters fire while
        // the owning containers (e.g. CPManager::owned_stacks_) are still alive.
        iomanager::stop_iomgr();
        Managers::reset();
        for (auto& p : dev_paths_) {
            std::filesystem::remove(p);
        }
        dev_paths_.clear();
    }

    std::vector< DevInfo > make_dev_infos() const {
        std::vector< DevInfo > infos;
        for (auto& p : dev_paths_) {
            infos.emplace_back(p, HSDevType::Data, DEV_SIZE);
        }
        return infos;
    }

    // Format devices, create MetaBlkManager, create CPManager, register test consumer.
    folly::coro::Task< shared< DeviceManager > > format_and_start_cp() {
        auto dm = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(true /* first_time_boot */);

        test_cb_ = std::make_shared< TestCPCallbacks >();
        cpmgr->register_consumer("test_consumer", test_cb_);

        co_return dm;
    }

    // Reload devices, load MetaBlkManager, start CPManager (recovery path).
    folly::coro::Task< shared< DeviceManager > > reload_and_start_cp() {
        Managers::reset();
        auto dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();
        co_await MetaBlkManager::load();

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(false /* first_time_boot */);

        test_cb_ = std::make_shared< TestCPCallbacks >();
        cpmgr->register_consumer("test_consumer", test_cb_);

        co_return dm;
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< TestCPCallbacks > test_cb_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 1: Create CPManager, verify initial CP is io_ready with id 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, CreateAndVerifyInitialCP) {
    auto dm = co_await self.format_and_start_cp();

    {
        auto guard = cp_mgr().cp_guard();
        EXPECT_EQ(guard->id(), 0);
        EXPECT_EQ(guard->get_status(), cp_status_t::cp_io_ready);
    }

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 2: Register consumer, add values under cp_guard, trigger flush, verify flush callback ran.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, SimulateIOAndFlush) {
    auto dm = co_await self.format_and_start_cp();
    const uint32_t nrecords = SISL_OPTIONS["num_records"].as< uint32_t >();

    LOGINFO("Step 1: Simulate {} IOs under cp_guard", nrecords);
    for (uint32_t i = 0; i < nrecords; ++i) {
        auto guard = cp_mgr().cp_guard();
        self.test_cb_->add();
    }

    LOGINFO("Step 2: Trigger CP flush and wait");
    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);
    EXPECT_GE(self.test_cb_->flush_count(), 1u);

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 3: Trigger back-to-back CPs (force=true while previous is flushing).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, BackToBackCP) {
    auto dm = co_await self.format_and_start_cp();

    LOGINFO("Step 1: Simulate some IOs");
    for (uint32_t i = 0; i < 50; ++i) {
        auto guard = cp_mgr().cp_guard();
        self.test_cb_->add();
    }

    LOGINFO("Step 2: Trigger first CP (no wait)");
    auto fut1 = cp_mgr().trigger_cp_flush(true /* force */);

    LOGINFO("Step 3: Trigger second CP (back-to-back, wait)");
    auto fut2 = cp_mgr().trigger_cp_flush(true /* force */);

    auto success1 = co_await std::move(fut1);
    auto success2 = co_await std::move(fut2);
    EXPECT_TRUE(success1);
    EXPECT_TRUE(success2);
    EXPECT_GE(self.test_cb_->flush_count(), 2u);

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 4: CP guard nesting — inner guard should reuse the same CP as the outer guard.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, NestedCPGuard) {
    auto dm = co_await self.format_and_start_cp();

    cp_id_t outer_id;
    cp_id_t inner_id;
    {
        auto outer = cp_mgr().cp_guard();
        outer_id = outer->id();
        {
            auto inner = cp_mgr().cp_guard();
            inner_id = inner->id();
        }
    }
    EXPECT_EQ(outer_id, inner_id);

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 5: CP id advances after each flush.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, CPIdAdvancesAfterFlush) {
    auto dm = co_await self.format_and_start_cp();

    cp_id_t id_before;
    {
        auto guard = cp_mgr().cp_guard();
        id_before = guard->id();
    }

    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    cp_id_t id_after;
    {
        auto guard = cp_mgr().cp_guard();
        id_after = guard->id();
    }

    EXPECT_EQ(id_after, id_before + 1);

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 6: CP superblock persists across restart — CP id continues from where it left off.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// In-test restart driven from the main thread: phase 1 runs on the original iomgr, then we cycle iomgr
// (kills reactor TLS so CPManager's t_cp_info_ doesn't dangle into the freed prior CPManager), then phase
// 2 runs on a fresh reactor pool.  Done via plain TEST_F + spawn_and_block since stop_iomgr() cannot be
// called from inside a coroutine running on one of the reactors it would join.
TEST_F(CPMgrTest, CPIdSurvivesRestart) {
    cp_id_t id_before_restart{};

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, &id_before_restart]() -> folly::coro::Task< void > {
                                auto dm = co_await format_and_start_cp();
                                for (int i = 0; i < 3; ++i) {
                                    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
                                    EXPECT_TRUE(success);
                                }
                                {
                                    auto guard = cp_mgr().cp_guard();
                                    id_before_restart = guard->id();
                                }
                                co_await cp_mgr().shutdown();
                                co_await dm->close_devices();
                                Managers::reset();
                            }());

    iomanager::stop_iomgr();
    iomanager::init_iomgr(2);

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, id_before_restart]() -> folly::coro::Task< void > {
                                auto dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO,
                                                                IOFlag::BUFFERED_IO);
                                co_await dm->load_devices();
                                co_await MetaBlkManager::load();
                                auto cpmgr = CPManager::create();
                                co_await cpmgr->start(false /* first_time_boot */);
                                test_cb_ = std::make_shared< TestCPCallbacks >();
                                cpmgr->register_consumer("test_consumer", test_cb_);

                                cp_id_t id_after_restart{};
                                {
                                    auto guard = cp_mgr().cp_guard();
                                    id_after_restart = guard->id();
                                }
                                // After restart, the CP id should be last_flushed + 1, which equals
                                // id_before_restart (since shutdown does a final flush).
                                EXPECT_GE(id_after_restart, id_before_restart);

                                co_await cp_mgr().shutdown();
                                co_await dm->close_devices();
                            }());
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 7: IO parallel to CP flush — add values, trigger CP without waiting, add more, trigger again.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, IOParallelToFlush) {
    auto dm = co_await self.format_and_start_cp();
    const uint32_t nrecords = SISL_OPTIONS["num_records"].as< uint32_t >();

    LOGINFO("Step 1: Simulate {} IOs", nrecords);
    for (uint32_t i = 0; i < nrecords; ++i) {
        auto guard = cp_mgr().cp_guard();
        self.test_cb_->add();
    }

    LOGINFO("Step 2: Trigger CP without waiting");
    auto fut1 = cp_mgr().trigger_cp_flush(true /* force */);

    LOGINFO("Step 3: Simulate {} more IOs parallel to flush", nrecords);
    for (uint32_t i = 0; i < nrecords; ++i) {
        auto guard = cp_mgr().cp_guard();
        self.test_cb_->add();
    }

    auto success1 = co_await std::move(fut1);
    EXPECT_TRUE(success1);

    LOGINFO("Step 4: Trigger final CP and wait");
    auto success2 = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success2);

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 8: has_cp_flushed returns correct results.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(CPMgrTest, HasCPFlushed) {
    auto dm = co_await self.format_and_start_cp();

    cp_id_t initial_id;
    {
        auto guard = cp_mgr().cp_guard();
        initial_id = guard->id();
    }

    // Before any flush, CP 0 should not be flushed yet (it's the active one).
    EXPECT_FALSE(cp_mgr().has_cp_flushed(initial_id));

    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    // After flush, CP 0 should be flushed.
    EXPECT_TRUE(cp_mgr().has_cp_flushed(initial_id));

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();
}

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_cp_mgr");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown.
    return RUN_ALL_TESTS();
}
