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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <fmt/format.h>
#include "common/async.h"
#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "sisl/fds/buffer.h"

#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"
#include "homestore/base/hs_runtime_config.h"

#include "common/defs.h"
#include "homestore/device/device_manager.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/managers.h"

#include "homestore/checkpoint/cp_mgr.h"

#include "homestore/logstore/log_store.h"
#include "homestore/logstore/log_store_mgr.h"

using namespace homestore;
using namespace iomanager;

// ── Per-test scaffolding ────────────────────────────────────────────────────────────────────────────────────────────

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;      // 256 MB per backing file
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev

// Process-wide keep-alive for buffers fed into quick_append from worker threads in the concurrency test.  The
// per-store ShadowStore::materialize() requires an LSN we don't know in advance for the threaded path, so we
// stash buffers here instead and only validate counts.
static std::mutex global_keepalive_mtx_;
static std::vector< std::shared_ptr< std::vector< uint8_t > > > global_keepalive_;

// ─────────────────────────────────────────────────────────────────────────────
// ShadowStore — test wrapper around a real LogStore.  Owns the shared<LogStore> handed out by the manager,
// registers a deterministic-pattern replay handler, and tracks issued / completed / recovered LSNs so tests can
// validate without keeping per-record buffers alive.  Pattern is `(lsn ^ i)` per 8-byte word — same scheme as
// test_log_stream, no salting needed since we'll embed the LSN itself.
// ─────────────────────────────────────────────────────────────────────────────
class ShadowStore {
public:
    ShadowStore(shared< LogStore > store) : store_{std::move(store)} {
        store_->open([this](lsn_t lsn, const sisl::IoBufView& data) { record_replay(lsn, data); });
    }

    LogStore& store() { return *store_; }
    logstore_id_t store_id() const { return store_->store_id(); }

    // ── Deterministic data per LSN ──────────────────────────────────────────
    static std::vector< uint8_t > make_data(lsn_t lsn, size_t size) {
        std::vector< uint8_t > buf(size);
        auto* p = reinterpret_cast< uint64_t* >(buf.data());
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            p[i] = static_cast< uint64_t >(lsn) ^ i;
        }
        for (size_t i = (size / sizeof(uint64_t)) * sizeof(uint64_t); i < size; ++i) {
            buf[i] = static_cast< uint8_t >(lsn + i);
        }
        return buf;
    }

    static bool verify_data(const uint8_t* data, size_t size, lsn_t lsn) {
        auto* p = reinterpret_cast< const uint64_t* >(data);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            if (p[i] != (static_cast< uint64_t >(lsn) ^ i)) {
                return false;
            }
        }
        for (size_t i = (size / sizeof(uint64_t)) * sizeof(uint64_t); i < size; ++i) {
            if (data[i] != static_cast< uint8_t >(lsn + i)) {
                return false;
            }
        }
        return true;
    }

    // Pre-allocate, fill, and stash a buffer that outlives the next flush.  Returns IoBufSpan view + the keep-alive
    // shared_ptr is held internally.
    sisl::IoBufSpan materialize(lsn_t lsn, size_t size) {
        auto buf = std::make_shared< std::vector< uint8_t > >(make_data(lsn, size));
        std::lock_guard lk{mtx_};
        keep_alive_.push_back(buf);
        return sisl::IoBufSpan{buf->data(), to_u32(buf->size()), false};
    }

    // ── Replay tracking ─────────────────────────────────────────────────────
    void record_replay(lsn_t lsn, const sisl::IoBufView& data) {
        std::lock_guard lk{mtx_};
        replayed_.emplace_back(lsn, std::vector< uint8_t >(data.bytes(), data.bytes() + data.size()));
    }

    std::vector< std::pair< lsn_t, std::vector< uint8_t > > > replayed() const {
        std::lock_guard lk{mtx_};
        return replayed_;
    }

    size_t replayed_count() const {
        std::lock_guard lk{mtx_};
        return replayed_.size();
    }

    void reset_replay() {
        std::lock_guard lk{mtx_};
        replayed_.clear();
    }

