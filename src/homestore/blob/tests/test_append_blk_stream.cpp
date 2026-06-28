/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Licensed under the License is distributed on an "AS IS" BASIS, WITHOUT
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
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <folly/coro/Collect.h>

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

#include "homestore/blob/append_blk_stream.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;

SISL_OPTION_GROUP(test_append_blk_stream,
                  (num_io, "", "num_io", "number of IO operations per test",
                   ::cxxopts::value< uint32_t >()->default_value("100"), "number"));

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;      // 256 MB per device
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev
static constexpr uint64_t CHUNK_SIZE = 4 * 1024 * 1024;      // 4 MB per chunk
static constexpr uint32_t BLK_SIZE = 4096;

class AppendBlkStreamTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test fresh reactors: CPManager's t_cp_info_ thread_local would otherwise dangle into the freed
        // prior-test CPManager's owned_stacks_, and the next cp_guard() reads it as UAF.
        iomanager::init_iomgr(2);
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_append_blk_stream_{}", i);
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

    folly::coro::Task< void > bootstrap() {
        dm_ = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(true /* first_time_boot */);

        co_await BlobDevManager::create();

        VDevParameters params;
        params.initial_chunk_size = CHUNK_SIZE;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        using namespace std::string_literals;
        blob_dev_ = co_await blob_dev_mgr().create_blob_dev("abs_bd"s, std::move(params));
    }

    // Drive a full reload from the main test thread.  Cycling iomgr (kills reactor TLS, including
    // CPManager's cached ThreadStackInfo pointer that would otherwise dangle into the freed CPManager)
    // requires the main thread because stop_iomgr joins reactor threads — a reactor calling it would
    // self-join.  Two coroutine phases bracket the cycle: one to tear down on the old reactor pool, one
    // to bring up on the new pool.
    void reload_sync() {
        iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
            blob_dev_.reset();
            co_await cp_mgr().shutdown();
            blob_dev_mgr().shutdown();
            co_await dm_->close_devices();
            Managers::reset();
        }());
        iomanager::stop_iomgr();
        iomanager::init_iomgr(2);
        iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
            dm_ = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
            co_await dm_->load_devices();
            co_await MetaBlkManager::load();
            auto cpmgr = CPManager::create();
            co_await cpmgr->start(false /* first_time_boot */);
            co_await BlobDevManager::load();
            blob_dev_ = blob_dev_mgr().get_blob_dev("abs_bd");
        }());
    }

    folly::coro::Task< void > shutdown() {
        blob_dev_.reset();
        co_await cp_mgr().shutdown();
        blob_dev_mgr().shutdown();
        co_await dm_->close_devices();
    }

    static void fill_buf(uint8_t* buf, size_t size, uint64_t seed) {
        auto* p = reinterpret_cast< uint64_t* >(buf);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            p[i] = seed ^ i;
        }
    }

    static bool verify_buf(const uint8_t* buf, size_t size, uint64_t seed) {
        auto* p = reinterpret_cast< const uint64_t* >(buf);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            if (p[i] != (seed ^ i)) { return false; }
        }
        return true;
    }

    // Build a IoBufShared of `size` bytes filled with `seed`.
    static sisl::IoBufShared make_bytes(size_t size, uint64_t seed) {
        auto ba = sisl::make_io_buf_shared(to_u32(size));
        fill_buf(ba->bytes(), size, seed);
        return ba;
    }

    // Async append wrapper that takes an existing IoBufShared (moves into stream).
    static folly::coro::Task< BlkId > append_record(AppendBlkStream& s, CP* cp, uint16_t segment_id, size_t size,
                                                    uint64_t seed) {
        auto ba = make_bytes(size, seed);
        // Try the sync fast path first; fall back to async append on null.
        auto opt = s.quick_append(cp, segment_id, ba);
        if (opt.has_value()) { co_return opt.value(); }
        co_return co_await s.append(cp, segment_id, std::move(ba));
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< DeviceManager > dm_;
    shared< BlobDev > blob_dev_;
};

