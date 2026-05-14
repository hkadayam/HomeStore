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
#include <random>
#include <string>
#include <thread>
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

#include "homestore/blob/append_byte_stream.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"

using namespace homestore;
using namespace iomanager;
using sisl::IOBuffer;

SISL_OPTION_GROUP(test_append_byte_stream,
                  (num_io, "", "num_io", "number of IO operations per test",
                   ::cxxopts::value< uint32_t >()->default_value("100"), "number"));

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;      // 256 MB per device
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev
static constexpr uint64_t CHUNK_SIZE = 8 * 1024 * 1024;      // 8 MB per chunk
static constexpr uint32_t BLK_SIZE = 4096;

class AppendByteStreamTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test fresh reactors: CPManager's per-thread cache (cp_mgr.cpp's t_cp_info_) is a thread_local
        // pointer into the manager's owned_stacks_; the prior test's CPManager is gone but its stack info bytes
        // were freed, so the cache would dangle.  Cycling iomgr kills the reactor threads (and their TLS) so
        // the next test's first cp_guard() sees a fresh, empty cache.
        iomanager::init_iomgr(2);
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_append_byte_stream_{}", i);
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
        blob_dev_ = co_await blob_dev_mgr().create_blob_dev("test_blob_dev"s, std::move(params));
    }

    // Drive a full reload from the main test thread.  Cycling iomgr (kills reactor TLS, including CPManager's
    // cached ThreadStackInfo pointer that would otherwise dangle into the freed CPManager) requires the main
    // thread because stop_iomgr joins reactor threads — a reactor calling it would self-join.  Two coroutine
    // phases bracket the cycle: one to tear down on the old reactor pool, one to bring up on the new pool.
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
            blob_dev_ = blob_dev_mgr().get_blob_dev("test_blob_dev");
        }());
    }

    folly::coro::Task< void > shutdown() {
        blob_dev_.reset();
        co_await cp_mgr().shutdown();
        blob_dev_mgr().shutdown();
        co_await dm_->close_devices();
    }

    static void fill_pattern(uint8_t* buf, size_t size, uint64_t seed) {
        auto* p = reinterpret_cast< uint64_t* >(buf);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            p[i] = seed ^ i;
        }
        // Fill any unaligned tail with low byte of seed.
        for (size_t i = (size / sizeof(uint64_t)) * sizeof(uint64_t); i < size; ++i) {
            buf[i] = static_cast< uint8_t >(seed + i);
        }
    }

    static bool verify_pattern(const uint8_t* buf, size_t size, uint64_t seed) {
        auto* p = reinterpret_cast< const uint64_t* >(buf);
        for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
            if (p[i] != (seed ^ i)) {
                return false;
            }
        }
        for (size_t i = (size / sizeof(uint64_t)) * sizeof(uint64_t); i < size; ++i) {
            if (buf[i] != static_cast< uint8_t >(seed + i)) {
                return false;
            }
        }
        return true;
    }

    // Append `size` bytes filled with `seed` pattern.  Returns the offset where the data was written.
    static uint64_t append_pattern(AppendByteStream& s, size_t size, uint64_t seed) {
        std::vector< uint8_t > buf(size);
        fill_pattern(buf.data(), size, seed);
        return s.append(sisl::Blob{buf.data(), to_u32(buf.size())});
    }

    // Read [offset, offset+size) and verify it matches the seed pattern.  AppendByteStream::read() returns a
    // ByteView pre-sliced to start at byte_offset — index from 0.
    static folly::coro::Task< bool > verify_at(AppendByteStream& s, uint64_t offset, size_t size, uint64_t seed) {
        auto [ec, view] = co_await s.read(offset, size);
        if (ec) {
            co_return false;
        }
        co_return verify_pattern(view.bytes(), size, seed);
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    shared< DeviceManager > dm_;
    shared< BlobDev > blob_dev_;
};

// ── Create / multi-stream basics ─────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendByteStreamTest, CreateStream) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);
    CO_ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->num_chunks(), 1u);
    EXPECT_EQ(stream->chunk_size(), CHUNK_SIZE);
    EXPECT_EQ(stream->block_size(), BLK_SIZE);
    EXPECT_EQ(stream->tail_offset(), 0u);

    auto streams = self.blob_dev_->append_byte_streams();
    EXPECT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0]->stream_id(), stream->stream_id());

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, MultipleStreams) {
    co_await self.bootstrap();

    auto s1 = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);
    auto s2 = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);
    auto s3 = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    EXPECT_NE(s1->stream_id(), s2->stream_id());
    EXPECT_NE(s2->stream_id(), s3->stream_id());
    EXPECT_NE(s1->stream_id(), s3->stream_id());
    EXPECT_EQ(self.blob_dev_->append_byte_streams().size(), 3u);

    co_await self.shutdown();
}

