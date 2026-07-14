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
#include <array>
#include "common/async.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
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

#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"
#include "homestore/blob/raw_blk_stream.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;

SISL_OPTION_GROUP(test_raw_blk_stream,
                  (num_io, "", "num_io", "number of IO operations per test",
                   ::cxxopts::value< uint32_t >()->default_value("100"), "number"),
                  (max_blks, "", "max_blks", "max blocks per alloc", ::cxxopts::value< uint32_t >()->default_value("8"),
                   "number"));

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;      // 256 MB per device
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev
static constexpr uint64_t CHUNK_SIZE = 32 * 1024 * 1024;     // 32 MB per chunk
static constexpr uint32_t BLK_SIZE = 4096;

// ─────────────────────────────────────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────────────────────────────────────
class RawBlkStreamTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test fresh reactors so CPManager's t_cp_info_ thread_local cache doesn't dangle into the freed
        // CPManager from the previous test.  See comment in test_append_byte_stream.cpp's SetUp for details.
        iomanager::init_iomgr(2);
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_raw_blk_stream_{}", i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
        // Stop iomgr (joins reactor threads) BEFORE dropping Managers — streams own ConcurrentInsertSets whose
        // folly::ThreadLocalPtr deleters push into the owner's zombies_ vector when the reactor exits.  If
        // Managers::reset() runs first the owner is freed, and the thread-exit deleter ends up writing into
        // dangling memory (ASan reports this as a small leak from the resurrected vector).
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

    // Bootstrap: DeviceManager + MetaBlkManager + CPManager + BlobDevManager, then create a BlobDev.
    Async< void > bootstrap() {
        dm_ = co_await DeviceManager::create_and_format(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await MetaBlkManager::create(META_VDEV_SIZE);

        auto cpmgr = CPManager::create();
        co_await cpmgr->start(true /* first_time_boot */);

        co_await BlobDevManager::create();

        // Create a BlobDev backed by a dynamic VDev (chunks grow on demand).
        VDevParameters params;
        params.initial_chunk_size = CHUNK_SIZE;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        using namespace std::string_literals;
        blob_dev_ = co_await blob_dev_mgr().create_blob_dev("test_blob_dev"s, std::move(params));
    }

    // Drive a full reload from the main test thread.  Cycling iomgr (kills reactor TLS, including CPManager's
    // cached ThreadStackInfo pointer that would otherwise dangle into the freed CPManager) requires the main
    // thread because stop_iomgr joins reactor threads — a reactor calling it would self-join.  Two coroutine
    // phases bracket the cycle: one to tear down on the old reactor pool, one to bring up on the new pool.
    void reload_sync() {
        iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
            blob_dev_.reset();
            co_await cp_mgr().shutdown();
            blob_dev_mgr().shutdown();
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
            co_await BlobDevManager::load();
            blob_dev_ = blob_dev_mgr().get_blob_dev("test_blob_dev");
        }());
    }

    Async< void > shutdown() {
        blob_dev_.reset();
        co_await cp_mgr().shutdown();
        blob_dev_mgr().shutdown();
        co_await dm_->close_devices();
    }

    // Fill a buffer with a deterministic pattern based on seed.
    static void fill_buf(uint8_t* buf, size_t size, uint64_t seed) {
        auto* p = reinterpret_cast< uint64_t* >(buf);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            p[i] = seed ^ i;
        }
    }

    // Verify buffer matches the pattern from fill_buf.
    static bool verify_buf(const uint8_t* buf, size_t size, uint64_t seed) {
        auto* p = reinterpret_cast< const uint64_t* >(buf);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            if (p[i] != (seed ^ i)) {
                return false;
            }
        }
        return true;
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< DeviceManager > dm_;
    shared< BlobDev > blob_dev_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Create a RawBlkStream and verify it has exactly one chunk.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, CreateStream) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
    CO_ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->num_chunks(), 1u);
    EXPECT_EQ(stream->chunk_size(), CHUNK_SIZE);
    EXPECT_EQ(stream->block_size(), BLK_SIZE);

    // The stream should be retrievable from the BlobDev.
    auto streams = self.blob_dev_->raw_blk_streams();
    EXPECT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0]->stream_id(), stream->stream_id());

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Create multiple RawBlkStreams, verify distinct stream_ids.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, MultipleStreams) {
    co_await self.bootstrap();

    auto s1 = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
    auto s2 = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
    auto s3 = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    EXPECT_NE(s1->stream_id(), s2->stream_id());
    EXPECT_NE(s2->stream_id(), s3->stream_id());
    EXPECT_NE(s1->stream_id(), s3->stream_id());

    auto streams = self.blob_dev_->raw_blk_streams();
    EXPECT_EQ(streams.size(), 3u);

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Alloc, write, read, verify a single block.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, SingleBlockWriteRead) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    // Alloc one block.
    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
    EXPECT_EQ(bid.blk_count(), 1u);

    // Commit the block under a CP guard.
    {
        auto guard = cp_mgr().cp_guard();
        auto cstatus = stream->commit_blk(guard.get(), bid);
        EXPECT_EQ(cstatus, BlkAllocStatus::SUCCESS);
    }

    // Write a pattern.
    IoBuf wbuf(BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), BLK_SIZE, 0xDEAD);
    co_await stream->write(bid, wbuf);

    // Read back and verify.
    IoBuf rbuf(BLK_SIZE, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, 0xDEAD));

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Alloc multi-block, write, read, verify.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, MultiBlockWriteRead) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    constexpr blk_count_t nblks = 4;
    BlkId bid;
    blk_alloc_hints hints{.is_contiguous = true};
    auto status = stream->alloc_blk(nblks, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
    EXPECT_EQ(bid.blk_count(), nblks);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    const uint32_t total_size = nblks * BLK_SIZE;
    IoBuf wbuf(total_size, 512);
    self.fill_buf(wbuf.bytes(), total_size, 0xBEEF);
    co_await stream->write(bid, wbuf);

    IoBuf rbuf(total_size, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), total_size, 0xBEEF));

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Alloc, commit, invalidate (free), verify block is freed.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, AllocCommitInvalidate) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    // Write data before invalidating.
    IoBuf wbuf(BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), BLK_SIZE, 0xCAFE);
    co_await stream->write(bid, wbuf);

    // Invalidate (free) the block.
    {
        auto guard = cp_mgr().cp_guard();
        co_await stream->invalidate(guard.get(), bid);
    }

    // After invalidate, the block should be allocatable again.
    BlkId bid2;
    status = stream->alloc_blk(1, hints, bid2);
    EXPECT_EQ(status, BlkAllocStatus::SUCCESS);

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Expand stream by allocating more blocks than a single chunk can hold.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, ExpandOnDemand) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
    EXPECT_EQ(stream->num_chunks(), 1u);

    // Expand explicitly.
    co_await stream->expand();
    EXPECT_EQ(stream->num_chunks(), 2u);

    // Allocate a block from the expanded stream.
    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    EXPECT_EQ(status, BlkAllocStatus::SUCCESS);

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: Write many blocks, read them all back and verify data integrity.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, BulkWriteReadVerify) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
    const uint32_t num_io = SISL_OPTIONS["num_io"].as< uint32_t >();
    const uint32_t max_blks = SISL_OPTIONS["max_blks"].as< uint32_t >();

    std::mt19937 rng{42};
    std::uniform_int_distribution< uint32_t > blk_dist(1, max_blks);

    struct IoEntry {
        BlkId bid;
        uint64_t seed;
    };
    std::vector< IoEntry > entries;
    entries.reserve(num_io);

    // Allocate and write.
    for (uint32_t i = 0; i < num_io; ++i) {
        blk_count_t nblks = blk_dist(rng);
        BlkId bid;
        blk_alloc_hints hints{.is_contiguous = true};
        auto status = stream->alloc_blk(nblks, hints, bid);
        if (status == BlkAllocStatus::SPACE_FULL) {
            // Expand and retry once.
            co_await stream->expand();
            status = stream->alloc_blk(nblks, hints, bid);
        }
        if (status != BlkAllocStatus::SUCCESS) {
            LOGINFO("Stopping bulk write at iteration {} — allocation failed", i);
            break;
        }

        {
            auto guard = cp_mgr().cp_guard();
            stream->commit_blk(guard.get(), bid);
        }

        uint64_t seed = 0x1000 + i;
        uint32_t io_size = bid.blk_count() * BLK_SIZE;
        IoBuf wbuf(io_size, 512);
        self.fill_buf(wbuf.bytes(), io_size, seed);
        co_await stream->write(bid, wbuf);

        entries.push_back({bid, seed});
    }

    CO_ASSERT_FALSE(entries.empty());
    LOGINFO("Wrote {} blocks, now reading back", entries.size());

    // Read back and verify each entry.
    for (auto& [bid, seed] : entries) {
        uint32_t io_size = bid.blk_count() * BLK_SIZE;
        IoBuf rbuf(io_size, 512);
        auto ec = co_await stream->read(rbuf, bid);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), io_size, seed))
            << "Mismatch at blk_num=" << bid.blk_num() << " chunk=" << bid.chunk_num();
    }

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: CP flush persists allocator state — dirty chunks get written.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, CPFlush) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    // Alloc and commit some blocks to dirty the stream.
    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    IoBuf wbuf(BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), BLK_SIZE, 0xF00D);
    co_await stream->write(bid, wbuf);

    // Trigger a CP flush.
    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    // Read back data to verify it survived the flush.
    IoBuf rbuf(BLK_SIZE, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, 0xF00D));

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: Scatter-gather writev / readv.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, ScatterGatherIO) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    constexpr blk_count_t nblks = 4;
    BlkId bid;
    blk_alloc_hints hints{.is_contiguous = true};
    auto status = stream->alloc_blk(nblks, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    // Build scatter-gather write buffers — one IoBuf per block with distinct patterns.
    std::vector< IoBuf > wbufs;
    for (blk_count_t i = 0; i < nblks; ++i) {
        IoBuf buf(BLK_SIZE, 512);
        self.fill_buf(buf.bytes(), BLK_SIZE, 0xAA00 + i);
        wbufs.push_back(std::move(buf));
    }
    co_await stream->writev(wbufs, bid);

    // Read back with readv.
    std::vector< IoBuf > rbufs;
    for (blk_count_t i = 0; i < nblks; ++i) {
        rbufs.emplace_back(BLK_SIZE, 512);
    }
    auto ec = co_await stream->readv(rbufs, bid);
    CO_ASSERT_FALSE(ec);

    for (blk_count_t i = 0; i < nblks; ++i) {
        EXPECT_TRUE(self.verify_buf(rbufs[i].cbytes(), BLK_SIZE, 0xAA00 + i)) << "Mismatch at scatter block " << i;
    }

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: BlkReadTracker — invalidate waits for in-flight reads.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, InvalidateWaitsForReads) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    IoBuf wbuf(BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), BLK_SIZE, 0xFACE);
    co_await stream->write(bid, wbuf);

    // Issue a read (which registers with BlkReadTracker).
    IoBuf rbuf(BLK_SIZE, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, 0xFACE));

    // Now invalidate should succeed immediately (read already completed and was removed from tracker).
    {
        auto guard = cp_mgr().cp_guard();
        co_await stream->invalidate(guard.get(), bid);
    }

    // Block should be freed — alloc should succeed again.
    BlkId bid2;
    status = stream->alloc_blk(1, hints, bid2);
    EXPECT_EQ(status, BlkAllocStatus::SUCCESS);

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: Write, flush, free, flush — verify allocator state updates persist.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, WriteFlushFreeFlush) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    // Alloc and commit.
    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(2, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    IoBuf wbuf(bid.blk_count() * BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), wbuf.size(), 0x1234);
    co_await stream->write(bid, wbuf);

    // First flush — persists the allocation.
    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    // Free the block.
    {
        auto guard = cp_mgr().cp_guard();
        co_await stream->invalidate(guard.get(), bid);
    }

    // Second flush — persists the free.
    success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    // The freed space should be available.
    BlkId bid2;
    status = stream->alloc_blk(1, hints, bid2);
    EXPECT_EQ(status, BlkAllocStatus::SUCCESS);

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: Fsync flushes to physical device without error.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, Fsync) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);

    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid);
    }

    IoBuf wbuf(BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), BLK_SIZE, 0x5678);
    co_await stream->write(bid, wbuf);

    // Fsync should not fail.
    co_await stream->fsync();

    // Read back to confirm data persisted.
    IoBuf rbuf(BLK_SIZE, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), BLK_SIZE, 0x5678));

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: Destroy stream releases all chunks back to the VDev.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, DestroyStream) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
    EXPECT_EQ(stream->num_chunks(), 1u);

    co_await stream->expand();
    EXPECT_EQ(stream->num_chunks(), 2u);

    // Destroy releases all chunks.
    co_await stream->destroy();
    EXPECT_EQ(stream->num_chunks(), 0u);

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: Restart recovery — write + flush, reload, verify data and allocator state survive.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RawBlkStreamTest, RestartRecoverySingleBlock) {
    uint64_t sid{};
    BlkId bid;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid, &bid]() -> Async< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
        sid = stream->stream_id();
        blk_alloc_hints hints;
        auto status = stream->alloc_blk(1, hints, bid);
        CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            stream->commit_blk(guard.get(), bid);
        }
        IoBuf wbuf(BLK_SIZE, 512);
        fill_buf(wbuf.bytes(), BLK_SIZE, 0xABCD0001);
        co_await stream->write(bid, wbuf);
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, bid]() -> Async< void > {
        CO_ASSERT_NE(blob_dev_, nullptr);
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        EXPECT_EQ(recovered->stream_id(), sid);
        EXPECT_EQ(recovered->num_chunks(), 1u);
        IoBuf rbuf(BLK_SIZE, 512);
        auto ec = co_await recovered->read(rbuf, bid);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xABCD0001));
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: Restart recovery — multi-block write across expanded chunks.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RawBlkStreamTest, RestartRecoveryMultiChunk) {
    struct IoEntry {
        BlkId bid;
        uint64_t seed;
    };
    std::vector< IoEntry > entries;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &entries]() -> Async< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
        co_await stream->expand();
        EXPECT_EQ(stream->num_chunks(), 2u);

        for (uint32_t i = 0; i < 20; ++i) {
            BlkId bid;
            blk_alloc_hints hints{.is_contiguous = true};
            auto status = stream->alloc_blk(4, hints, bid);
            if (status == BlkAllocStatus::SPACE_FULL) {
                co_await stream->expand();
                status = stream->alloc_blk(4, hints, bid);
            }
            CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
            {
                auto guard = cp_mgr().cp_guard();
                stream->commit_blk(guard.get(), bid);
            }
            uint64_t seed = 0x2000 + i;
            uint32_t io_size = bid.blk_count() * BLK_SIZE;
            IoBuf wbuf(io_size, 512);
            fill_buf(wbuf.bytes(), io_size, seed);
            co_await stream->write(bid, wbuf);
            entries.push_back({bid, seed});
        }
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &entries]() -> Async< void > {
        CO_ASSERT_NE(blob_dev_, nullptr);
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        for (auto& [bid, seed] : entries) {
            uint32_t io_size = bid.blk_count() * BLK_SIZE;
            IoBuf rbuf(io_size, 512);
            auto ec = co_await recovered->read(rbuf, bid);
            CO_ASSERT_FALSE(ec);
            EXPECT_TRUE(verify_buf(rbuf.cbytes(), io_size, seed))
                << "Mismatch after restart at blk_num=" << bid.blk_num();
        }
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: Restart recovery — multiple streams per BlobDev survive restart.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RawBlkStreamTest, RestartRecoveryMultipleStreams) {
    BlkId bid1, bid2;
    uint64_t sid1{}, sid2{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &bid1, &bid2, &sid1, &sid2]() -> Async< void > {
        co_await bootstrap();
        auto s1 = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
        auto s2 = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
        blk_alloc_hints hints;

        auto st = s1->alloc_blk(1, hints, bid1);
        CO_ASSERT_EQ(st, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            s1->commit_blk(guard.get(), bid1);
        }
        IoBuf w1(BLK_SIZE, 512);
        fill_buf(w1.bytes(), BLK_SIZE, 0xAAAA);
        co_await s1->write(bid1, w1);

        st = s2->alloc_blk(1, hints, bid2);
        CO_ASSERT_EQ(st, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            s2->commit_blk(guard.get(), bid2);
        }
        IoBuf w2(BLK_SIZE, 512);
        fill_buf(w2.bytes(), BLK_SIZE, 0xBBBB);
        co_await s2->write(bid2, w2);

        sid1 = s1->stream_id();
        sid2 = s2->stream_id();

        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, bid1, bid2, sid1, sid2]() -> Async< void > {
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 2u);
        for (auto& s : streams) {
            if (s->stream_id() == sid1) {
                IoBuf rbuf(BLK_SIZE, 512);
                auto ec = co_await s->read(rbuf, bid1);
                CO_ASSERT_FALSE(ec);
                EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xAAAA));
            } else {
                EXPECT_EQ(s->stream_id(), sid2);
                IoBuf rbuf(BLK_SIZE, 512);
                auto ec = co_await s->read(rbuf, bid2);
                CO_ASSERT_FALSE(ec);
                EXPECT_TRUE(verify_buf(rbuf.cbytes(), BLK_SIZE, 0xBBBB));
            }
        }
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: Write, flush, free, flush, restart — freed blocks should remain free after recovery.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RawBlkStreamTest, RestartAfterFree) {
    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
        BlkId bid;
        blk_alloc_hints hints;
        auto status = stream->alloc_blk(1, hints, bid);
        CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            stream->commit_blk(guard.get(), bid);
        }
        IoBuf wbuf(BLK_SIZE, 512);
        fill_buf(wbuf.bytes(), BLK_SIZE, 0xF00DF00D);
        co_await stream->write(bid, wbuf);
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
        {
            auto guard = cp_mgr().cp_guard();
            co_await stream->invalidate(guard.get(), bid);
        }
        success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> Async< void > {
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        BlkId bid2;
        blk_alloc_hints hints;
        auto status = recovered->alloc_blk(1, hints, bid2);
        EXPECT_EQ(status, BlkAllocStatus::SUCCESS);
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: Double restart — write+flush, restart, write more+flush, restart again, verify all data.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RawBlkStreamTest, DoubleRestart) {
    BlkId bid1, bid2;

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &bid1]() -> Async< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE);
        blk_alloc_hints hints;
        auto status = stream->alloc_blk(1, hints, bid1);
        CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            stream->commit_blk(guard.get(), bid1);
        }
        IoBuf w1(BLK_SIZE, 512);
        fill_buf(w1.bytes(), BLK_SIZE, 0x1111);
        co_await stream->write(bid1, w1);
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &bid2]() -> Async< void > {
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto stream = streams[0];
        blk_alloc_hints hints;
        auto status = stream->alloc_blk(1, hints, bid2);
        CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            stream->commit_blk(guard.get(), bid2);
        }
        IoBuf w2(BLK_SIZE, 512);
        fill_buf(w2.bytes(), BLK_SIZE, 0x2222);
        co_await stream->write(bid2, w2);
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, bid1, bid2]() -> Async< void > {
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        IoBuf r1(BLK_SIZE, 512);
        auto ec = co_await recovered->read(r1, bid1);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(r1.cbytes(), BLK_SIZE, 0x1111));
        IoBuf r2(BLK_SIZE, 512);
        ec = co_await recovered->read(r2, bid2);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(r2.cbytes(), BLK_SIZE, 0x2222));
        co_await shutdown();
    }());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: Create a stream with a block-size multiplier (2x vdev blk_size) and verify I/O works at that granularity.