private:
    shared< LogStore > store_;
    mutable std::mutex mtx_;
    std::vector< std::shared_ptr< std::vector< uint8_t > > > keep_alive_;
    std::vector< std::pair< lsn_t, std::vector< uint8_t > > > replayed_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Fixture — bootstraps DM + MetaBlk + CP + LogStoreManager.  CP is required because LogStoreManager will (in
// future) register as a CP consumer for log-truncation-on-checkpoint; for now CP doesn't drive any log work but
// has to exist for the manager to live alongside it cleanly.
// ─────────────────────────────────────────────────────────────────────────────
class LogStoreTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test fresh reactors: CPManager's t_cp_info_ thread_local pointer would otherwise dangle into the
        // freed prior-test CPManager's owned_stacks_, and the next cp_guard() reads it as UAF.
        iomanager::init_iomgr(2);
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_log_store_{}", i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
        // Stop iomgr (joins reactor threads) BEFORE Managers::reset() so the reactor's TLS deleters fire
        // while the owning containers (ConcurrentInsertSet's zombies_, CPManager's owned_stacks_) are still
        // alive.
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

    Async< void > bootstrap() {
        dm_ = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(true /* first_time_boot */);

        // 4MB chunk × 1 initial chunk fits inside the 256MB dev files even with multi-test buildup.
        co_await LogStoreManager::create(/*chunk_size=*/4 * 1024 * 1024, /*initial_num_chunks=*/1);
    }

    // Drive a full reload from the main test thread.  Cycling iomgr (kills reactor TLS, including
    // CPManager's cached ThreadStackInfo pointer that would otherwise dangle into the freed CPManager)
    // requires the main thread because stop_iomgr joins reactor threads — a reactor calling it would
    // self-join.  Two coroutine phases bracket the cycle.
    void reload_sync() {
        iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
            co_await log_store_mgr().shutdown();
            co_await cp_mgr().shutdown();
            co_await dm_->close_devices();
            Managers::reset();
        }());
        iomanager::stop_iomgr();
        iomanager::init_iomgr(2);
        iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
            dm_ = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
            co_await dm_->load_devices();
            co_await MetaBlkManager::load();
            auto cpmgr = CPManager::create();
            co_await cpmgr->start(false /* first_time_boot */);
            co_await LogStoreManager::load();
        }());
    }

    Async< void > shutdown() {
        co_await log_store_mgr().shutdown();
        co_await cp_mgr().shutdown();
        co_await dm_->close_devices();
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< DeviceManager > dm_;
};

// ── Append-mode basics ────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(LogStoreTest, SingleStoreAppendReadback) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    auto sid = raw->store_id();
    log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    constexpr uint32_t N = 32;
    constexpr size_t kRecordSize = 256;
    for (uint32_t i = 0; i < N; ++i) {
        auto blob = s.materialize(i, kRecordSize);
        auto lsn = s.store().quick_append(blob);
        EXPECT_EQ(lsn, to_i64(i));
    }
    co_await s.store().flush();

    for (uint32_t i = 0; i < N; ++i) {
        auto data = co_await s.store().read(to_i64(i));
        CO_ASSERT_EQ(data.size(), kRecordSize);
        EXPECT_TRUE(ShadowStore::verify_data(data.bytes(), data.size(), to_i64(i)));
    }

    co_await self.shutdown();
}