// ── Create / multi-stream basics ─────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, CreateStream) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);
    CO_ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->num_chunks(), 1u);
    EXPECT_EQ(stream->chunk_size(), CHUNK_SIZE);
    EXPECT_EQ(stream->block_size(), BLK_SIZE);

    auto streams = self.blob_dev_->append_blk_streams();
    EXPECT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0]->stream_id(), stream->stream_id());

    co_await self.shutdown();
}

CORO_TEST_F(AppendBlkStreamTest, MultipleStreams) {
    co_await self.bootstrap();

    auto s1 = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);
    auto s2 = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);
    auto s3 = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    EXPECT_NE(s1->stream_id(), s2->stream_id());
    EXPECT_NE(s2->stream_id(), s3->stream_id());
    EXPECT_NE(s1->stream_id(), s3->stream_id());
    EXPECT_EQ(self.blob_dev_->append_blk_streams().size(), 3u);

    co_await self.shutdown();
}

// ── Append fast / slow paths ─────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, QuickAppendFastPath) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    {
        auto guard = cp_mgr().cp_guard();

        // First call: no active WriteUnit yet → quick_append should return nullopt; caller falls back to async.
        auto ba = self.make_bytes(BLK_SIZE, 0x1A01);
        auto opt = stream->quick_append(guard.get(), /*segment=*/0, ba);
        EXPECT_FALSE(opt.has_value());
        auto bid = co_await stream->append(guard.get(), /*segment=*/0, std::move(ba));
        EXPECT_EQ(bid.blk_count(), 1u);

        // Second call: active unit exists → fast path should succeed.
        auto ba2 = self.make_bytes(BLK_SIZE, 0x1A02);
        auto opt2 = stream->quick_append(guard.get(), /*segment=*/0, ba2);
        EXPECT_TRUE(opt2.has_value());
        EXPECT_EQ(opt2->blk_count(), 1u);
    }

    co_await self.shutdown();
}

CORO_TEST_F(AppendBlkStreamTest, AppendFlushAndRead) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    constexpr uint32_t kRecords = 10;
    std::vector< std::pair< BlkId, uint64_t > > entries;
    {
        auto guard = cp_mgr().cp_guard();
        for (uint32_t i = 0; i < kRecords; ++i) {
            auto bid = co_await self.append_record(*stream, guard.get(), 0, BLK_SIZE, 0x2A00 + i);
            entries.emplace_back(bid, 0x2A00 + i);
        }
    }
    co_await cp_mgr().trigger_cp_flush(true);

    for (auto const& [bid, seed] : entries) {
        IoBuf rbuf(BLK_SIZE, 512);
        auto ec = co_await stream->read(rbuf, bid);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, seed))
            << "mismatch at blk_num=" << bid.blk_num() << " chunk=" << bid.chunk_num();
    }

    co_await self.shutdown();
}

CORO_TEST_F(AppendBlkStreamTest, MultiSegmentAppend) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    constexpr uint16_t kSegments = 4;
    constexpr uint32_t kPerSeg = 8;
    std::unordered_map< uint16_t, std::vector< std::pair< BlkId, uint64_t > > > by_seg;
    {
        auto guard = cp_mgr().cp_guard();
        for (uint16_t seg = 0; seg < kSegments; ++seg) {
            for (uint32_t i = 0; i < kPerSeg; ++i) {
                uint64_t seed = (uint64_t(seg) << 16) | i;
                auto bid = co_await self.append_record(*stream, guard.get(), seg, BLK_SIZE, seed);
                by_seg[seg].emplace_back(bid, seed);
            }
        }
    }
    co_await cp_mgr().trigger_cp_flush(true);

    for (auto const& [seg, entries] : by_seg) {
        for (auto const& [bid, seed] : entries) {
            IoBuf rbuf(BLK_SIZE, 512);
            auto ec = co_await stream->read(rbuf, bid);
            CO_ASSERT_FALSE(ec);
            EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, seed))
                << "mismatch at seg=" << seg << " blk=" << bid.blk_num();
        }
    }

    co_await self.shutdown();
}

