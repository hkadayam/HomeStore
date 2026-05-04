/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <sisl/fds/buffer.h>

#include "iomanager/iomanager.h"
#include "base/test_defs.h"
#include "base/homestore_config.hpp"

#include "common/defs.h"
#include "device/device_manager.h"
#include "meta/meta_blk_manager.h"
#include "managers.h"

#include <homestore/checkpoint/cp_mgr.h>

#include "logstore/log_store.h"
#include "logstore/log_store_mgr.h"

using namespace homestore;
using namespace iomanager;

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024;

// ─────────────────────────────────────────────────────────────────────────────
// Fixture mirrors test_log_store: bootstrap DM + Meta + CP + LogStoreManager.  CP is required because the
// manager is wired as a CP consumer (today a no-op for log truncation; placeholder for future log-truncate-on-cp).
// ─────────────────────────────────────────────────────────────────────────────
class LogStoreMgrTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_log_store_mgr_{}", i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
        Managers::reset();
        for (auto& p : dev_paths_) {
            std::filesystem::remove(p);
        }
    }

    std::vector< DevInfo > make_dev_infos() const {
        std::vector< DevInfo > infos;
        for (auto& p : dev_paths_) {
            infos.emplace_back(p, HSDevType::Data, DEV_SIZE);
        }
        return infos;
    }

    folly::coro::Task< void > bootstrap() {
        dm_ = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(true /* first_time_boot */);

        co_await LogStoreManager::create(/*chunk_size=*/4 * 1024 * 1024, /*initial_num_chunks=*/1);
    }

    folly::coro::Task< void > reload() {
        co_await log_store_mgr().shutdown();
        co_await cp_mgr().shutdown();
        co_await dm_->close_devices();
        Managers::reset();

        dm_ = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm_->load_devices();
        co_await MetaBlkManager::load();

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(false /* first_time_boot */);

        co_await LogStoreManager::load();
    }

    folly::coro::Task< void > shutdown() {
        co_await log_store_mgr().shutdown();
        co_await cp_mgr().shutdown();
        co_await dm_->close_devices();
    }

    // Convenience: append `n` records of `size` bytes each to the given store, then flush.  Returns the LSNs
    // assigned, in order.
    static folly::coro::Task< std::vector< lsn_t > > append_n(LogStore& store, uint32_t n, size_t size,
                                                              std::vector< std::shared_ptr< std::vector< uint8_t > > >& keep) {
        std::vector< lsn_t > lsns;
        lsns.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            auto buf = std::make_shared< std::vector< uint8_t > >(size, static_cast< uint8_t >(0xCD));
            keep.push_back(buf);
            sisl::IoBlob blob{buf->data(), to_u32(buf->size()), false};
            lsns.push_back(store.quick_append(blob));
        }
        co_await store.flush();
        co_return lsns;
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< DeviceManager > dm_;
};

// ── Lifecycle ───────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(LogStoreMgrTest, RecoverWithoutAnyStores) {
    co_await self.bootstrap();
    EXPECT_EQ(log_store_mgr().log_stores().size(), 0u);
    co_await self.reload();
    EXPECT_EQ(log_store_mgr().log_stores().size(), 0u);
    co_await log_store_mgr().recover(); // no-op
    co_await self.shutdown();
}

CORO_TEST_F(LogStoreMgrTest, CreateOpenRecover) {
    co_await self.bootstrap();
    constexpr uint32_t kStores = 3;
    constexpr uint32_t kRecordsPer = 8;

    std::vector< logstore_id_t > sids;
    {
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        for (uint32_t i = 0; i < kStores; ++i) {
            auto store = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
            sids.push_back(store->store_id());
            log_store_mgr().open_log_store(store->store_id(), [](lsn_t, const sisl::ByteView&) {});
            co_await self.append_n(*store, kRecordsPer, 128, keep);
        }
    }
    co_await self.reload();
    EXPECT_EQ(log_store_mgr().log_stores().size(), kStores);

    // Open all stores with replay-counting handlers, then recover.
    std::map< logstore_id_t, std::atomic< uint32_t > > replay_counts;
    for (auto sid : sids) {
        auto& cnt = replay_counts[sid];
        log_store_mgr().open_log_store(sid, [&cnt](lsn_t, const sisl::ByteView&) {
            cnt.fetch_add(1, std::memory_order_relaxed);
        });
    }
    co_await log_store_mgr().recover();

    for (auto sid : sids) {
        EXPECT_EQ(replay_counts[sid].load(), kRecordsPer) << "sid=" << sid;
    }
    co_await self.shutdown();
}