CORO_TEST_F(LogStoreTest, MultipleStoresOnOneStream) {
    co_await self.bootstrap();
    constexpr uint32_t kStores = 4;
    std::vector< std::unique_ptr< ShadowStore > > stores;
    for (uint32_t i = 0; i < kStores; ++i) {
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        auto sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        stores.push_back(std::make_unique< ShadowStore >(raw));
    }

    constexpr uint32_t kPerStore = 16;
    for (uint32_t i = 0; i < kPerStore; ++i) {
        for (auto& s : stores) {
            auto blob = s->materialize(i, 200);
            auto lsn = s->store().quick_append(blob);
            EXPECT_EQ(lsn, to_i64(i));
        }
    }
    for (auto& s : stores) {
        co_await s->store().flush();
    }

    // Each store's reads see only its own data.
    for (auto& s : stores) {
        for (uint32_t i = 0; i < kPerStore; ++i) {
            auto data = co_await s->store().read(to_i64(i));
            EXPECT_EQ(data.size(), 200u);
            EXPECT_TRUE(ShadowStore::verify_data(data.bytes(), data.size(), to_i64(i)));
        }
    }

    co_await self.shutdown();
}

CORO_TEST_F(LogStoreTest, ConcurrentAppends) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    log_store_mgr().open_log_store(raw->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    constexpr uint32_t kThreads = 8;
    constexpr uint32_t kPerThread = 200;
    constexpr uint32_t kTotal = kThreads * kPerThread;

    std::vector< std::thread > workers;
    workers.reserve(kThreads);
    for (uint32_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            for (uint32_t i = 0; i < kPerThread; ++i) {
                // We don't know what LSN we'll get; use a placeholder seed for the data and verify after.
                auto buf = std::make_shared< std::vector< uint8_t > >(128);
                std::memset(buf->data(), 0xAB, buf->size()); // placeholder; LSN-specific verify is overkill here
                sisl::IoBufSpan blob{buf->data(), to_u32(buf->size()), false};
                s.materialize(0, 0); // hold a slot in keep-alive (lifetime-only)
                std::lock_guard< std::mutex > _lk{global_keepalive_mtx_};
                global_keepalive_.push_back(buf);
                s.store().quick_append(blob);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    co_await s.store().flush();

    EXPECT_EQ(s.store().tail_lsn(), to_i64(kTotal - 1));
    co_await self.shutdown();
}

// ── Non-append-mode ──────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(LogStoreTest, OutOfOrderWritesThenTruncate) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/false);
    log_store_mgr().open_log_store(raw->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    // Issue LSNs 0..15 in shuffled order; verify reads see them all.
    constexpr uint32_t N = 16;
    std::vector< lsn_t > order;
    for (uint32_t i = 0; i < N; ++i) {
        order.push_back(to_i64(i));
    }
    std::shuffle(order.begin(), order.end(), std::mt19937{42});

    for (auto lsn : order) {
        auto blob = s.materialize(lsn, 128);
        s.store().quick_write(lsn, blob);
    }
    co_await s.store().flush();

    for (uint32_t i = 0; i < N; ++i) {
        auto data = co_await s.store().read(to_i64(i));
        EXPECT_EQ(data.size(), 128u);
        EXPECT_TRUE(ShadowStore::verify_data(data.bytes(), data.size(), to_i64(i)));
    }

    // Truncate to lsn 7; reads of 0..7 return empty, 8..15 still work.
    co_await s.store().truncate(7);
    for (uint32_t i = 0; i <= 7; ++i) {
        auto data = co_await s.store().read(to_i64(i));
        EXPECT_EQ(data.size(), 0u);
    }
    for (uint32_t i = 8; i < N; ++i) {
        auto data = co_await s.store().read(to_i64(i));
        EXPECT_EQ(data.size(), 128u);
    }

    co_await self.shutdown();
}

CORO_TEST_F(LogStoreTest, WriteWithHolesAndFillGap) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/false);
    log_store_mgr().open_log_store(raw->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    // Write LSNs 0,1,2,4,5,7 (holes at 3,6).
    for (lsn_t lsn : {0, 1, 2, 4, 5, 7}) {
        auto blob = s.materialize(lsn, 64);
        s.store().quick_write(lsn, blob);
    }
    co_await s.store().flush();

    // Plug the holes so truncate can advance past them.
    s.store().fill_gap(3);
    s.store().fill_gap(6);

    co_await s.store().truncate(7);
    EXPECT_EQ(s.store().head_lsn(), 8);

    co_await self.shutdown();
}

// ── Cross-store truncate (the central LogStore-over-LogStream invariant) ────────────────────────────────────────

CORO_TEST_F(LogStoreTest, GlobalTruncateMinAcrossStores) {
    co_await self.bootstrap();

    // Two stores sharing one stream; alternating flushes so each store's records land in distinct LogGroups at
    // distinct stream offsets.  After this setup:
    //   stream:  [group_a0][group_b0][group_a1][group_b1]
    //   off:     0          off_b0    off_a1    off_b1
    auto raw_a = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    auto raw_b = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    log_store_mgr().open_log_store(raw_a->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    log_store_mgr().open_log_store(raw_b->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore a{raw_a};
    ShadowStore b{raw_b};

    auto append_batch = [](ShadowStore& s, uint32_t n) -> Async< void > {
        for (uint32_t i = 0; i < n; ++i) {
            auto blob = s.materialize(s.store().tail_lsn() + 1, 128);
            s.store().quick_append(blob);
        }
        co_await s.store().flush();
    };

    co_await append_batch(a, 5); // group_a0
    co_await append_batch(b, 5); // group_b0
    co_await append_batch(a, 5); // group_a1
    co_await append_batch(b, 5); // group_b1

    // A's first record is in group_a0 at stream offset 0; B's first record is in group_b0 just after.
    const uint64_t a_initial_anchor = a.store().min_trunc_stream_offset().value();
    const uint64_t b_initial_anchor = b.store().min_trunc_stream_offset().value();
    EXPECT_EQ(a_initial_anchor, 0u) << "A's first record is in group_a0 at stream offset 0";
    EXPECT_GT(b_initial_anchor, a_initial_anchor) << "B's first record (group_b0) is past A's group_a0";

    // Truncate A to LSN 4 — A's surviving anchor is now group_a1's offset (somewhere past group_b0).
    co_await a.store().truncate(4);
    const uint64_t a_anchor = a.store().min_trunc_stream_offset().value();
    EXPECT_GT(a_anchor, 0u) << "after truncating A's first batch, A's anchor moved past offset 0";

    // B is untouched — its anchor is still at group_b0 (between A's two groups, so smaller than A's anchor).
    const uint64_t b_anchor = b.store().min_trunc_stream_offset().value();
    EXPECT_LT(b_anchor, a_anchor) << "B's anchor is earlier than A's";

    // global_truncate uses min(a_anchor, b_anchor) = b_anchor.  Stream head must NOT advance past A's anchor —
    // doing so would discard A's surviving records.
    co_await log_store_mgr().truncate();
    EXPECT_EQ(log_store_mgr().log_stream()->head_offset(), b_anchor)
        << "global_truncate should advance stream head to min(A's anchor, B's anchor) = B's anchor";

    // Now truncate B past A's anchor.  global_truncate should now use A's anchor.
    co_await b.store().truncate(4);
    const uint64_t b_anchor_2 = b.store().min_trunc_stream_offset().value();
    EXPECT_GT(b_anchor_2, a_anchor) << "B's new anchor is past A's";
    co_await log_store_mgr().truncate();
    EXPECT_EQ(log_store_mgr().log_stream()->head_offset(), a_anchor)
        << "after B truncated past A, stream head advances only to A's anchor";

    co_await self.shutdown();
}

TEST_F(LogStoreTest, GlobalTruncateAcrossRestart) {
    logstore_id_t sid_a{}, sid_b{};
    uint64_t pre_restart_head{0};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid_a, &sid_b, &pre_restart_head]() -> Async< void > {
        co_await bootstrap();
        auto raw_a = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        auto raw_b = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid_a = raw_a->store_id();
        sid_b = raw_b->store_id();
        log_store_mgr().open_log_store(sid_a, [](lsn_t, const sisl::IoBufView&) {});
        log_store_mgr().open_log_store(sid_b, [](lsn_t, const sisl::IoBufView&) {});
        {
            ShadowStore a{raw_a};
            ShadowStore b{raw_b};
            for (uint32_t i = 0; i < 5; ++i) {
                a.store().quick_append(a.materialize(i, 128));
            }
            co_await a.store().flush();
            for (uint32_t i = 0; i < 5; ++i) {
                b.store().quick_append(b.materialize(i, 128));
            }
            co_await b.store().flush();
            for (uint32_t i = 5; i < 10; ++i) {
                a.store().quick_append(a.materialize(i, 128));
            }
            co_await a.store().flush();
            co_await a.store().truncate(4);
            co_await log_store_mgr().truncate();
        }
        pre_restart_head = log_store_mgr().log_stream()->head_offset();
        EXPECT_GT(pre_restart_head, 0u) << "stream head advanced after global_truncate";
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid_a, sid_b, pre_restart_head]() -> Async< void > {
        EXPECT_EQ(log_store_mgr().log_stream()->head_offset(), pre_restart_head)
            << "stream head_offset persisted across restart";
        auto recov_a = log_store_mgr().get_log_store(sid_a);
        auto recov_b = log_store_mgr().get_log_store(sid_b);
        CO_ASSERT_NE(recov_a, nullptr);
        CO_ASSERT_NE(recov_b, nullptr);
        ShadowStore a2{recov_a};
        ShadowStore b2{recov_b};
        co_await log_store_mgr().recover();
        EXPECT_EQ(a2.replayed_count(), 5u) << "A's surviving records replay";
        EXPECT_EQ(b2.replayed_count(), 5u) << "B's records replay";
        co_await shutdown();
    }());
}

CORO_TEST_F(LogStoreTest, GlobalTruncateNoOpWhenStoreEmpty) {
    co_await self.bootstrap();
    auto raw_a = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    auto raw_b = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    log_store_mgr().open_log_store(raw_a->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    log_store_mgr().open_log_store(raw_b->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore a{raw_a};
    ShadowStore b{raw_b};

    // Only A has data; B is created but never written to.  B's min_trunc_stream_offset() is std::nullopt and
    // must NOT constrain the global min (an empty store would otherwise pin the stream head at 0 forever).
    for (uint32_t i = 0; i < 10; ++i) {
        a.store().quick_append(a.materialize(i, 128));
    }
    co_await a.store().flush();
    for (uint32_t i = 5; i < 10; ++i) {
        a.store().quick_append(a.materialize(i, 128));
    }
    co_await a.store().flush();

    EXPECT_FALSE(b.store().min_trunc_stream_offset().has_value())
        << "empty store contributes no anchor (must be nullopt, not 0)";

    // Truncate A past the first batch; A's anchor is now past offset 0.
    co_await a.store().truncate(9);
    const uint64_t a_anchor = a.store().min_trunc_stream_offset().value();
    EXPECT_GT(a_anchor, 0u);

    // global_truncate should use A's anchor only — B being empty must not pin the stream head at 0.
    co_await log_store_mgr().truncate();
    EXPECT_EQ(log_store_mgr().log_stream()->head_offset(), a_anchor)
        << "global_truncate uses A's anchor; B being empty doesn't constrain the min";

    co_await self.shutdown();
}

// ── Truncate + restart ──────────────────────────────────────────────────────────────────────────────────────────

TEST_F(LogStoreTest, TruncatePartialAcrossRestart) {
    logstore_id_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        {
            ShadowStore s{raw};
            constexpr uint32_t N = 12;
            for (uint32_t i = 0; i < N; ++i) {
                auto blob = s.materialize(i, 128);
                s.store().quick_append(blob);
            }
            co_await s.store().flush();
            co_await s.store().truncate(5); // keep lsns 6..11
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        auto recovered_raw = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(recovered_raw, nullptr);
        ShadowStore s2{recovered_raw};
        co_await log_store_mgr().recover();

        auto rec = s2.replayed();
        EXPECT_EQ(rec.size(), 6u);
        for (size_t i = 0; i < rec.size(); ++i) {
            EXPECT_EQ(rec[i].first, to_i64(6 + i));
            EXPECT_TRUE(ShadowStore::verify_data(rec[i].second.data(), rec[i].second.size(), rec[i].first));
        }
        co_await shutdown();
    }());
}

TEST_F(LogStoreTest, TruncateAllAcrossRestart) {
    logstore_id_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        {
            ShadowStore s{raw};
            for (uint32_t i = 0; i < 4; ++i) {
                auto blob = s.materialize(i, 128);
                s.store().quick_append(blob);
            }
            co_await s.store().flush();
            co_await s.store().truncate(3); // truncate everything
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        auto recovered_raw = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(recovered_raw, nullptr);
        ShadowStore s2{recovered_raw};
        co_await log_store_mgr().recover();
        EXPECT_EQ(s2.replayed_count(), 0u);
        EXPECT_EQ(recovered_raw->head_lsn(), 4);
        co_await shutdown();
    }());
}

// ── Restart cycles ───────────────────────────────────────────────────────────────────────────────────────────────

TEST_F(LogStoreTest, BasicReplayPreservesOrder) {
    logstore_id_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        {
            ShadowStore s{raw};
            for (uint32_t i = 0; i < 16; ++i) {
                auto blob = s.materialize(i, 200);
                s.store().quick_append(blob);
            }
            co_await s.store().flush();
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        auto recovered_raw = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(recovered_raw, nullptr);
        ShadowStore s2{recovered_raw};
        co_await log_store_mgr().recover();
        auto rec = s2.replayed();
        CO_ASSERT_EQ(rec.size(), 16u);
        for (size_t i = 0; i < rec.size(); ++i) {
            EXPECT_EQ(rec[i].first, to_i64(i));
        }
        co_await shutdown();
    }());
}

