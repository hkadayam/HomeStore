#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <folly/coro/Task.h>
#include <folly/coro/ViaIfAsync.h>

#include "common/async.h"
#include "common/defs.h"
#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"
#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/managers.h"

#include "homedb/db_table/db.h"
#include "homedb/db_table/table.h"
#include "homedb/home_db.h"
#include "homedb/wal/journal.h"

using namespace homedb;
using namespace iomanager;

SISL_OPTION_GROUP(test_homedb_basic,
                  (num_threads, "", "num_threads", "iomgr reactor count",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"))

static constexpr uint64_t DEV_SIZE = 1024ull * 1024 * 1024;
static constexpr size_t NUM_DEVS = 2;

class HomeDBTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < NUM_DEVS; ++i) {
            auto path = fmt::format("/tmp/hs_test_homedb_{}_{}", ::getpid(), i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(s_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
        iomanager::init_iomgr(SISL_OPTIONS["num_threads"].as< uint32_t >());
    }

    void TearDown() override {
        iomanager::stop_iomgr();
        for (auto& p : dev_paths_) {
            std::filesystem::remove(p);
        }
        dev_paths_.clear();
    }

    std::vector< DeviceSpec > device_specs() const {
        std::vector< DeviceSpec > out;
        out.reserve(dev_paths_.size());
        for (auto const& p : dev_paths_) {
            out.push_back(DeviceSpec{p, DEV_SIZE});
        }
        return out;
    }

    static Async< void > force_cp_flush() {
        auto fut = homestore::cp_mgr().trigger_cp_flush(/*force=*/true, homestore::CPTriggerReason::UserDriven);
        co_await std::move(fut).via(co_await folly::coro::co_current_executor);
        co_return;
    }

    std::vector< std::string > dev_paths_;
};

// ── Golden path: fresh boot → create Database + table → db.put / db.get in the same session ────────────────────

CORO_TEST_F(HomeDBTest, PutGetSameSession) {
    auto hs = co_await HomeDB::start(self.device_specs());
    auto db = co_await Database::open(JournalConfig{});

    auto table_result = co_await db->create_table("t1", TableSpec::fixed_kv(8, 128));
    CO_ASSERT_TRUE(table_result.hasValue());

    uint64_t const k = 42;
    std::vector< uint8_t > const v(128, 0xAB);
    sisl::Blob key_blob{r_cast< uint8_t const* >(&k), to_u32(sizeof(k))};
    sisl::Blob val_blob{v.data(), to_u32(v.size())};

    auto put_result = co_await db->put("t1", key_blob, val_blob);
    CO_ASSERT_TRUE(put_result.hasValue());

    auto get_result = co_await db->get("t1", key_blob);
    CO_ASSERT_TRUE(get_result.hasValue());
    auto retrieved = get_result.value();
    CO_ASSERT_TRUE(retrieved != nullptr);
    CO_ASSERT_EQ(retrieved->size(), 128u);
    EXPECT_EQ(std::memcmp(retrieved->cbytes(), v.data(), 128), 0);

    db.reset();
    co_await hs->shutdown();
}

// ── Restart: fresh boot → put 100 keys → CP flush → shutdown → recover → all keys readable ─────────────────────

TEST_F(HomeDBTest, PutGetAcrossRestart) {
    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        auto hs = co_await HomeDB::start(this->device_specs());
        auto db = co_await Database::open(JournalConfig{});

        auto table_result = co_await db->create_table("t1", TableSpec::fixed_kv(8, 128));
        ASSERT_TRUE(table_result.hasValue());

        for (uint64_t k = 0; k < 100; ++k) {
            std::vector< uint8_t > const v(128, s_cast< uint8_t >(k));
            sisl::Blob key_blob{r_cast< uint8_t const* >(&k), to_u32(sizeof(k))};
            sisl::Blob val_blob{v.data(), to_u32(v.size())};
            auto put_result = co_await db->put("t1", key_blob, val_blob);
            ASSERT_TRUE(put_result.hasValue());
        }
        co_await force_cp_flush();

        db.reset();
        co_await hs->shutdown();
        co_return;
    }());

    iomanager::stop_iomgr();
    iomanager::init_iomgr(SISL_OPTIONS["num_threads"].as< uint32_t >());

    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        auto hs = co_await HomeDB::start(this->device_specs());
        auto db = co_await Database::open(JournalConfig{});

        for (uint64_t k = 0; k < 100; ++k) {
            sisl::Blob key_blob{r_cast< uint8_t const* >(&k), to_u32(sizeof(k))};
            auto get_result = co_await db->get("t1", key_blob);
            ASSERT_TRUE(get_result.hasValue());
            auto retrieved = get_result.value();
            ASSERT_TRUE(retrieved != nullptr) << "key " << k << " missing after restart";
            ASSERT_EQ(retrieved->size(), 128u);
            std::vector< uint8_t > const expected(128, s_cast< uint8_t >(k));
            EXPECT_EQ(std::memcmp(retrieved->cbytes(), expected.data(), 128), 0)
                << "value mismatch for key " << k;
        }
        db.reset();
        co_await hs->shutdown();
        co_return;
    }());
}

// ── WAL recovery: put → shutdown WITHOUT CP → Database::open drives replay → all puts visible ──────────────────

TEST_F(HomeDBTest, WalReplayRecoversPuts) {
    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        auto hs = co_await HomeDB::start(this->device_specs());
        auto db = co_await Database::open(JournalConfig{});
        auto table_result = co_await db->create_table("t1", TableSpec::fixed_kv(8, 64));
        ASSERT_TRUE(table_result.hasValue());

        for (uint64_t k = 0; k < 50; ++k) {
            std::vector< uint8_t > const v(64, s_cast< uint8_t >(k + 100));
            sisl::Blob key_blob{r_cast< uint8_t const* >(&k), to_u32(sizeof(k))};
            sisl::Blob val_blob{v.data(), to_u32(v.size())};
            auto put_result = co_await db->put("t1", key_blob, val_blob);
            ASSERT_TRUE(put_result.hasValue());
        }
        // Deliberately no force_cp_flush — puts stay only in the WAL.
        db.reset();
        co_await hs->shutdown();
        co_return;
    }());

    iomanager::stop_iomgr();
    iomanager::init_iomgr(SISL_OPTIONS["num_threads"].as< uint32_t >());

    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        auto hs = co_await HomeDB::start(this->device_specs());
        auto db = co_await Database::open(JournalConfig{});

        for (uint64_t k = 0; k < 50; ++k) {
            sisl::Blob key_blob{r_cast< uint8_t const* >(&k), to_u32(sizeof(k))};
            auto get_result = co_await db->get("t1", key_blob);
            ASSERT_TRUE(get_result.hasValue());
            auto retrieved = get_result.value();
            ASSERT_TRUE(retrieved != nullptr) << "key " << k << " missing after WAL replay";
            ASSERT_EQ(retrieved->size(), 64u);
            std::vector< uint8_t > const expected(64, s_cast< uint8_t >(k + 100));
            EXPECT_EQ(std::memcmp(retrieved->cbytes(), expected.data(), 64), 0)
                << "value mismatch for key " << k;
        }
        db.reset();
        co_await hs->shutdown();
        co_return;
    }());
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_homedb_basic");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");
    return RUN_ALL_TESTS();
}