// ── Append + read ────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendByteStreamTest, SingleAppendRead) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    constexpr size_t N = 1024;
    auto off = self.append_pattern(*stream, N, 0xDEAD);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(stream->tail_offset(), N);

    co_await stream->flush();

    EXPECT_TRUE(co_await self.verify_at(*stream, 0, N, 0xDEAD));

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, MultipleAppendsContiguousOffsets) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    struct Rec {
        uint64_t off;
        size_t size;
        uint64_t seed;
    };
    std::vector< Rec > recs;

    // Interleave appends and flushes to exercise the flush boundary.
    uint64_t expected_off = 0;
    for (uint32_t i = 0; i < 20; ++i) {
        size_t sz = 200 + (i * 37);
        auto off = self.append_pattern(*stream, sz, 0xA000 + i);
        EXPECT_EQ(off, expected_off);
        recs.push_back({off, sz, 0xA000 + i});
        expected_off += sz;
        if ((i % 5) == 4) {
            co_await stream->flush();
        }
    }
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), expected_off);

    for (auto const& r : recs) {
        EXPECT_TRUE(co_await self.verify_at(*stream, r.off, r.size, r.seed))
            << "mismatch at off=" << r.off << " size=" << r.size;
    }

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, AppendLargerThanBlock) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    // 5 blocks worth of data in a single append.
    constexpr size_t N = 5 * BLK_SIZE;
    self.append_pattern(*stream, N, 0xB100);
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), N);
    EXPECT_TRUE(co_await self.verify_at(*stream, 0, N, 0xB100));

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, AppendSpanningChunks) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);
    EXPECT_EQ(stream->num_chunks(), 1u);

    // Fill the first chunk plus some of the second chunk.
    size_t const N = CHUNK_SIZE + (256 * 1024);
    self.append_pattern(*stream, N, 0xC200);
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), N);
    EXPECT_GE(stream->num_chunks(), 2u);

    EXPECT_TRUE(co_await self.verify_at(*stream, 0, N, 0xC200));

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, PartialBlockTailCarriesAcrossFlush) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    // First flush ends mid-block.
    constexpr size_t a = BLK_SIZE + 100;
    self.append_pattern(*stream, a, 0xD300);
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), a);

    // Next append starts where the previous left off; tail_block_ should bridge the partial block.
    constexpr size_t b = 2 * BLK_SIZE + 50;
    auto off_b = self.append_pattern(*stream, b, 0xD301);
    EXPECT_EQ(off_b, a);
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), a + b);

    EXPECT_TRUE(co_await self.verify_at(*stream, 0, a, 0xD300));
    EXPECT_TRUE(co_await self.verify_at(*stream, a, b, 0xD301));

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, ReadAcrossChunkBoundary) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    // Three back-to-back records: head fully in chunk 0, mid record fits exactly in remainder of chunk 0, tail
    // record fully in chunk 1.  Verifies each record stays addressable through read() once the stream has expanded.
    self.append_pattern(*stream, CHUNK_SIZE - 4096, 0xE400);
    self.append_pattern(*stream, 4096, 0xE401);
    self.append_pattern(*stream, 8192, 0xE402);
    co_await stream->flush();
    EXPECT_GE(stream->num_chunks(), 2u);

    EXPECT_TRUE(co_await self.verify_at(*stream, 0, CHUNK_SIZE - 4096, 0xE400));
    EXPECT_TRUE(co_await self.verify_at(*stream, CHUNK_SIZE - 4096, 4096, 0xE401));
    EXPECT_TRUE(co_await self.verify_at(*stream, CHUNK_SIZE, 8192, 0xE402));

    co_await self.shutdown();
}