TEST_F(LogStoreTest, AppendRestartAppendRestart) {
    logstore_id_t sid{};

    // Cycle 1: bootstrap, append 8, flush.
    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        ShadowStore s{raw};
        for (uint32_t i = 0; i < 8; ++i) {
            auto blob = s.materialize(i, 128);
            s.store().quick_append(blob);
        }
        co_await s.store().flush();
    }());

    reload_sync();

    // Cycle 2: recover, verify 8 replayed, append 8 more (LSNs 8..15), flush.
    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        auto raw2 = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(raw2, nullptr);
        ShadowStore s2{raw2};
        co_await log_store_mgr().recover();
        EXPECT_EQ(s2.replayed_count(), 8u);
        for (uint32_t i = 0; i < 8; ++i) {
            const lsn_t expected_lsn = 8 + i;
            auto blob = s2.materialize(expected_lsn, 128);
            auto lsn = s2.store().quick_append(blob);
            EXPECT_EQ(lsn, expected_lsn);
        }
        co_await s2.store().flush();
    }());

    reload_sync();

    // Final: recover, verify all 16 replay in order.
    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        auto raw3 = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(raw3, nullptr);
        ShadowStore s3{raw3};
        co_await log_store_mgr().recover();
        auto rec = s3.replayed();
        CO_ASSERT_EQ(rec.size(), 16u);
        for (size_t i = 0; i < rec.size(); ++i) {
            EXPECT_EQ(rec[i].first, to_i64(i));
            EXPECT_TRUE(ShadowStore::verify_data(rec[i].second.data(), rec[i].second.size(), rec[i].first));
        }
        co_await shutdown();
    }());
}