CORO_TEST_F(LogStoreMgrTest, DropUnopenedStores) {
    co_await self.bootstrap();
    constexpr uint32_t kStores = 3;

    std::vector< logstore_id_t > sids;
    {
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        for (uint32_t i = 0; i < kStores; ++i) {
            auto store = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
            sids.push_back(store->store_id());
            log_store_mgr().open_log_store(store->store_id(), [](lsn_t, const sisl::ByteView&) {});
            co_await self.append_n(*store, 4, 128, keep);
        }
    }
    co_await self.reload();
    CO_ASSERT_EQ(log_store_mgr().log_stores().size(), kStores);

    // Open only sids[0] and sids[2]; sids[1] is unopened — drop_unopened_stores should remove it.
    std::map< logstore_id_t, std::atomic< uint32_t > > replay_counts;
    log_store_mgr().open_log_store(sids[0], [&replay_counts, sid = sids[0]](lsn_t, const sisl::ByteView&) {
        replay_counts[sid].fetch_add(1);
    });
    log_store_mgr().open_log_store(sids[2], [&replay_counts, sid = sids[2]](lsn_t, const sisl::ByteView&) {
        replay_counts[sid].fetch_add(1);
    });

    co_await log_store_mgr().recover();

    // sids[1] should be dropped post-recover.
    EXPECT_EQ(log_store_mgr().log_stores().size(), 2u) << "unopened sid should be dropped";
    EXPECT_NE(log_store_mgr().get_log_store(sids[0]), nullptr);
    EXPECT_EQ(log_store_mgr().get_log_store(sids[1]), nullptr) << "unopened sid removed";
    EXPECT_NE(log_store_mgr().get_log_store(sids[2]), nullptr);

    EXPECT_EQ(replay_counts[sids[0]].load(), 4u);
    EXPECT_EQ(replay_counts[sids[2]].load(), 4u);

    // Restart again and verify the dropped store stays gone (its sb mblk was removed).
    co_await self.reload();
    EXPECT_EQ(log_store_mgr().log_stores().size(), 2u) << "dropped store stays gone across restart";
    EXPECT_EQ(log_store_mgr().get_log_store(sids[1]), nullptr);

    co_await self.shutdown();
}

CORO_TEST_F(LogStoreMgrTest, OrphanRecordsSilentlyDropped) {
    co_await self.bootstrap();

    // Create 2 stores, write to both, restart, but only OPEN one of them.  The other is "orphaned" — its
    // records survive in the LogStream chain but on_log_found for them dispatches to an unopened LogStore,
    // which has no handler so the records are silently dropped during recover.  After recover, the unopened
    // store is dropped (same as DropUnopenedStores).
    logstore_id_t kept_sid;
    logstore_id_t orphan_sid;
    {
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto kept = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        auto orphan = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        kept_sid = kept->store_id();
        orphan_sid = orphan->store_id();
        log_store_mgr().open_log_store(kept_sid, [](lsn_t, const sisl::ByteView&) {});
        log_store_mgr().open_log_store(orphan_sid, [](lsn_t, const sisl::ByteView&) {});
        co_await self.append_n(*kept, 5, 128, keep);
        co_await self.append_n(*orphan, 5, 128, keep);
    }
    co_await self.reload();

    // Open only `kept`.  Don't open `orphan`.
    std::atomic< uint32_t > kept_replay{0};
    log_store_mgr().open_log_store(kept_sid, [&kept_replay](lsn_t, const sisl::ByteView&) { kept_replay.fetch_add(1); });

    co_await log_store_mgr().recover();

    EXPECT_EQ(kept_replay.load(), 5u) << "kept store sees its 5 records";
    EXPECT_EQ(log_store_mgr().log_stores().size(), 1u) << "orphan store dropped after recover";
    EXPECT_EQ(log_store_mgr().get_log_store(orphan_sid), nullptr);

    co_await self.shutdown();
}

CORO_TEST_F(LogStoreMgrTest, CreateAfterRecoverContinuesIds) {
    co_await self.bootstrap();
    constexpr uint32_t kInitialStores = 4;
    std::vector< logstore_id_t > original_sids;
    {
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        for (uint32_t i = 0; i < kInitialStores; ++i) {
            auto store = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
            original_sids.push_back(store->store_id());
            log_store_mgr().open_log_store(store->store_id(), [](lsn_t, const sisl::ByteView&) {});
            co_await self.append_n(*store, 2, 64, keep);
        }
    }
    co_await self.reload();

    // Re-open all so they aren't dropped.
    for (auto sid : original_sids) {
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::ByteView&) {});
    }
    co_await log_store_mgr().recover();
    CO_ASSERT_EQ(log_store_mgr().log_stores().size(), kInitialStores);

    // The next created store should get sid >= max(original_sids) + 1.  Allocator must NOT collide with
    // recovered ids.
    auto fresh = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    const logstore_id_t expected_min = *std::max_element(original_sids.begin(), original_sids.end()) + 1;
    EXPECT_GE(fresh->store_id(), expected_min) << "new sid must not collide with recovered sids";

    co_await self.shutdown();
}

// ── Test main ─────────────────────────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_log_store_mgr");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    iomanager::init_iomgr(2);
    auto ret = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return ret;
}