// ── Read cursor ──────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendByteStreamTest, ReadCursorSequential) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    // Append several records.
    constexpr uint32_t kRecords = 16;
    constexpr size_t kRecSize = 8192;
    for (uint32_t i = 0; i < kRecords; ++i) {
        self.append_pattern(*stream, kRecSize, 0xF500 + i);
    }
    co_await stream->flush();

    auto cursor = stream->open_cursor();
    EXPECT_TRUE(cursor.has_more());
    EXPECT_EQ(cursor.position(), 0u);
    EXPECT_EQ(cursor.remaining(), kRecords * kRecSize);

    uint32_t i = 0;
    uint64_t pos = 0;
    while (cursor.has_more()) {
        // ReadCursor::next now returns a ByteView pre-sliced to start at pos — index from 0.
        auto [view, sz] = co_await cursor.next(kRecSize);
        EXPECT_EQ(sz, kRecSize);
        EXPECT_TRUE(self.verify_pattern(view.bytes(), sz, 0xF500 + i));
        ++i;
        pos += sz;
    }
    EXPECT_EQ(i, kRecords);
    EXPECT_EQ(pos, kRecords * kRecSize);

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, ReadCursorRange) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);
    constexpr size_t kSize = 64 * 1024;
    self.append_pattern(*stream, kSize, 0x1601);
    co_await stream->flush();

    constexpr uint64_t start = 8192;
    constexpr uint64_t end = 32 * 1024;
    auto cursor = stream->open_cursor(start, end);
    EXPECT_EQ(cursor.position(), start);
    EXPECT_EQ(cursor.remaining(), end - start);

    uint64_t total = 0;
    while (cursor.has_more()) {
        auto [iobuf, sz] = co_await cursor.next(4096);
        total += sz;
    }
    EXPECT_EQ(total, end - start);

    co_await self.shutdown();
}

// ── Truncate ─────────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendByteStreamTest, TruncateKeepsChunks) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    self.append_pattern(*stream, 32 * 1024, 0x1701);
    co_await stream->flush();
    EXPECT_GT(stream->tail_offset(), 0u);

    // Truncate up to tail — head==tail triggers the fresh-start reset: all-but-one chunk released (one kept as
    // anchor so the stream is immediately usable for new appends and the sb has a chunk to reference), positions
    // back to 0.
    co_await stream->truncate(stream->tail_offset());
    EXPECT_EQ(stream->tail_offset(), 0u);
    EXPECT_EQ(stream->num_chunks(), 1u);

    // Subsequent appends start from offset 0 (fresh stream).
    auto off = self.append_pattern(*stream, 1024, 0x1702);
    EXPECT_EQ(off, 0u);
    co_await stream->flush();
    EXPECT_TRUE(co_await self.verify_at(*stream, 0, 1024, 0x1702));

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, TruncateReleasesChunks) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);

    // Force expansion across multiple chunks first.
    self.append_pattern(*stream, CHUNK_SIZE + 1024, 0x1801);
    co_await stream->flush();
    EXPECT_GE(stream->num_chunks(), 2u);

    co_await stream->truncate(stream->tail_offset());
    EXPECT_EQ(stream->tail_offset(), 0u);
    // After release, the stream may keep one base chunk or zero — implementation-defined.
    // We only require the count to be strictly less than before.
    EXPECT_LE(stream->num_chunks(), 1u);

    co_await self.shutdown();
}

// ── Flush edge cases ─────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendByteStreamTest, FlushEmptyIsNoOp) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE);
    auto off_before = stream->tail_offset();
    co_await stream->flush(); // nothing buffered
    EXPECT_EQ(stream->tail_offset(), off_before);

    // Two back-to-back empty flushes still safe.
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), off_before);

    co_await self.shutdown();
}

// ── Concurrency ──────────────────────────────────────────────────────────────────────────────────────────────────────

