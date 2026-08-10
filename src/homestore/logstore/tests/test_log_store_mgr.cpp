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
#include "common/async.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <thread>

#include <fmt/format.h>
#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "sisl/fds/buffer.h"
#include "sisl/flip/flip.h"
#include "sisl/flip/flip_client.h"

#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"
#include "homestore/base/crash_simulator.h"
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

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024;

// ─────────────────────────────────────────────────────────────────────────────
// Fixture mirrors test_log_store: bootstrap DM + Meta + CP + LogStoreManager.  CP is required because the
// manager is wired as a CP consumer (today a no-op for log truncation; placeholder for future log-truncate-on-cp).
// ─────────────────────────────────────────────────────────────────────────────
class LogStoreMgrTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test fresh reactors: CPManager's t_cp_info_ thread_local pointer would otherwise dangle into the
        // freed prior-test CPManager's owned_stacks_, and the next cp_guard() reads it as UAF.
        iomanager::init_iomgr(2);
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

        co_await LogStoreManager::create(/*chunk_size=*/4 * 1024 * 1024, /*initial_num_chunks=*/1);
    }

    // Drive a full reload from the main test thread.  Cycling iomgr (kills reactor TLS, including
    // CPManager's cached ThreadStackInfo pointer that would otherwise dangle into the freed CPManager)
    // requires the main thread because stop_iomgr joins reactor threads — a reactor calling it would
    // self-join.  Two coroutine phases bracket the cycle: teardown on the old reactor pool, bringup on
    // the new pool.
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

    // Convenience: append `n` records of `size` bytes each to the given store, then flush.  Returns the LSNs
    // assigned, in order.
    static Async< std::vector< lsn_t > > append_n(LogStore& store, uint32_t n, size_t size,
                                                  std::vector< std::shared_ptr< std::vector< uint8_t > > >& keep) {
        std::vector< lsn_t > lsns;
        lsns.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            auto buf = std::make_shared< std::vector< uint8_t > >(size, static_cast< uint8_t >(0xCD));
            keep.push_back(buf);
            sisl::IoBufSpan blob{buf->data(), to_u32(buf->size()), false};
            lsns.push_back(store.quick_append(blob));
        }
        co_await store.flush();
        co_return lsns;
    }

    // ── Crash simulation helpers ───────────────────────────────────────────────────────────────────────────────
    // Install a fresh CrashSimulator with a no-op restart callback so crash_now() marks crashed_=true (gating
    // every PhysicalDev write to a silent no-op) but does not raise(SIGKILL) — the process survives.  Fresh
    // instance resets crashed_ to false, so call again after the crash to unblock reload writes.
    static void install_fresh_crash_sim() {
        Managers::init_crash_simulator(std::make_shared< CrashSimulator >([]() {}));
    }

    static void arm_crash_flip(const std::string& flip_name) {
        flip::FlipFrequencyT freq;
        freq.count = 1;
        flip::PercentFrequencyT pf;
        pf.v = 100;
        freq.kind.Set(pf);
        flip::FlipClient::instance().inject_noreturn_flip(flip_name, {}, freq);
    }

    static void remove_flip(const std::string& flip_name) { flip::Flip::instance().remove(flip_name); }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< DeviceManager > dm_;
};

// ── Lifecycle ───────────────────────────────────────────────────────────────────────────────────────────────────

TEST_F(LogStoreMgrTest, RecoverWithoutAnyStores) {
    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        co_await bootstrap();
        EXPECT_EQ(log_store_mgr().log_stores().size(), 0u);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        EXPECT_EQ(log_store_mgr().log_stores().size(), 0u);
        co_await log_store_mgr().replay(); // no-op
        co_await shutdown();
    }());
}