// ── Rollback ────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(LogStoreTest, RollbackBasic) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    log_store_mgr().open_log_store(raw->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    for (uint32_t i = 0; i < 10; ++i) {
        auto blob = s.materialize(i, 64);
        s.store().quick_append(blob);
    }
    co_await s.store().flush();
    EXPECT_EQ(s.store().tail_lsn(), 9);

    // Rollback to lsn 4 — keep 0..4, drop 5..9.
    auto ok = co_await s.store().rollback(4);
    EXPECT_TRUE(ok);
    EXPECT_EQ(s.store().tail_lsn(), 4);

    // Reads of lsn > 4 return empty.
    for (lsn_t lsn = 5; lsn < 10; ++lsn) {
        auto data = co_await s.store().read(lsn);
        EXPECT_EQ(data.size(), 0u);
    }
    // Reads <= 4 still work.
    for (lsn_t lsn = 0; lsn <= 4; ++lsn) {
        auto data = co_await s.store().read(lsn);
        EXPECT_EQ(data.size(), 64u);
    }

    co_await self.shutdown();
}

TEST_F(LogStoreTest, RollbackPersistsAcrossRestart) {
    logstore_id_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        ShadowStore s{raw};
        for (uint32_t i = 0; i < 10; ++i) {
            auto blob = s.materialize(i, 64);
            s.store().quick_append(blob);
        }
        co_await s.store().flush();
        EXPECT_TRUE(co_await s.store().rollback(4));
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        auto raw2 = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(raw2, nullptr);
        ShadowStore s2{raw2};
        co_await log_store_mgr().recover();
        // Replay sees only 0..4; rolled-back range 5..9 is skipped.
        auto rec = s2.replayed();
        CO_ASSERT_EQ(rec.size(), 5u);
        for (size_t i = 0; i < rec.size(); ++i) {
            EXPECT_EQ(rec[i].first, to_i64(i));
        }
        co_await shutdown();
    }());
}