CORO_TEST_F(AppendByteStreamTest, ConcurrentAppendsSerialized) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE, true /* concurrent_safe */);

    constexpr uint32_t kThreads = 8;
    constexpr uint32_t kPerThread = 200;
    constexpr size_t kRecSize = 256;

    std::vector< std::thread > workers;
    std::atomic< uint64_t > total_appended{0};
    for (uint32_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            for (uint32_t i = 0; i < kPerThread; ++i) {
                std::vector< uint8_t > buf(kRecSize);
                std::memset(buf.data(), static_cast< uint8_t >(t), kRecSize);
                stream->append(sisl::Blob{buf.data(), to_u32(buf.size())});
                total_appended.fetch_add(kRecSize, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    co_await stream->flush();

    // Each append is atomic w.r.t. tail_offset_ updates — total bytes must be exact.
    EXPECT_EQ(stream->tail_offset(), total_appended.load());

    co_await self.shutdown();
}

CORO_TEST_F(AppendByteStreamTest, ConcurrentSafeOff) {
    co_await self.bootstrap();

    auto stream = co_await self.blob_dev_->create_append_byte_stream(CHUNK_SIZE, false /* not concurrent-safe */);

    // Single-threaded usage path.
    for (uint32_t i = 0; i < 50; ++i) {
        self.append_pattern(*stream, 512, 0x1900 + i);
    }
    co_await stream->flush();
    EXPECT_EQ(stream->tail_offset(), 50u * 512);
    EXPECT_TRUE(co_await self.verify_at(*stream, 0, 512, 0x1900));
    EXPECT_TRUE(co_await self.verify_at(*stream, 49 * 512, 512, 0x1900 + 49));

    co_await self.shutdown();
}

// ── Restart recovery ─────────────────────────────────────────────────────────────────────────────────────────────────

TEST_F(AppendByteStreamTest, RestartRecoverySingleChunk) {
    constexpr size_t N = 16 * 1024;
    uint64_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> folly::coro::Task< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_append_byte_stream(CHUNK_SIZE);
        sid = stream->stream_id();
        append_pattern(*stream, N, 0x2A01);
        co_await stream->flush();
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, N]() -> folly::coro::Task< void > {
        auto streams = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        EXPECT_EQ(recovered->stream_id(), sid);
        EXPECT_EQ(recovered->tail_offset(), N);
        EXPECT_TRUE(co_await verify_at(*recovered, 0, N, 0x2A01));
        co_await shutdown();
    }());
}

TEST_F(AppendByteStreamTest, RestartRecoveryMultiChunk) {
    struct Seg {
        uint64_t off;
        size_t size;
        uint64_t seed;
    };
    constexpr size_t kSeg = 256 * 1024;
    constexpr uint32_t kCount = (CHUNK_SIZE * 2 + kSeg - 1) / kSeg;
    std::vector< Seg > segs;
    uint64_t sid{};
    uint64_t off = 0;

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, &sid, &off, &segs]() -> folly::coro::Task< void > {
                                co_await bootstrap();
                                auto stream = co_await blob_dev_->create_append_byte_stream(CHUNK_SIZE);
                                sid = stream->stream_id();
                                for (uint32_t i = 0; i < kCount; ++i) {
                                    append_pattern(*stream, kSeg, 0x2B00 + i);
                                    segs.push_back({off, kSeg, 0x2B00 + i});
                                    off += kSeg;
                                }
                                co_await stream->flush();
                                auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
                                CO_ASSERT_TRUE(success);
                                EXPECT_GE(stream->num_chunks(), 2u);
                            }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, sid, off, &segs]() -> folly::coro::Task< void > {
                                auto streams = blob_dev_->append_byte_streams();
                                CO_ASSERT_EQ(streams.size(), 1u);
                                auto recovered = streams[0];
                                EXPECT_EQ(recovered->stream_id(), sid);
                                EXPECT_EQ(recovered->tail_offset(), off);
                                for (auto const& s : segs) {
                                    EXPECT_TRUE(co_await verify_at(*recovered, s.off, s.size, s.seed))
                                        << "mismatch after restart at off=" << s.off;
                                }
                                co_await shutdown();
                            }());
}

TEST_F(AppendByteStreamTest, RestartWithPartialTailBlock) {
    // Non-block-aligned write sizes so tail crosses partial blocks.
    constexpr size_t N = 2 * BLK_SIZE + 137;
    constexpr size_t M = BLK_SIZE + 7;
    uint64_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> folly::coro::Task< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_append_byte_stream(CHUNK_SIZE);
        sid = stream->stream_id();
        append_pattern(*stream, N, 0x2C01);
        co_await stream->flush();
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, N, M]() -> folly::coro::Task< void > {
        auto streams = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        EXPECT_EQ(recovered->stream_id(), sid);
        EXPECT_EQ(recovered->tail_offset(), N);
        EXPECT_TRUE(co_await verify_at(*recovered, 0, N, 0x2C01));
        // Subsequent append starts exactly at N — tail_block_ primed from disk so bytes stay contiguous.
        auto off = append_pattern(*recovered, M, 0x2C02);
        EXPECT_EQ(off, N);
        co_await recovered->flush();
        EXPECT_TRUE(co_await verify_at(*recovered, N, M, 0x2C02));
        co_await shutdown();
    }());
}