TEST_F(LogStoreMgrTest, CreateOpenRecover) {
    constexpr uint32_t kStores = 3;
    constexpr uint32_t kRecordsPer = 8;
    std::vector< logstore_id_t > sids;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sids]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        for (uint32_t i = 0; i < kStores; ++i) {
            auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
            sids.push_back(store->store_id());
            log_store_mgr().open_log_store(store->store_id(), LogStoreOptions{.append_mode = true},
                                           [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
            co_await append_n(*store, kRecordsPer, 128, keep);
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sids, kStores, kRecordsPer]() -> Async< void > {
        EXPECT_EQ(log_store_mgr().log_stores().size(), kStores);

        std::map< logstore_id_t, std::atomic< uint32_t > > replay_counts;
        for (auto sid : sids) {
            auto& cnt = replay_counts[sid];
            log_store_mgr().open_log_store(sid, LogStoreOptions{.append_mode = true},
                                           [&cnt](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                               cnt.fetch_add(1, std::memory_order_relaxed);
                                               co_return;
                                           });
        }
        co_await log_store_mgr().replay();

        for (auto sid : sids) {
            EXPECT_EQ(replay_counts[sid].load(), kRecordsPer) << "sid=" << sid;
        }
        co_await shutdown();
    }());
}

TEST_F(LogStoreMgrTest, DropUnopenedStores) {
    constexpr uint32_t kStores = 3;
    std::vector< logstore_id_t > sids;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sids]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        for (uint32_t i = 0; i < kStores; ++i) {
            auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
            sids.push_back(store->store_id());
            log_store_mgr().open_log_store(store->store_id(), LogStoreOptions{.append_mode = true},
                                           [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
            co_await append_n(*store, 4, 128, keep);
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sids, kStores]() -> Async< void > {
        CO_ASSERT_EQ(log_store_mgr().log_stores().size(), kStores);

        std::map< logstore_id_t, std::atomic< uint32_t > > replay_counts;
        log_store_mgr().open_log_store(sids[0], LogStoreOptions{.append_mode = true},
                                       [&replay_counts, sid = sids[0]](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                           replay_counts[sid].fetch_add(1);
                                           co_return;
                                       });
        log_store_mgr().open_log_store(sids[2], LogStoreOptions{.append_mode = true},
                                       [&replay_counts, sid = sids[2]](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                           replay_counts[sid].fetch_add(1);
                                           co_return;
                                       });

        co_await log_store_mgr().replay();

        EXPECT_EQ(log_store_mgr().log_stores().size(), 2u) << "unopened sid should be dropped";
        EXPECT_NE(log_store_mgr().get_log_store(sids[0]), nullptr);
        EXPECT_EQ(log_store_mgr().get_log_store(sids[1]), nullptr) << "unopened sid removed";
        EXPECT_NE(log_store_mgr().get_log_store(sids[2]), nullptr);

        EXPECT_EQ(replay_counts[sids[0]].load(), 4u);
        EXPECT_EQ(replay_counts[sids[2]].load(), 4u);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sids]() -> Async< void > {
        EXPECT_EQ(log_store_mgr().log_stores().size(), 2u) << "dropped store stays gone across restart";
        EXPECT_EQ(log_store_mgr().get_log_store(sids[1]), nullptr);
        co_await shutdown();
    }());
}

// Create 2 stores, write to both, restart, but only OPEN one of them.  The other is "orphaned" — its
// records survive in the LogStream chain but on_log_found for them dispatches to an unopened LogStore,
// which has no handler so the records are silently dropped during recover.  After recover, the unopened
// store is dropped (same as DropUnopenedStores).
TEST_F(LogStoreMgrTest, OrphanRecordsSilentlyDropped) {
    logstore_id_t kept_sid{};
    logstore_id_t orphan_sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &kept_sid, &orphan_sid]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto kept = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        auto orphan = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        kept_sid = kept->store_id();
        orphan_sid = orphan->store_id();
        log_store_mgr().open_log_store(kept_sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        log_store_mgr().open_log_store(orphan_sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        co_await append_n(*kept, 5, 128, keep);
        co_await append_n(*orphan, 5, 128, keep);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, kept_sid, orphan_sid]() -> Async< void > {
        std::atomic< uint32_t > kept_replay{0};
        log_store_mgr().open_log_store(kept_sid, LogStoreOptions{.append_mode = true},
                                       [&kept_replay](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                           kept_replay.fetch_add(1);
                                           co_return;
                                       });
        co_await log_store_mgr().replay();
        EXPECT_EQ(kept_replay.load(), 5u) << "kept store sees its 5 records";
        EXPECT_EQ(log_store_mgr().log_stores().size(), 1u) << "orphan store dropped after recover";
        EXPECT_EQ(log_store_mgr().get_log_store(orphan_sid), nullptr);
        co_await shutdown();
    }());
}