CORO_TEST_F(AppendBlkStreamTest, MultiBlockAppend) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    // Append a buffer that spans multiple blocks within a single record.
    constexpr blk_count_t kNblks = 8;
    constexpr size_t kSize = kNblks * BLK_SIZE;
    BlkId bid;
    {
        auto guard = cp_mgr().cp_guard();
        bid = co_await self.append_record(*stream, guard.get(), 0, kSize, 0x3A01);
    }
    EXPECT_EQ(bid.blk_count(), kNblks);
    co_await cp_mgr().trigger_cp_flush(true);

    IoBuf rbuf(kSize, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), kSize, 0x3A01));

    co_await self.shutdown();
}

// ── Invalidate ───────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, InvalidateAppendedBlk) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    BlkId bid;
    {
        auto guard = cp_mgr().cp_guard();
        bid = co_await self.append_record(*stream, guard.get(), 0, BLK_SIZE, 0x4A01);
    }
    co_await cp_mgr().trigger_cp_flush(true);

    // Invalidate.
    {
        auto guard = cp_mgr().cp_guard();
        stream->invalidate(guard.get(), bid);
    }
    // No public allocator API to inspect free state directly; the bitmap-flush + restart path covers
    // the persistence side (see RestartAfterInvalidate).  Here we just verify no crash and a follow-up
    // append still succeeds.
    {
        auto guard = cp_mgr().cp_guard();
        auto bid2 = co_await self.append_record(*stream, guard.get(), 0, BLK_SIZE, 0x4A02);
        EXPECT_EQ(bid2.blk_count(), 1u);
    }

    co_await self.shutdown();
}

// ── CP integration ───────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, CPSwitchoverAndFlush) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    BlkId bid;
    {
        auto guard = cp_mgr().cp_guard();
        bid = co_await self.append_record(*stream, guard.get(), 0, BLK_SIZE, 0x5A01);
        EXPECT_TRUE(stream->is_dirty(guard->id()));
    }

    // Trigger a CP flush — exercises on_cp_switchover + cp_flush via BlobDevManager.
    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    // Read survives the flush.
    IoBuf rbuf(BLK_SIZE, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, 0x5A01));

    co_await self.shutdown();
}

// ── Expand on demand ─────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, ExpandOnDemand) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);
    EXPECT_EQ(stream->num_chunks(), 1u);

    // Append more bytes than a single chunk holds; the stream should expand.
    blk_count_t const blks_per_chunk = CHUNK_SIZE / BLK_SIZE;
    {
        auto guard = cp_mgr().cp_guard();
        for (blk_count_t i = 0; i < blks_per_chunk + 8; ++i) {
            co_await self.append_record(*stream, guard.get(), 0, BLK_SIZE, 0x6A00 + i);
        }
    }
    co_await cp_mgr().trigger_cp_flush(true);

    EXPECT_GE(stream->num_chunks(), 2u);

    co_await self.shutdown();
}

// ── Bulk write/read ──────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, BulkAppendReadVerify) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);
    const uint32_t num_io = SISL_OPTIONS["num_io"].as< uint32_t >();

    std::mt19937 rng{42};
    std::uniform_int_distribution< uint32_t > size_dist(1, 8); // blocks per record
    std::uniform_int_distribution< uint16_t > seg_dist(0, 7);

    struct IoEntry {
        BlkId bid;
        uint64_t seed;
    };
    std::vector< IoEntry > entries;
    entries.reserve(num_io);

    {
        auto guard = cp_mgr().cp_guard();
        for (uint32_t i = 0; i < num_io; ++i) {
            blk_count_t nblks = size_dist(rng);
            uint16_t seg = seg_dist(rng);
            uint64_t seed = 0x7000 + i;
            auto bid = co_await self.append_record(*stream, guard.get(), seg, nblks * BLK_SIZE, seed);
            entries.push_back({bid, seed});
        }
    }
    co_await cp_mgr().trigger_cp_flush(true);

    for (auto const& [bid, seed] : entries) {
        uint32_t io_size = bid.blk_count() * BLK_SIZE;
        IoBuf rbuf(io_size, 512);
        auto ec = co_await stream->read(rbuf, bid);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), io_size, seed))
            << "mismatch at blk_num=" << bid.blk_num() << " chunk=" << bid.chunk_num();
    }

    co_await self.shutdown();
}