TEST_F(AppendByteStreamTest, RestartAfterTruncate) {
    uint64_t sid{};

    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid]() -> folly::coro::Task< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_append_byte_stream(CHUNK_SIZE);
        sid = stream->stream_id();
        append_pattern(*stream, 4096, 0x2D01);
        co_await stream->flush();
        co_await stream->truncate(stream->tail_offset());
        auto success = co_await cp_mgr().trigger_cp_flush(true /* force */);
        CO_ASSERT_TRUE(success);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid]() -> folly::coro::Task< void > {
        auto streams = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(streams.size(), 1u);
        auto recovered = streams[0];
        EXPECT_EQ(recovered->stream_id(), sid);
        EXPECT_EQ(recovered->tail_offset(), 0u);
        co_await shutdown();
    }());
}

TEST_F(AppendByteStreamTest, DoubleRestart) {
    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_append_byte_stream(CHUNK_SIZE);
        append_pattern(*stream, 4096, 0x2E01);
        co_await stream->flush();
        co_await cp_mgr().trigger_cp_flush(true);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
        auto recovered = blob_dev_->append_byte_streams().at(0);
        EXPECT_EQ(recovered->tail_offset(), 4096u);
        append_pattern(*recovered, 4096, 0x2E02);
        co_await recovered->flush();
        co_await cp_mgr().trigger_cp_flush(true);
    }());

    reload_sync();

    iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
        auto recovered2 = blob_dev_->append_byte_streams().at(0);
        EXPECT_EQ(recovered2->tail_offset(), 8192u);
        EXPECT_TRUE(co_await verify_at(*recovered2, 0, 4096, 0x2E01));
        EXPECT_TRUE(co_await verify_at(*recovered2, 4096, 4096, 0x2E02));
        co_await shutdown();
    }());
}

