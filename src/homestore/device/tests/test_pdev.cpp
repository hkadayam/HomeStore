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
 ***************************************************************************/
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <boost/uuid/random_generator.hpp>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "iomanager/iomanager.h"

#include "homestore/base/test_defs.h"
#include "homestore/device/physical_dev.h"
#include "homestore/device/chunk.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;

static constexpr uint64_t DEV_SIZE = 128 * 1024 * 1024; // 128 MB
static constexpr uint32_t BLK_SIZE = 4096;
static constexpr int OFLAGS = O_RDWR | O_CREAT;

// ── Fixture ──────────────────────────────────────────────────────────────────────────────────────────────────────────
class PDevTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_pdev_{}", i);
            dev_paths_.push_back(path);
            // Create a file of DEV_SIZE bytes.
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
        // Uncache any device fds left over (e.g. from standalone read_first_block calls) before deleting files,
        // otherwise the next test's SetUp recreates the path but open_and_cache_dev returns a stale fd.
        iomgr().spawn_and_block(ReactorTarget::any(), [this]() -> folly::coro::Task< void > {
            for (auto& p : dev_paths_) {
                co_await close_and_uncache_dev(p);
            }
        }());
        for (auto& p : dev_paths_) {
            std::filesystem::remove(p);
        }
    }

    DevInfo make_dev_info(size_t idx) const { return DevInfo{dev_paths_[idx], HSDevType::Data, DEV_SIZE}; }

    /// Build a default FirstBlockHeader suitable for single-pdev tests.
    static FirstBlockHeader make_first_blk_hdr() {
        FirstBlockHeader hdr{};
        hdr.gen_number = 1;
        hdr.version = FirstBlockHeader::CURRENT_SUPERBLOCK_VERSION;
        std::strncpy(hdr.product_name, FirstBlockHeader::PRODUCT_NAME, FirstBlockHeader::s_product_name_size - 1);
        hdr.product_name[FirstBlockHeader::s_product_name_size - 1] = '\0';
        hdr.num_pdevs = 1;
        hdr.max_vdevs = HSSuperBlk::MAX_VDEVS_IN_SYSTEM;
        hdr.max_system_chunks = HSSuperBlk::MAX_CHUNKS_IN_SYSTEM;
        hdr.system_uuid = boost::uuids::random_generator{}();
        return hdr;
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
    FirstBlockHeader fbhdr_{make_first_blk_hdr()};
};

// ── CreateAndVerifyInfo ──────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, CreateAndVerifyInfo) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    CO_ASSERT_NE(pdev, nullptr);
    EXPECT_EQ(pdev->pdev_id(), 0u);
    EXPECT_GT(pdev->data_start_offset(), 0u);
    EXPECT_GT(pdev->data_size(), 0u);
    EXPECT_LE(pdev->data_start_offset() + pdev->data_size(), DEV_SIZE);
    EXPECT_GT(pdev->align_size(), 0u);
    EXPECT_GT(pdev->optimal_page_size(), 0u);
    co_await pdev->close_device();
}

// ── ReadFirstBlock ───────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, ReadFirstBlock) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    co_await pdev->close_device();

    auto fb = co_await PhysicalDev::read_first_block(self.dev_paths_[0], OFLAGS);
    EXPECT_EQ(fb.magic, FirstBlock::HOMESTORE_MAGIC);
    EXPECT_STREQ(fb.hdr.product_name, FirstBlockHeader::PRODUCT_NAME);
    EXPECT_EQ(fb.this_pdev_hdr.pdev_id, 0u);
}

// ── LoadRecovery ─────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, LoadRecovery) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    auto orig_data_offset = pdev->data_start_offset();
    auto orig_data_size = pdev->data_size();
    co_await pdev->commit_formatting();
    co_await pdev->close_device();
    pdev.reset();

    auto loaded = co_await PhysicalDev::load(self.make_dev_info(0), OFLAGS, self.fbhdr_);
    CO_ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->pdev_id(), 0u);
    EXPECT_EQ(loaded->data_start_offset(), orig_data_offset);
    EXPECT_EQ(loaded->data_size(), orig_data_size);
    co_await loaded->close_device();
}

// ── ChunkCreateSingle ────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, ChunkCreateSingle) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    co_await pdev->format_chunks();

    constexpr uint64_t chunk_size = 16 * 1024 * 1024; // 16 MB
    auto chunk = co_await pdev->create_chunk(/*vdev_id=*/1, chunk_size, /*ordinal=*/0);
    CO_ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->vdev_id(), 1u);
    EXPECT_EQ(chunk->size(), chunk_size);
    EXPECT_EQ(chunk->vdev_order(), 0u);
    EXPECT_TRUE(chunk->is_busy());

    co_await pdev->close_device();
}