// ── Concurrency ──────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendBlkStreamTest, ConcurrentAppendsDifferentSegments) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_blk_stream(CHUNK_SIZE);

    constexpr uint16_t kSegments = 4;
    constexpr uint32_t kPerSeg = 25;
    std::vector< std::vector< std::pair< BlkId, uint64_t > > > per_seg(kSegments);

    // Coroutine factory that takes arguments by value so captured state survives inside the coroutine frame.
    auto run_seg = [](AppendBlkStream& stream, CP* cp, uint16_t seg, uint32_t per_seg_count,
                      std::vector< std::pair< BlkId, uint64_t > >* out) -> folly::coro::Task< void > {
        for (uint32_t i = 0; i < per_seg_count; ++i) {
            uint64_t seed = (uint64_t(seg) << 16) | i;
            auto bid = co_await AppendBlkStreamTest::append_record(stream, cp, seg, BLK_SIZE, seed);
            out->emplace_back(bid, seed);
        }
        co_return;
    };

    {
        auto guard = cp_mgr().cp_guard();
        auto* cp = guard.get();

        std::vector< folly::coro::Task< void > > tasks;
        for (uint16_t seg = 0; seg < kSegments; ++seg) {
            tasks.push_back(run_seg(*stream, cp, seg, kPerSeg, &per_seg[seg]));
        }
        co_await folly::coro::collectAllRange(std::move(tasks));
    }
    co_await cp_mgr().trigger_cp_flush(true);

    for (uint16_t seg = 0; seg < kSegments; ++seg) {
        for (auto const& [bid, seed] : per_seg[seg]) {
            IoBuf rbuf(BLK_SIZE, 512);
            auto ec = co_await stream->read(rbuf, bid);
            CO_ASSERT_FALSE(ec);
            EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, seed));
        }
    }

    co_await self.shutdown();
}

// ── Restart recovery ─────────────────────────────────────────────────────────────────────────────────────────────────

TEST_F(AppendBlkStreamTest, RestartRecovery) {
    constexpr uint32_t kRecords = 16;
    std::vector< std::pair< BlkId, uint64_t > > entries;
    uint64_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, &entries, &sid]() -> folly::coro::Task< void > {
                                co_await bootstrap();
                                auto stream = co_await blob_dev_->create_append_blk_stream(CHUNK_SIZE);
                                sid = stream->stream_id();
                                {
                                    auto guard = cp_mgr().cp_guard();
                                    for (uint32_t i = 0; i < kRecords; ++i) {
                                        auto bid = co_await append_record(*stream, guard.get(), 0, BLK_SIZE,
                                                                          0x8A00 + i);
                                        entries.emplace_back(bid, 0x8A00 + i);
                                    }
                                }
                                co_await cp_mgr().trigger_cp_flush(true);
                                auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
                                CO_ASSERT_TRUE(success);
                            }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &entries, sid]() -> folly::coro::Task< void > {
        auto streams = blob_dev_->append_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        EXPECT_EQ(recovered->stream_id(), sid);
        for (auto const& [bid, seed] : entries) {
            IoBuf rbuf(BLK_SIZE, 512);
            auto ec = co_await recovered->read(rbuf, bid);
            CO_ASSERT_FALSE(ec);
            EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, seed))
                << "mismatch after restart at blk_num=" << bid.blk_num();
        }
        co_await shutdown();
    }());
}