// Extended lifecycle: empty restart → partial + cross-chunk writes → mid-chunk truncate → restart → further truncate
// + append → restart → full truncate (keeps 1 chunk anchor) → restart → append again.
TEST_F(AppendByteStreamTest, LifecycleExtended) {
    constexpr uint64_t seed_a = 0x3A00;
    constexpr uint64_t seed_b = 0x3B00;
    constexpr uint64_t seed_c = 0x3C00;
    constexpr uint64_t seed_d = 0x3D00;
    constexpr uint64_t seed_e = 0x3E00;
    uint64_t sid{};
    uint64_t CS = 0; // VirtualDev bumps chunk_size to MIN_CHUNK_SIZE; captured from the first created stream.
    uint64_t upto_1_6 = 0;

    // ── Phase 1: bootstrap → create empty stream → flush + cp_flush → (reload boundary). ────────────────────────
    iomgr().spawn_and_block(ReactorTarget::any(), [this, &sid, &CS]() -> folly::coro::Task< void > {
        co_await bootstrap();
        auto stream = co_await blob_dev_->create_append_byte_stream(CHUNK_SIZE);
        CS = stream->chunk_size();
        sid = stream->stream_id();
        co_await stream->flush();
        CO_ASSERT_TRUE(co_await cp_mgr().trigger_cp_flush(true /* force */));
    }());

    reload_sync();

    // ── Phase 2: verify empty after restart → append seed_a + seed_b → cp_flush → (reload boundary). ───────────
    iomgr().spawn_and_block(ReactorTarget::any(), [this, sid, CS]() -> folly::coro::Task< void > {
        auto ss = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(ss.size(), 1u);
        auto stream = ss[0];
        EXPECT_EQ(stream->stream_id(), sid);
        EXPECT_EQ(stream->head_offset(), 0u);
        EXPECT_EQ(stream->tail_offset(), 0u);

        append_pattern(*stream, CS / 2, seed_a);
        co_await stream->flush();
        EXPECT_EQ(stream->tail_offset(), CS / 2);

        append_pattern(*stream, CS, seed_b);
        co_await stream->flush();
        EXPECT_EQ(stream->tail_offset(), CS + CS / 2);
        EXPECT_GE(stream->num_chunks(), 2u);

        CO_ASSERT_TRUE(co_await cp_mgr().trigger_cp_flush(true /* force */));
    }());

    reload_sync();

    // ── Phase 3: verify a/b after restart → append seed_c + head-truncate to CS/4 → cp_flush → (reload). ───────
    iomgr().spawn_and_block(ReactorTarget::any(), [this, CS]() -> folly::coro::Task< void > {
        auto ss = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(ss.size(), 1u);
        auto stream = ss[0];
        EXPECT_EQ(stream->head_offset(), 0u);
        EXPECT_EQ(stream->tail_offset(), CS + CS / 2);
        EXPECT_TRUE(co_await verify_at(*stream, 0, CS / 2, seed_a));
        EXPECT_TRUE(co_await verify_at(*stream, CS / 2, CS, seed_b));

        append_pattern(*stream, CS / 4, seed_c);
        co_await stream->flush();
        EXPECT_EQ(stream->tail_offset(), CS + CS * 3 / 4); // 1.75C
        co_await stream->truncate(CS / 4);
        EXPECT_EQ(stream->head_offset(), CS / 4);
        EXPECT_EQ(stream->tail_offset(), CS + CS * 3 / 4);

        CO_ASSERT_TRUE(co_await cp_mgr().trigger_cp_flush(true /* force */));
    }());

    reload_sync();

    // ── Phase 4: verify b/c after restart → head-truncate to 1.6C (releases chunk) → append seed_d → (reload). ─
    iomgr().spawn_and_block(ReactorTarget::any(),
                            [this, CS, &upto_1_6]() -> folly::coro::Task< void > {
                                auto ss = blob_dev_->append_byte_streams();
                                CO_ASSERT_EQ(ss.size(), 1u);
                                auto stream = ss[0];
                                EXPECT_EQ(stream->head_offset(), CS / 4);
                                EXPECT_EQ(stream->tail_offset(), CS + CS * 3 / 4);
                                EXPECT_TRUE(co_await verify_at(*stream, CS / 2, CS, seed_b));
                                EXPECT_TRUE(co_await verify_at(*stream, CS + CS / 2, CS / 4, seed_c));

                                upto_1_6 = CS + CS * 6 / 10; // 1.6C
                                co_await stream->truncate(upto_1_6);
                                EXPECT_EQ(stream->head_offset(), upto_1_6);

                                append_pattern(*stream, CS / 10, seed_d);
                                co_await stream->flush();
                                EXPECT_EQ(stream->tail_offset(), CS + CS * 3 / 4 + CS / 10); // 1.85C

                                CO_ASSERT_TRUE(co_await cp_mgr().trigger_cp_flush(true /* force */));
                            }());

    reload_sync();

    // ── Phase 5: verify d after restart → full-truncate (1 chunk anchor) → (reload). ────────────────────────────
    iomgr().spawn_and_block(ReactorTarget::any(), [this, CS, upto_1_6]() -> folly::coro::Task< void > {
        auto ss = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(ss.size(), 1u);
        auto stream = ss[0];
        EXPECT_EQ(stream->head_offset(), upto_1_6);
        EXPECT_EQ(stream->tail_offset(), CS + CS * 3 / 4 + CS / 10);
        EXPECT_TRUE(co_await verify_at(*stream, CS + CS * 3 / 4, CS / 10, seed_d));

        co_await stream->truncate(stream->tail_offset());
        EXPECT_EQ(stream->head_offset(), 0u);
        EXPECT_EQ(stream->tail_offset(), 0u);
        EXPECT_EQ(stream->num_chunks(), 1u);

        CO_ASSERT_TRUE(co_await cp_mgr().trigger_cp_flush(true /* force */));
    }());

    reload_sync();

    // ── Phase 6: verify empty + 1 anchor chunk → append seed_e → verify → shutdown. ─────────────────────────────
    iomgr().spawn_and_block(ReactorTarget::any(), [this, CS]() -> folly::coro::Task< void > {
        auto ss = blob_dev_->append_byte_streams();
        CO_ASSERT_EQ(ss.size(), 1u);
        auto stream = ss[0];
        EXPECT_EQ(stream->head_offset(), 0u);
        EXPECT_EQ(stream->tail_offset(), 0u);
        EXPECT_EQ(stream->num_chunks(), 1u);

        append_pattern(*stream, CS / 2, seed_e);
        co_await stream->flush();
        EXPECT_EQ(stream->tail_offset(), CS / 2);
        EXPECT_TRUE(co_await verify_at(*stream, 0, CS / 2, seed_e));

        co_await shutdown();
    }());
}

int main(int argc, char* argv[]) {
    int parsed_argc = argc;
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv);
    sisl::logging::SetLogger("test_append_byte_stream");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");

    // iomgr is started/stopped per-test in the fixture's SetUp/TearDown — see comment there.
    return RUN_ALL_TESTS();
}