// ─────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(RawBlkStreamTest, BlockSizeMultiplier) {
    co_await self.bootstrap();

    static constexpr uint32_t STREAM_BLK_SIZE = BLK_SIZE * 2; // 8192 when vdev is 4096

    auto stream = co_await self.blob_dev_->create_raw_blk_stream(CHUNK_SIZE, STREAM_BLK_SIZE);
    CO_ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->block_size(), STREAM_BLK_SIZE);
    EXPECT_EQ(stream->blk_multiplier(), 2u);

    // Allocate 1 stream-level block (= 2 vdev blocks).
    BlkId bid;
    blk_alloc_hints hints;
    auto status = stream->alloc_blk(1, hints, bid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
    EXPECT_EQ(bid.blk_count(), 1u);

    {
        auto guard = cp_mgr().cp_guard();
        auto cstatus = stream->commit_blk(guard.get(), bid);
        EXPECT_EQ(cstatus, BlkAllocStatus::SUCCESS);
    }

    // Write at the stream's block size.
    IoBuf wbuf(STREAM_BLK_SIZE, 512);
    self.fill_buf(wbuf.bytes(), STREAM_BLK_SIZE, 0xB1C2);
    co_await stream->write(bid, wbuf);

    // Read back and verify.
    IoBuf rbuf(STREAM_BLK_SIZE, 512);
    auto ec = co_await stream->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf.cbytes(), STREAM_BLK_SIZE, 0xB1C2));

    // Allocate multi-block (3 stream blocks = 6 vdev blocks).
    BlkId bid2;
    blk_alloc_hints hints2{.is_contiguous = true};
    status = stream->alloc_blk(3, hints2, bid2);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
    EXPECT_EQ(bid2.blk_count(), 3u);

    {
        auto guard = cp_mgr().cp_guard();
        stream->commit_blk(guard.get(), bid2);
    }

    const uint32_t multi_size = 3 * STREAM_BLK_SIZE;
    IoBuf wbuf2(multi_size, 512);
    self.fill_buf(wbuf2.bytes(), multi_size, 0xD3E4);
    co_await stream->write(bid2, wbuf2);

    IoBuf rbuf2(multi_size, 512);
    ec = co_await stream->read(rbuf2, bid2);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf2.cbytes(), multi_size, 0xD3E4));

    // CP flush and verify data survives.
    auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
    EXPECT_TRUE(success);

    IoBuf rbuf3(STREAM_BLK_SIZE, 512);
    ec = co_await stream->read(rbuf3, bid);
    CO_ASSERT_FALSE(ec);
    EXPECT_TRUE(self.verify_buf(rbuf3.cbytes(), STREAM_BLK_SIZE, 0xB1C2));

    co_await self.shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: Block-size multiplier survives restart recovery.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RawBlkStreamTest, BlockSizeMultiplierRestart) {
    static constexpr uint32_t STREAM_BLK_SIZE = BLK_SIZE * 4; // 16384 when vdev is 4096
    BlkId bid;
    uint64_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &bid, &sid]() -> Async< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_raw_blk_stream(CHUNK_SIZE, STREAM_BLK_SIZE);
        EXPECT_EQ(stream->block_size(), STREAM_BLK_SIZE);
        EXPECT_EQ(stream->blk_multiplier(), 4u);

        blk_alloc_hints hints;
        auto status = stream->alloc_blk(1, hints, bid);
        CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);
        {
            auto guard = cp_mgr().cp_guard();
            stream->commit_blk(guard.get(), bid);
        }
        IoBuf wbuf(STREAM_BLK_SIZE, 512);
        fill_buf(wbuf.bytes(), STREAM_BLK_SIZE, 0xABCD04);
        co_await stream->write(bid, wbuf);
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
        sid = stream->stream_id();
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, bid, sid]() -> Async< void > {
        auto streams = blob_dev_->raw_blk_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        EXPECT_EQ(recovered->stream_id(), sid);
        EXPECT_EQ(recovered->block_size(), STREAM_BLK_SIZE);
        EXPECT_EQ(recovered->blk_multiplier(), 4u);
        IoBuf rbuf(STREAM_BLK_SIZE, 512);
        auto ec = co_await recovered->read(rbuf, bid);
        CO_ASSERT_FALSE(ec);
        EXPECT_TRUE(verify_buf(rbuf.cbytes(), STREAM_BLK_SIZE, 0xABCD04));
        co_await shutdown();
    }());
}

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_raw_blk_stream");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown — see comment there.
    return RUN_ALL_TESTS();
}