// ── ChunkCreateBatch ─────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, ChunkCreateBatch) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    co_await pdev->format_chunks();

    constexpr uint64_t chunk_size = 16 * 1024 * 1024;
    auto chunks = co_await pdev->create_chunks(/*vdev_id=*/1, /*num_chunks=*/4, chunk_size, /*start_ordinal=*/0);
    CO_ASSERT_EQ(chunks.size(), 4u);

    std::unordered_set< uint64_t > offsets;
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(chunks[i]->vdev_order(), i);
        EXPECT_EQ(chunks[i]->size(), chunk_size);
        offsets.insert(chunks[i]->start_offset());
    }
    // All offsets must be unique (no overlapping).
    EXPECT_EQ(offsets.size(), 4u);

    co_await pdev->close_device();
}

// ── ChunkRemoveAndRecreate ───────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, ChunkRemoveAndRecreate) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    co_await pdev->format_chunks();

    constexpr uint64_t chunk_size = 16 * 1024 * 1024;
    auto chunks = co_await pdev->create_chunks(/*vdev_id=*/1, 4, chunk_size);
    CO_ASSERT_EQ(chunks.size(), 4u);

    // Remove chunks 0 and 2.
    co_await pdev->remove_chunk(chunks[0]);
    co_await pdev->remove_chunk(chunks[2]);

    // Create 2 smaller chunks in the freed space.
    constexpr uint64_t small_size = 8 * 1024 * 1024;
    auto c1 = co_await pdev->create_chunk(/*vdev_id=*/2, small_size, /*ordinal=*/0);
    auto c2 = co_await pdev->create_chunk(/*vdev_id=*/2, small_size, /*ordinal=*/1);
    CO_ASSERT_NE(c1, nullptr);
    CO_ASSERT_NE(c2, nullptr);
    EXPECT_NE(c1->start_offset(), c2->start_offset());

    co_await pdev->close_device();
}

// ── ChunkLoadAfterRestart ────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, ChunkLoadAfterRestart) {
    constexpr uint64_t chunk_size = 16 * 1024 * 1024;

    // Phase 1: create pdev + chunks.
    {
        auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
        co_await pdev->format_chunks();
        auto chunks = co_await pdev->create_chunks(/*vdev_id=*/1, 3, chunk_size);
        CO_ASSERT_EQ(chunks.size(), 3u);
        co_await pdev->commit_formatting();
        co_await pdev->close_device();
    }

    // Phase 2: reload and verify.
    {
        auto pdev = co_await PhysicalDev::load(self.make_dev_info(0), OFLAGS, self.fbhdr_);
        auto vdev_chunks = co_await pdev->load_chunks();
        // All 3 chunks belong to vdev_id=1.
        CO_ASSERT_TRUE(vdev_chunks.count(1) > 0);
        EXPECT_EQ(vdev_chunks[1].size(), 3u);
        co_await pdev->close_device();
    }
}

// ── ChunkDeactivateReactivate ────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, ChunkDeactivateReactivate) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    co_await pdev->format_chunks();

    constexpr uint64_t chunk_size = 16 * 1024 * 1024;
    auto chunk = co_await pdev->create_chunk(/*vdev_id=*/1, chunk_size, /*ordinal=*/0);
    CO_ASSERT_TRUE(chunk->is_busy());

    co_await pdev->deactivate_chunk(chunk);
    EXPECT_FALSE(chunk->is_busy());

    co_await pdev->reactivate_chunk(chunk, /*new_vdev_order=*/42);
    EXPECT_TRUE(chunk->is_busy());
    EXPECT_EQ(chunk->vdev_order(), 42u);

    co_await pdev->close_device();
}

// ── WriteReadSingleBlock ─────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, WriteReadSingleBlock) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);

    IoBuf wbuf{BLK_SIZE, 512};
    std::memset(wbuf.bytes(), 0x42, BLK_SIZE);
    uint64_t offset = pdev->data_start_offset();

    co_await pdev->write(wbuf, offset);

    IoBuf rbuf{BLK_SIZE, 512};
    auto ec = co_await pdev->read(rbuf, offset);
    CO_ASSERT_FALSE(ec);
    EXPECT_EQ(std::memcmp(wbuf.bytes(), rbuf.bytes(), BLK_SIZE), 0);

    co_await pdev->close_device();
}

