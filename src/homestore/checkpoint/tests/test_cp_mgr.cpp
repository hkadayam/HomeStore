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
#include "common/async.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <folly/coro/Baton.h>

#include <gtest/gtest.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "common/defs.h"
#include "homestore/base/hs_runtime_config.h"
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

// CP flush rank for the test consumer.  Chosen well below any production rank so this test file remains isolated.
static constexpr uint32_t kCPRank_Test = 100;

// ─── Test CP consumer
// ───────────────────────────────────────────────────────────────────────────────────────────────── Tracks values added
// during each CP session via atomic counter. On flush, validates that all values belong to the expected CP id.
class TestCPCallbacks : public CPCallbacks {
public:
    void on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) override {
        // Reset per-CP counter for the new session.
        next_val_.store(0);
    }

    Async< bool > cp_flush(CP* cp) override {
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
    Async< shared< DeviceManager > > format_and_start_cp() {
        auto dm = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(true /* first_time_boot */);

        test_cb_ = std::make_shared< TestCPCallbacks >();
        cpmgr->register_consumer("test_consumer", test_cb_, kCPRank_Test);

        co_return dm;
    }

    // Reload devices, load MetaBlkManager, start CPManager (recovery path).
    Async< shared< DeviceManager > > reload_and_start_cp() {
        Managers::reset();
        auto dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();
        co_await MetaBlkManager::load();

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(false /* first_time_boot */);

        test_cb_ = std::make_shared< TestCPCallbacks >();
        cpmgr->register_consumer("test_consumer", test_cb_, kCPRank_Test);

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

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &id_before_restart]() -> Async< void > {
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

    iomgr().spawn_and_block(ReactorTarget::any(), [this, id_before_restart]() -> Async< void > {
        auto dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();
        co_await MetaBlkManager::load();
        auto cpmgr = CPManager::create();
        co_await cpmgr->start(false /* first_time_boot */);
        test_cb_ = std::make_shared< TestCPCallbacks >();
        cpmgr->register_consumer("test_consumer", test_cb_, kCPRank_Test);

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
// Test 9: Consumer rank ordering — register three consumers with non-monotonic ranks and verify the CP switchover and
// flush walks visit them in ascending rank order regardless of registration order.  Duplicate ranks are a debug
// assert (HS_DBG_ASSERT in register_consumer, aborting in debug builds); the release-mode adjacent-after fallback is
// documented in CPRank's header and not exercised here (no death-test pattern in this repo).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
class OrderRecordingCallbacks : public CPCallbacks {
public:
    OrderRecordingCallbacks(uint32_t my_rank, std::vector< uint32_t >* switch_order,
                            std::vector< uint32_t >* flush_order, std::mutex* order_mtx) :
            rank_{my_rank}, switch_order_{switch_order}, flush_order_{flush_order}, order_mtx_{order_mtx} {}

    void on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) override {
        std::lock_guard lg{*order_mtx_};
        switch_order_->push_back(rank_);
    }

    Async< bool > cp_flush(CP* /*cp*/) override {
        {
            std::lock_guard lg{*order_mtx_};
            flush_order_->push_back(rank_);
        }
        co_return true;
    }

    void cp_cleanup(CP* /*cp*/) override {}

    int cp_progress_percent() override { return 100; }

private:
    uint32_t rank_;
    std::vector< uint32_t >* switch_order_;
    std::vector< uint32_t >* flush_order_;
    std::mutex* order_mtx_;
};
} // namespace

CORO_TEST_F(CPMgrTest, ConsumerRankOrderingEnforced) {
    auto dm = co_await self.format_and_start_cp();

    std::vector< uint32_t > switch_order;
    std::vector< uint32_t > flush_order;
    std::mutex order_mtx;

    auto cb_high = std::make_shared< OrderRecordingCallbacks >(300u, &switch_order, &flush_order, &order_mtx);
    auto cb_low = std::make_shared< OrderRecordingCallbacks >(50u, &switch_order, &flush_order, &order_mtx);
    auto cb_mid = std::make_shared< OrderRecordingCallbacks >(200u, &switch_order, &flush_order, &order_mtx);

    // Registration order deliberately non-monotonic: high(300), low(50), mid(200).
    cp_mgr().register_consumer("high", cb_high, 300u);
    cp_mgr().register_consumer("low", cb_low, 50u);
    cp_mgr().register_consumer("mid", cb_mid, 200u);

    // register_consumer invokes on_switchover_cp(nullptr, cur_cp) once per call, so switch_order at this point simply
    // mirrors registration order.  Clear it and drive the next switchover via trigger_cp_flush; that path walks the
    // rank-sorted consumers_ list in one pass, which is the ordering under test.
    {
        std::lock_guard lg{order_mtx};
        switch_order.clear();
    }

    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    {
        std::lock_guard lg{order_mtx};
        // format_and_start_cp registered its own test_consumer at rank kCPRank_Test (=100) using TestCPCallbacks, which
        // does not push to our tracking vectors — so we see just the three OrderRecordingCallbacks ranks in ascending
        // order, with rank 100 firing between 50 and 200 unobserved.
        std::vector< uint32_t > expected = {50u, 200u, 300u};
        EXPECT_EQ(switch_order, expected);
        EXPECT_EQ(flush_order, expected);
    }

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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 10: Watchdog detects a stalled CP.  A consumer's cp_flush blocks on a baton and its cp_progress_percent reports
// a value that never advances; the watchdog must observe the stall (progress not advancing beyond the recorded
// watermark) and invoke repair_slow_cp on the lagging consumer.  The panic branch (elapsed past the 12x-timer
// tolerance) is a HS_REL_ASSERT and is not exercised here — asserting on a stuck CP would kill the test process, and
// there is no death-test pattern in this repo.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
class StallableCallbacks : public CPCallbacks {
public:
    void on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) override {}

    Async< bool > cp_flush(CP* /*cp*/) override {
        co_await stall_baton_;
        co_return true;
    }

    void cp_cleanup(CP* /*cp*/) override {}

    int cp_progress_percent() override { return progress_.load(); }

    void repair_slow_cp() override { repair_count_.fetch_add(1); }

    void release() { stall_baton_.post(); }

    folly::coro::Baton stall_baton_;
    std::atomic< uint32_t > progress_{50};
    std::atomic< uint32_t > repair_count_{0};
};
} // namespace

CORO_TEST_F(CPMgrTest, WatchdogDetectsStalledCP) {
    // Shrink the watchdog tick so the test observes stall detection in a few seconds instead of ~2 min at the 10s
    // default.  Captured at CPWatchdog construction (a member of CPManager), so must land before format_and_start_cp.
    HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) { s.checkpoint.cp_watchdog_timer_sec = 1; });
    HS_SETTINGS_FACTORY().save();

    auto dm = co_await self.format_and_start_cp();

    auto stall_cb = std::make_shared< StallableCallbacks >();
    cp_mgr().register_consumer("stallable", stall_cb, 500u);

    // Fire and forget: the flush stalls on the baton, so we cannot co_await its completion here.  The consumer's
    // cp_progress_percent stays at 50, so the watchdog sees progress plateau after its first advance-record and
    // starts nudging repair_slow_cp on subsequent ticks.
    auto fut = cp_mgr().trigger_cp_flush(true /* force */);

    // Sleep past several watchdog ticks (1s each) but well short of the 12x tolerance panic window (12s).
    co_await iomgr().sleep(std::chrono::milliseconds{4000});

    EXPECT_GT(stall_cb->repair_count_.load(), 0u);

    // Release the stall so the flush completes; then drain the future before shutdown, which would otherwise wedge
    // waiting for the inflight CP to finish.
    stall_cb->progress_.store(100);
    stall_cb->release();
    auto success = co_await std::move(fut);
    EXPECT_TRUE(success);

    co_await cp_mgr().shutdown();
    co_await dm->close_devices();

    // Restore watchdog default so subsequent tests in the binary aren't affected.
    HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) { s.checkpoint.cp_watchdog_timer_sec = 10; });
    HS_SETTINGS_FACTORY().save();
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