// Pattern: append batch, rollback last half, restart, verify replay matches kept prefix.  Each "cycle" =
// one append+rollback batch.  With N_CYCLES=2 we unroll the loop into explicit phases so the iomgr cycle
// happens between them on the main thread (Shape A).  expected_max_alive_lsn is carried across phases.
TEST_F(LogStoreTest, RollbackAppendRestartCycle) {
    constexpr uint32_t kBatchSize = 8;
    logstore_id_t sid{};
    lsn_t expected_max_alive_lsn = -1;

    // Cycle 0: bootstrap, append batch, rollback half.
    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid, &expected_max_alive_lsn]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        ShadowStore s{raw};
        const lsn_t batch_start = s.store().tail_lsn() + 1;
        for (uint32_t i = 0; i < kBatchSize; ++i) {
            const lsn_t lsn = batch_start + i;
            auto blob = s.materialize(lsn, 128);
            auto got = s.store().quick_append(blob);
            EXPECT_EQ(got, lsn);
        }
        co_await s.store().flush();
        const lsn_t rollback_to = batch_start + (kBatchSize / 2) - 1;
        EXPECT_TRUE(co_await s.store().rollback(rollback_to));
        EXPECT_EQ(s.store().tail_lsn(), rollback_to);
        expected_max_alive_lsn = rollback_to;
    }());

    reload_sync();

    // Cycle 1: recover, verify carryover, append batch, rollback half.
    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, &expected_max_alive_lsn]() -> Async< void > {
        auto raw = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(raw, nullptr);
        ShadowStore s{raw};
        co_await log_store_mgr().recover();
        EXPECT_EQ(s.replayed_count(), to_size(expected_max_alive_lsn + 1)) << "cycle=1 replay count mismatch";
        s.reset_replay();
        const lsn_t batch_start = s.store().tail_lsn() + 1;
        for (uint32_t i = 0; i < kBatchSize; ++i) {
            const lsn_t lsn = batch_start + i;
            auto blob = s.materialize(lsn, 128);
            auto got = s.store().quick_append(blob);
            EXPECT_EQ(got, lsn);
        }
        co_await s.store().flush();
        const lsn_t rollback_to = batch_start + (kBatchSize / 2) - 1;
        EXPECT_TRUE(co_await s.store().rollback(rollback_to));
        EXPECT_EQ(s.store().tail_lsn(), rollback_to);
        expected_max_alive_lsn = rollback_to;
    }());

    reload_sync();

    // Final: recover, verify replay order against kept prefix.
    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, expected_max_alive_lsn]() -> Async< void > {
        auto final_raw = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(final_raw, nullptr);
        ShadowStore final_s{final_raw};
        co_await log_store_mgr().recover();
        auto rec = final_s.replayed();
        CO_ASSERT_EQ(rec.size(), to_size(expected_max_alive_lsn + 1));
        for (size_t i = 0; i < rec.size(); ++i) {
            EXPECT_EQ(rec[i].first, to_i64(i)) << "final replay order mismatch at index " << i;
        }
        co_await shutdown();
    }());
}