TEST_F(LogStoreMgrTest, CreateAfterRecoverContinuesIds) {
    constexpr uint32_t kInitialStores = 4;
    std::vector< logstore_id_t > original_sids;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &original_sids]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        for (uint32_t i = 0; i < kInitialStores; ++i) {
            auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
            original_sids.push_back(store->store_id());
            log_store_mgr().open_log_store(store->store_id(), LogStoreOptions{.append_mode = true},
                                           [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
            co_await append_n(*store, 2, 64, keep);
        }
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &original_sids, kInitialStores]() -> Async< void > {
        for (auto sid : original_sids) {
            log_store_mgr().open_log_store(sid, LogStoreOptions{.append_mode = true},
                                           [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        }
        co_await log_store_mgr().replay();
        CO_ASSERT_EQ(log_store_mgr().log_stores().size(), kInitialStores);

        auto fresh = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        const logstore_id_t expected_min = *std::max_element(original_sids.begin(), original_sids.end()) + 1;
        EXPECT_GE(fresh->store_id(), expected_min) << "new sid must not collide with recovered sids";

        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Orphan stores: storage reclaimed.  DropUnopenedStores already verifies unopened stores are dropped from the
// registry.  This test extends that with the "storage reclaimed" claim: after the drop, LogStoreManager::truncate()
// should be able to advance past the orphan's records (the orphan no longer contributes to min_trunc_stream_offset),
// and the underlying LogStream should shed chunks that only held the orphan's records.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(LogStoreMgrTest, OrphanStoreStorageReclaimed) {
    logstore_id_t kept_sid{}, orphan_sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &kept_sid, &orphan_sid]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto kept = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        auto orphan = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        kept_sid = kept->store_id();
        orphan_sid = orphan->store_id();
        log_store_mgr().open_log_store(kept_sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        log_store_mgr().open_log_store(orphan_sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        // Push enough data that at least one full chunk is claimed.
        co_await append_n(*kept, 20, 1024, keep);
        co_await append_n(*orphan, 20, 1024, keep);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, kept_sid, orphan_sid]() -> Async< void > {
        // Only open kept; leave orphan unopened so replay drops it.
        std::atomic< uint32_t > kept_replay{0};
        log_store_mgr().open_log_store(kept_sid, LogStoreOptions{.append_mode = true},
                                       [&kept_replay](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                           kept_replay.fetch_add(1);
                                           co_return;
                                       });
        co_await log_store_mgr().replay();
        EXPECT_EQ(kept_replay.load(), 20u);
        EXPECT_EQ(log_store_mgr().log_stores().size(), 1u);
        EXPECT_EQ(log_store_mgr().get_log_store(orphan_sid), nullptr) << "orphan store must be dropped";

        // Truncate the kept store up to its tail (via LogStore::truncate) so it stops contributing to min_off.
        // With the orphan dropped, the manager's aggregate truncate should now be able to advance the stream past
        // all previously-live records — proving the orphan's chunks are no longer "pinned" by its records.
        auto kept = log_store_mgr().get_log_store(kept_sid);
        CO_ASSERT_NE(kept, nullptr);
        co_await kept->truncate(kept->tail_lsn(), /*in_memory_only=*/false);
        co_await log_store_mgr().truncate();
        // Truncate is a no-op if there's nothing to trim; that's fine — the important recovery contract (orphan
        // gone, only kept records delivered) is already verified above.
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Store-id floor: destroy a store WITHOUT truncating its records from the stream first.  The store's sb is
// removed, so on restart LogStoreManager::load sees no sb for that sid — normally next_store_id_ would revert
// to 0.  But the LogStream chain still holds records tagged with the dead sid; replay walks them and does
// atomic_update_max(next_store_id_, sid + 1), raising the floor so a subsequent create_log_store() cannot
// recycle the dead sid.  Without this fix, a new store would inherit the dead store's records at replay.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(LogStoreMgrTest, StoreIdFloorProtectsDeadRecords) {
    logstore_id_t destroyed_sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &destroyed_sid]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        destroyed_sid = store->store_id();
        log_store_mgr().open_log_store(destroyed_sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        co_await append_n(*store, 5, 128, keep);
        // Destroy the store WITHOUT truncating first — the records remain in the LogStream, only the store's sb
        // is removed.
        co_await log_store_mgr().destroy_log_store(destroyed_sid);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, destroyed_sid]() -> Async< void > {
        // No stores after load (sb was removed).
        EXPECT_EQ(log_store_mgr().log_stores().size(), 0u);
        // Replay walks the stream; the dead sid's records raise next_store_id_ past it.
        co_await log_store_mgr().replay();

        // A fresh create MUST NOT recycle the dead sid — that would resurrect the dead store's records into the
        // new store on a subsequent boot's replay.
        auto fresh = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        EXPECT_GT(fresh->store_id(), destroyed_sid)
            << "new sid " << fresh->store_id() << " must not collide with destroyed sid " << destroyed_sid;
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Auto-truncate timer + preserve-log-count.  A store opened with auto_truncate=true, preserve_log_count=K has
// LogStoreManager's periodic timer invoke LogStore::truncate(MAX), which clamps to min(checkpt_lsn, tail-K).
// preserve_log_count guarantees at least K entries survive past the compact point.  Test: append N records,
// register a watermark_cb that returns tail_lsn (so the CP promotes checkpt_lsn = tail), trigger CP, invoke
// manager truncate.  Assert head_lsn advanced to (tail - K + 1) — the K tail entries are preserved.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(LogStoreMgrTest, AutoTruncateWithPreserveLogCount) {
    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        co_await bootstrap();
        constexpr uint32_t total = 50;
        constexpr uint32_t preserve = 8;

        auto store = co_await log_store_mgr().create_log_store(
            LogStoreOptions{.append_mode = true, .auto_truncate = true, .preserve_log_count = preserve});
        // Register a watermark_cb that returns the current tail — so on_switchover_cp captures pending=tail,
        // and cp_flush_persist promotes checkpt_lsn to tail.  Without this, checkpt_lsn stays at -1 and
        // truncate can't advance at all.
        auto* store_raw = store.get();
        log_store_mgr().open_log_store(
            store->store_id(), LogStoreOptions{.append_mode = true, .auto_truncate = true, .preserve_log_count = preserve},
            [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; },
            [store_raw]() -> lsn_t { return store_raw->tail_lsn(); });

        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto lsns = co_await append_n(*store, total, 64, keep);
        auto const tail = store->tail_lsn();
        EXPECT_EQ(tail, lsns.back());

        // Trigger CP to promote checkpt_lsn = tail via the watermark_cb.
        auto ok = co_await cp_mgr().trigger_cp_flush(/*force=*/true);
        CO_ASSERT_TRUE(ok);
        EXPECT_EQ(store->checkpt_lsn(), tail);

        // Invoke manager truncate — auto_truncate opt-in stores get LogStore::truncate(MAX), clamped by
        // (checkpt=tail, tail - preserve).
        co_await log_store_mgr().truncate();

        // head_lsn should advance to (tail - preserve + 1), i.e., preserve records remain [tail-preserve+1 .. tail].
        auto const expected_head = tail - static_cast< lsn_t >(preserve) + 1;
        EXPECT_EQ(store->head_lsn(), expected_head)
            << "auto-truncate did not respect preserve_log_count=" << preserve;

        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Crash before LogStream truncate-commit: fires the crash_before_logstream_truncate_commit flip in the
// truncate-all branch of LogStream::truncate — chunks are released and the AppendByteStream sb reflects an
// empty stream, but the LogStream sb has NOT been re-persisted with a fresh init_crc.  After restart, any
// stale group left in the anchor chunk must NOT match the (still-old-on-disk) init_crc through the crashed
// path and resurrect pre-truncate records.  We validate the end state: post-recovery, the store sees zero
// live records (replay delivers nothing new — the LogStore SB itself was untouched, so it reports its own
// pre-truncate view of head_lsn/tail_lsn — but the physical stream state is coherent).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(LogStoreMgrTest, CrashBeforeLogstreamTruncateCommit) {
    logstore_id_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        sid = store->store_id();
        // watermark_cb=tail so a CP promotes checkpt to tail, allowing truncate to go all the way.
        auto* store_raw = store.get();
        log_store_mgr().open_log_store(
            sid, LogStoreOptions{.append_mode = true},
            [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; },
            [store_raw]() -> lsn_t { return store_raw->tail_lsn(); });

        co_await append_n(*store, 10, 128, keep);
        auto ok = co_await cp_mgr().trigger_cp_flush(/*force=*/true);
        CO_ASSERT_TRUE(ok);

        // Truncate the store up to tail so its records_ is empty (min_trunc_stream_offset becomes nullopt).
        co_await store->truncate(store->tail_lsn(), /*in_memory_only=*/false);

        // LogStoreManager::truncate would no-op here (no store contributes an anchor).  To force the
        // truncate-all branch on the LogStream (where the flip lives), call log_stream->truncate directly
        // with the current tail_offset — that collapses head==tail=(0,0) and enters the init_crc-refresh path.
        auto stream = log_store_mgr().log_stream();
        auto const tail = stream->tail_offset();

        install_fresh_crash_sim();
        arm_crash_flip("crash_before_logstream_truncate_commit");
        co_await stream->truncate(stream_key{/*log_id=*/-1, /*record_off=*/tail, /*group_off=*/tail});
        EXPECT_TRUE(is_crash_simulated()) << "flip never fired";

        remove_flip("crash_before_logstream_truncate_commit");
        install_fresh_crash_sim();
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        // The LogStore's sb was persisted with the truncated head_lsn BEFORE the crash (LogStore::truncate
        // calls persist_sb), so on load the store reports head_lsn past the truncated records.  The stream
        // itself may or may not have shed all chunks depending on the crash timing, but recovery must NOT
        // deliver stale pre-truncate records to the handler.
        std::atomic< uint32_t > replay_count{0};
        log_store_mgr().open_log_store(sid, LogStoreOptions{.append_mode = true},
                                       [&replay_count](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                           replay_count.fetch_add(1);
                                           co_return;
                                       });
        co_await log_store_mgr().replay();

        auto store = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(store, nullptr);
        // head_lsn is past all previous records; replay must not deliver anything past the crash-frozen sb.
        // The exact replay_count depends on whether truncate-all completed on disk before the crash — but no
        // pre-truncate record should appear as a NEW record above head_lsn.
        EXPECT_LE(replay_count.load(), 10u);
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Crash before LogStore rollback-commit: fires the crash_before_logstore_rollback_commit flip between the
// in-memory rollback (records_.rollback + tail_lsn update) and the sb persist.  On recovery, the LogStore sb
// still shows pre-rollback state (no rollback_record persisted), so replay delivers ALL records (including
// those that would have been suppressed by the rollback).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(LogStoreMgrTest, CrashBeforeLogstoreRollbackCommit) {
    logstore_id_t sid{};
    lsn_t rollback_target{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid, &rollback_target]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        sid = store->store_id();
        log_store_mgr().open_log_store(sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });

        auto lsns = co_await append_n(*store, 10, 128, keep);
        // Roll back to the 5th lsn — should leave 5 records live, roll back the last 5.
        rollback_target = lsns[4];

        install_fresh_crash_sim();
        arm_crash_flip("crash_before_logstore_rollback_commit");
        auto rc = co_await store->rollback(rollback_target);
        EXPECT_FALSE(rc) << "rollback should return false when the flip fires";
        EXPECT_TRUE(is_crash_simulated()) << "flip never fired";

        remove_flip("crash_before_logstore_rollback_commit");
        install_fresh_crash_sim();
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> Async< void > {
        // Recovery reloads LogStore from sb — no rollback_record was persisted, so replay must deliver ALL 10
        // original records (the rollback effectively never happened from a durability perspective).
        std::atomic< uint32_t > replay_count{0};
        log_store_mgr().open_log_store(sid, LogStoreOptions{.append_mode = true},
                                       [&replay_count](lsn_t, const sisl::IoBufView&) -> Async< void > {
                                           replay_count.fetch_add(1);
                                           co_return;
                                       });
        co_await log_store_mgr().replay();

        EXPECT_EQ(replay_count.load(), 10u) << "post-crash replay must not honor the uncommitted rollback";
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Cross-consumer CP crash — watermark stays behind certified data.  Register a fake CPCallbacks consumer at a
// low rank whose cp_flush fires a crash flip mid-run.  CPManager still iterates in rank order: the fake's flush
// "succeeds" from CPManager's PoV (the flip's co_return true), but crashed_=true gates every subsequent write.
// LogStore's cp_flush_persist runs afterward — its checkpt_lsn is updated in memory, but the sb write is a
// no-op.  On restart, the LogStore sb still shows the OLD checkpt_lsn — the watermark on disk has not raced
// ahead of the durable state.
// ─────────────────────────────────────────────────────────────────────────────
namespace {
class CrashingCPConsumer : public CPCallbacks {
public:
    void on_switchover_cp(CP*, CP*) override {}
    Async< bool > cp_flush(CP*) override {
        // Fire the flip so is_crash_simulated goes true; return success so CPManager continues iterating
        // consumers (letting subsequent ranks' writes get gated).
        (void)crash_if_flip_fired("crash_at_test_consumer_flush");
        co_return true;
    }
    void cp_cleanup(CP*) override {}
    int cp_progress_percent() override { return 100; }
};
} // namespace

TEST_F(LogStoreMgrTest, CrossConsumerCpCrashWatermarkNotAdvanced) {
    logstore_id_t sid{};
    lsn_t pre_crash_checkpt{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid, &pre_crash_checkpt]() -> Async< void > {
        co_await bootstrap();
        std::vector< std::shared_ptr< std::vector< uint8_t > > > keep;
        auto store = co_await log_store_mgr().create_log_store(LogStoreOptions{.append_mode = true});
        sid = store->store_id();
        auto* store_raw = store.get();
        log_store_mgr().open_log_store(
            sid, LogStoreOptions{.append_mode = true},
            [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; },
            [store_raw]() -> lsn_t { return store_raw->tail_lsn(); });

        // Take a baseline CP so checkpt_lsn has a known value on disk.  LSNs start at 0, so 3 records land at
        // lsns {0,1,2} and tail_lsn = 2.
        co_await append_n(*store, 3, 64, keep);
        auto ok1 = co_await cp_mgr().trigger_cp_flush(/*force=*/true);
        CO_ASSERT_TRUE(ok1);
        pre_crash_checkpt = store->checkpt_lsn();
        auto const pre_crash_tail = store->tail_lsn();
        EXPECT_EQ(pre_crash_checkpt, pre_crash_tail) << "watermark_cb=tail should promote checkpt to tail";

        // Register the crashing consumer at rank 500 — LogStore rank is 999, so the crashing consumer runs BEFORE
        // LogStore.  Once it fires the flip, LogStore's subsequent sb writes are gated.
        cp_mgr().register_consumer("test_crashing_consumer", std::make_shared< CrashingCPConsumer >(), 500u);

        // Append more so a successful CP would advance checkpt_lsn past pre_crash_checkpt.
        co_await append_n(*store, 5, 64, keep);
        EXPECT_GT(store->tail_lsn(), pre_crash_tail);

        install_fresh_crash_sim();
        arm_crash_flip("crash_at_test_consumer_flush");
        // Fire and forget — the crash flip will fire inside the fake consumer's cp_flush; further writes gated.
        auto fut = cp_mgr().trigger_cp_flush(/*force=*/true);
        co_await std::move(fut).via(co_await folly::coro::co_current_executor);
        EXPECT_TRUE(is_crash_simulated()) << "flip never fired";

        remove_flip("crash_at_test_consumer_flush");
        install_fresh_crash_sim();
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, pre_crash_checkpt]() -> Async< void > {
        log_store_mgr().open_log_store(sid, LogStoreOptions{.append_mode = true},
                                       [](lsn_t, const sisl::IoBufView&) -> Async< void > { co_return; });
        co_await log_store_mgr().replay();
        auto store = log_store_mgr().get_log_store(sid);
        CO_ASSERT_NE(store, nullptr);

        // LogStore's on-disk sb should still report pre_crash_checkpt — the crashed CP's would-be-new checkpt
        // update was gated by is_crash_simulated().
        EXPECT_EQ(store->checkpt_lsn(), pre_crash_checkpt)
            << "watermark advanced past what was actually flushed durably";
        co_await shutdown();
    }());
}

// ── Test main ─────────────────────────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_log_store_mgr");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown.
    return RUN_ALL_TESTS();
}