// ── WriteReadLargeBuffer ─────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, WriteReadLargeBuffer) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);

    constexpr uint32_t large_size = 256 * 1024; // 256 KB
    IoBuf wbuf{large_size, 512};
    for (uint32_t i = 0; i < large_size; ++i) {
        wbuf.bytes()[i] = to_u8(i & 0xFF);
    }
    uint64_t offset = pdev->data_start_offset();

    co_await pdev->write(wbuf, offset);

    IoBuf rbuf{large_size, 512};
    auto ec = co_await pdev->read(rbuf, offset);
    CO_ASSERT_FALSE(ec);
    EXPECT_EQ(std::memcmp(wbuf.bytes(), rbuf.bytes(), large_size), 0);

    co_await pdev->close_device();
}

// ── WritevReadv ──────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, WritevReadv) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);

    constexpr uint32_t num_bufs = 4;
    std::vector< IoBuf > wbufs;
    wbufs.reserve(num_bufs);
    for (uint32_t i = 0; i < num_bufs; ++i) {
        wbufs.emplace_back(BLK_SIZE, 512);
        std::memset(wbufs.back().bytes(), static_cast< int >(0xA0 + i), BLK_SIZE);
    }

    uint64_t offset = pdev->data_start_offset();
    // writev takes rvalue ref to vector.
    std::vector< IoBuf > wbufs_copy;
    wbufs_copy.reserve(num_bufs);
    for (auto& wb : wbufs) {
        IoBuf copy{BLK_SIZE, 512};
        std::memcpy(copy.bytes(), wb.bytes(), BLK_SIZE);
        wbufs_copy.push_back(std::move(copy));
    }
    co_await pdev->writev(std::move(wbufs_copy), offset);

    std::vector< IoBuf > rbufs;
    rbufs.reserve(num_bufs);
    for (uint32_t i = 0; i < num_bufs; ++i) {
        rbufs.emplace_back(BLK_SIZE, 512);
    }
    auto ec = co_await pdev->readv(rbufs, offset);
    CO_ASSERT_FALSE(ec);

    for (uint32_t i = 0; i < num_bufs; ++i) {
        EXPECT_EQ(std::memcmp(wbufs[i].bytes(), rbufs[i].bytes(), BLK_SIZE), 0) << "buffer " << i << " mismatch";
    }

    co_await pdev->close_device();
}

// ── Fsync ────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, Fsync) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);

    IoBuf wbuf{BLK_SIZE, 512};
    std::memset(wbuf.bytes(), 0xBB, BLK_SIZE);
    uint64_t offset = pdev->data_start_offset();
    co_await pdev->write(wbuf, offset);
    co_await pdev->fsync();

    IoBuf rbuf{BLK_SIZE, 512};
    auto ec = co_await pdev->read(rbuf, offset);
    CO_ASSERT_FALSE(ec);
    EXPECT_EQ(std::memcmp(wbuf.bytes(), rbuf.bytes(), BLK_SIZE), 0);

    co_await pdev->close_device();
}

// ── RandomChunkOps ───────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(PDevTest, RandomChunkOps) {
    auto pdev = co_await PhysicalDev::create(self.make_dev_info(0), OFLAGS, /*pdev_id=*/0, self.fbhdr_);
    co_await pdev->format_chunks();

    constexpr uint64_t chunk_size = 16 * 1024 * 1024;
    constexpr int NUM_ITERS = 200;
    const uint32_t max_chunks = pdev->data_size() / chunk_size;
    std::mt19937 rng{42};
    std::vector< shared< Chunk > > live_chunks;

    for (int iter = 0; iter < NUM_ITERS; ++iter) {
        bool do_create = live_chunks.empty() || (live_chunks.size() < max_chunks && (rng() % 3 != 0));
        if (do_create) {
            auto chunk = co_await pdev->create_chunk(/*vdev_id=*/1, chunk_size, /*ordinal=*/0);
            if (chunk) {
                live_chunks.push_back(std::move(chunk));
            }
        } else {
            // Remove a random chunk.
            std::uniform_int_distribution< size_t > dist(0, live_chunks.size() - 1);
            auto idx = dist(rng);
            co_await pdev->remove_chunk(live_chunks[idx]);
            live_chunks.erase(live_chunks.begin() + static_cast< ptrdiff_t >(idx));
        }
    }

    // Verify: reload and count chunks.
    co_await pdev->commit_formatting();
    co_await pdev->close_device();
    pdev.reset();

    auto reloaded = co_await PhysicalDev::load(self.make_dev_info(0), OFLAGS, self.fbhdr_);
    auto vdev_chunks = co_await reloaded->load_chunks();
    size_t total_loaded = 0;
    for (auto& [vid, cvec] : vdev_chunks) {
        total_loaded += cvec.size();
    }
    EXPECT_EQ(total_loaded, live_chunks.size());
    co_await reloaded->close_device();
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_pdev");
    ::testing::InitGoogleTest(&argc, argv);
    iomanager::init_iomgr(2);
    int rc = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return rc;
}