TEST_F(AppendBlkStreamTest, RestartAfterInvalidate) {
    uint64_t sid{};
    BlkId keep_bid;

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, &sid, &keep_bid]() -> folly::coro::Task< void > {
                                co_await bootstrap();
                                auto stream = co_await blob_dev_->create_append_blk_stream(CHUNK_SIZE);
                                sid = stream->stream_id();
                                BlkId free_bid;
                                {
                                    auto guard = cp_mgr().cp_guard();
                                    keep_bid = co_await append_record(*stream, guard.get(), 0, BLK_SIZE, 0x9A01);
                                    free_bid = co_await append_record(*stream, guard.get(), 0, BLK_SIZE, 0x9A02);
                                }
                                co_await cp_mgr().trigger_cp_flush(true);
                                {
                                    auto guard = cp_mgr().cp_guard();
                                    stream->invalidate(guard.get(), free_bid);
                                }
                                auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
                                CO_ASSERT_TRUE(success);
                            }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, keep_bid]() -> folly::coro::Task< void > {
        auto recovered = blob_dev_->append_blk_streams().at(0);
        EXPECT_EQ(recovered->stream_id(), sid);
        IoBuf rbuf(BLK_SIZE, 512);
        auto ec = co_await recovered->read(rbuf, keep_bid);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0x9A01));
        co_await shutdown();
    }());
}

TEST_F(AppendBlkStreamTest, RestartRecoveryMultipleStreams) {
    uint64_t sid1{}, sid2{};
    BlkId b1, b2;

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, &sid1, &sid2, &b1, &b2]() -> folly::coro::Task< void > {
                                co_await bootstrap();
                                auto s1 = co_await blob_dev_->create_append_blk_stream(CHUNK_SIZE);
                                auto s2 = co_await blob_dev_->create_append_blk_stream(CHUNK_SIZE);
                                sid1 = s1->stream_id();
                                sid2 = s2->stream_id();
                                {
                                    auto guard = cp_mgr().cp_guard();
                                    b1 = co_await append_record(*s1, guard.get(), 0, BLK_SIZE, 0xAAAA);
                                    b2 = co_await append_record(*s2, guard.get(), 0, BLK_SIZE, 0xBBBB);
                                }
                                co_await cp_mgr().trigger_cp_flush(true);
                                auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
                                CO_ASSERT_TRUE(success);
                            }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, sid1, sid2, b1, b2]() -> folly::coro::Task< void > {
                                auto streams = blob_dev_->append_blk_streams();
                                CO_ASSERT_EQ(streams.size(), 2u);
                                for (auto const& s : streams) {
                                    IoBuf rbuf(BLK_SIZE, 512);
                                    if (s->stream_id() == sid1) {
                                        auto ec = co_await s->read(rbuf, b1);
                                        CO_ASSERT_FALSE(ec);
                                        EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xAAAA));
                                    } else {
                                        EXPECT_EQ(s->stream_id(), sid2);
                                        auto ec = co_await s->read(rbuf, b2);
                                        CO_ASSERT_FALSE(ec);
                                        EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xBBBB));
                                    }
                                }
                                co_await shutdown();
                            }());
}

TEST_F(AppendBlkStreamTest, DoubleRestart) {
    BlkId b1, b2;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &b1]() -> folly::coro::Task< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_append_blk_stream(CHUNK_SIZE);
        {
            auto guard = cp_mgr().cp_guard();
            b1 = co_await append_record(*stream, guard.get(), 0, BLK_SIZE, 0xDA01);
        }
        co_await cp_mgr().trigger_cp_flush(true);
        co_await cp_mgr().trigger_cp_flush(true);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &b2]() -> folly::coro::Task< void > {
        auto recovered = blob_dev_->append_blk_streams().at(0);
        {
            auto guard = cp_mgr().cp_guard();
            b2 = co_await append_record(*recovered, guard.get(), 0, BLK_SIZE, 0xDA02);
        }
        co_await cp_mgr().trigger_cp_flush(true);
        co_await cp_mgr().trigger_cp_flush(true);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, b1, b2]() -> folly::coro::Task< void > {
        auto recovered2 = blob_dev_->append_blk_streams().at(0);
        IoBuf rbuf(BLK_SIZE, 512);
        auto ec = co_await recovered2->read(rbuf, b1);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xDA01));
        ec = co_await recovered2->read(rbuf, b2);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xDA02));
        co_await shutdown();
    }());
}

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_append_blk_stream");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown.
    return RUN_ALL_TESTS();
}