// Same intent as RollbackAppendRestartCycle but without reboots between cycles — the in-memory chain stays
// intact across cycles, and only the final reload validates persistence.  All cycles run in a single
// spawn_and_block phase since there's no reload between them.
TEST_F(LogStoreTest, RollbackAppendNoRestartCycle) {
    constexpr uint32_t N_CYCLES = 2;
    constexpr uint32_t kBatchSize = 8;
    logstore_id_t sid{};
    lsn_t expected_max_alive_lsn = -1;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid, &expected_max_alive_lsn]() -> Async< void > {
        co_await bootstrap();
        auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
        sid = raw->store_id();
        log_store_mgr().open_log_store(sid, [](lsn_t, const sisl::IoBufView&) {});
        ShadowStore s{raw};
        for (uint32_t cycle = 0; cycle < N_CYCLES; ++cycle) {
            const lsn_t batch_start = s.store().tail_lsn() + 1;
            for (uint32_t i = 0; i < kBatchSize; ++i) {
                const lsn_t lsn = batch_start + i;
                auto blob = s.materialize(lsn, 128);
                auto got = s.store().quick_append(blob);
                EXPECT_EQ(got, lsn) << "cycle=" << cycle;
            }
            co_await s.store().flush();
            const lsn_t rollback_to = batch_start + (kBatchSize / 2) - 1;
            EXPECT_TRUE(co_await s.store().rollback(rollback_to));
            EXPECT_EQ(s.store().tail_lsn(), rollback_to);
            expected_max_alive_lsn = rollback_to;
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, expected_max_alive_lsn]() -> Async< void > {
        auto final_raw = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(final_raw, nullptr);
        ShadowStore final_s{final_raw};
        co_await log_store_mgr().recover();
        auto rec = final_s.replayed();
        CO_ASSERT_EQ(rec.size(), to_size(expected_max_alive_lsn + 1));
        for (size_t i = 0; i < rec.size(); ++i) {
            EXPECT_EQ(rec[i].first, to_i64(i)) << "final replay order mismatch at index " << i;
        }
        co_await shutdown();
    }());
}

// ── Sync API ────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(LogStoreTest, WriteAndFlushReturnsDurable) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/false);
    log_store_mgr().open_log_store(raw->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    auto blob = s.materialize(/*lsn=*/0, 64);
    co_await s.store().write_and_flush(0, blob);

    // Immediately readable after write_and_flush returns.
    auto data = co_await s.store().read(0);
    EXPECT_EQ(data.size(), 64u);
    EXPECT_TRUE(ShadowStore::verify_data(data.bytes(), data.size(), 0));

    co_await self.shutdown();
}

CORO_TEST_F(LogStoreTest, ReadOutOfRangeReturnsEmpty) {
    co_await self.bootstrap();
    auto raw = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    log_store_mgr().open_log_store(raw->store_id(), [](lsn_t, const sisl::IoBufView&) {});
    ShadowStore s{raw};

    auto blob = s.materialize(/*lsn=*/0, 64);
    s.store().quick_append(blob);
    co_await s.store().flush();

    // lsn 100 is way past tail.
    auto data = co_await s.store().read(100);
    EXPECT_EQ(data.size(), 0u);

    co_await self.shutdown();
}

// ── Test main ─────────────────────────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_log_store");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown.
    return RUN_ALL_TESTS();
}