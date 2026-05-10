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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "homestore/base/blk.h"
#include "homestore/base/homestore_config.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/physical_dev.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/device/chunk.h"

using namespace homestore;
using namespace iomanager;
using sisl::IOBuffer;

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024; // 256 MB
static constexpr uint32_t BLK_SIZE = 4096;
static constexpr uint64_t CHUNK_SIZE = 32 * 1024 * 1024; // 32 MB

// ── Fixture ──────────────────────────────────────────────────────────────────────────────────────────────────────────
// Creates 2 temp files, formats them via DeviceManager, then creates VirtualDevs for each test.
class VDevTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_vdev_{}", i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
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

    VDevParameters static_params(const std::string& name, uint32_t num_chunks = 4) const {
        VDevParameters p;
        p.vdev_name = name;
        p.initial_chunk_size = CHUNK_SIZE;
        p.initial_num_chunks = num_chunks;
        p.blk_size = BLK_SIZE;
        p.dev_type = HSDevType::Data;
        p.multi_pdev_opts = MultiPDevOpts::AllPDevStriped;
        p.alloc_type = BlkAllocatorType::SlabCompact;
        p.chunk_sel_type = ChunkSelectorType::RoundRobin;
        return p;
    }

    VDevParameters dynamic_params(const std::string& name) const {
        VDevParameters p;
        p.vdev_name = name;
        p.initial_chunk_size = CHUNK_SIZE;
        p.initial_num_chunks = 0;
        p.blk_size = BLK_SIZE;
        p.dev_type = HSDevType::Data;
        p.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        p.alloc_type = BlkAllocatorType::None;
        p.chunk_sel_type = ChunkSelectorType::RoundRobin;
        return p;
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
};

// ── CreateStaticRoundRobin ───────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, CreateStaticRoundRobin) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_static", 4));
    CO_ASSERT_NE(vdev, nullptr);
    EXPECT_EQ(vdev->block_size(), BLK_SIZE);
    EXPECT_EQ(vdev->num_chunks(), 4u);
    EXPECT_EQ(vdev->name(), "test_static");
    co_await dm->close_devices();
}

// ── CreateDynamic ────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, CreateDynamic) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.dynamic_params("test_dynamic"));
    CO_ASSERT_NE(vdev, nullptr);
    EXPECT_EQ(vdev->num_chunks(), 0u);
    co_await dm->close_devices();
}

// ── FormatZerosData ──────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, FormatZerosData) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_format", 2));
    co_await vdev->format();

    // Read first block from chunk 0 — should be all zeros.
    BlkId bid(0, 1, to_u16(vdev->get_nth_chunk(0)->chunk_id()));
    IOBuffer rbuf{BLK_SIZE, 512};
    auto ec = co_await vdev->read(rbuf, bid);
    CO_ASSERT_FALSE(ec);
    for (uint32_t i = 0; i < BLK_SIZE; ++i) {
        EXPECT_EQ(rbuf.bytes()[i], 0) << "Expected zero at byte " << i;
    }
    co_await dm->close_devices();
}

// ── AllocWriteReadSingleBlock ────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, AllocWriteReadSingleBlock) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_rw", 2));

    BlkId blkid;
    blk_alloc_hints hints;
    auto status = vdev->alloc_contiguous_blks(1, hints, blkid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    IOBuffer wbuf{BLK_SIZE, 512};
    std::memset(wbuf.bytes(), 0xAA, BLK_SIZE);
    co_await vdev->write(wbuf, blkid);

    IOBuffer rbuf{BLK_SIZE, 512};
    auto ec = co_await vdev->read(rbuf, blkid);
    CO_ASSERT_FALSE(ec);
    EXPECT_EQ(std::memcmp(wbuf.bytes(), rbuf.bytes(), BLK_SIZE), 0);

    co_await dm->close_devices();
}

// ── AllocWriteReadMultiBlock ─────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, AllocWriteReadMultiBlock) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_multi", 2));

    constexpr blk_count_t nblks = 4;
    BlkId blkid;
    blk_alloc_hints hints;
    auto status = vdev->alloc_contiguous_blks(nblks, hints, blkid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    uint32_t total_size = nblks * BLK_SIZE;
    IOBuffer wbuf{total_size, 512};
    for (uint32_t i = 0; i < total_size; ++i) {
        wbuf.bytes()[i] = to_u8(i & 0xFF);
    }
    co_await vdev->write(wbuf, blkid);

    IOBuffer rbuf{total_size, 512};
    auto ec = co_await vdev->read(rbuf, blkid);
    CO_ASSERT_FALSE(ec);
    EXPECT_EQ(std::memcmp(wbuf.bytes(), rbuf.bytes(), total_size), 0);

    co_await dm->close_devices();
}

// ── WritevReadv ──────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, WritevReadv) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_writev", 2));

    constexpr blk_count_t nblks = 4;
    BlkId blkid;
    blk_alloc_hints hints;
    auto status = vdev->alloc_contiguous_blks(nblks, hints, blkid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    // Write 4 separate buffers with distinct patterns.
    std::vector< IOBuffer > wbufs;
    wbufs.reserve(nblks);
    for (int i = 0; i < nblks; ++i) {
        wbufs.emplace_back(BLK_SIZE, 512);
        std::memset(wbufs.back().bytes(), 0xC0 + i, BLK_SIZE);
    }

    // Save copies for verification since writev takes rvalue ref.
    std::vector< std::vector< uint8_t > > saved(nblks);
    for (int i = 0; i < nblks; ++i) {
        saved[i].assign(wbufs[i].bytes(), wbufs[i].bytes() + BLK_SIZE);
    }

    co_await vdev->writev(std::move(wbufs), blkid);

    std::vector< IOBuffer > rbufs;
    rbufs.reserve(nblks);
    for (int i = 0; i < nblks; ++i) {
        rbufs.emplace_back(BLK_SIZE, 512);
    }
    auto ec = co_await vdev->readv(rbufs, blkid);
    CO_ASSERT_FALSE(ec);

    for (int i = 0; i < nblks; ++i) {
        EXPECT_EQ(std::memcmp(saved[i].data(), rbufs[i].bytes(), BLK_SIZE), 0) << "buffer " << i << " mismatch";
    }

    co_await dm->close_devices();
}

// ── AllocFreeRealloc ─────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, AllocFreeRealloc) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_free", 2));

    BlkId blkid;
    blk_alloc_hints hints;
    auto status = vdev->alloc_contiguous_blks(1, hints, blkid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    vdev->free_blk(blkid);

    // Re-allocate — should succeed now that space was freed.
    BlkId blkid2;
    status = vdev->alloc_contiguous_blks(1, hints, blkid2);
    EXPECT_EQ(status, BlkAllocStatus::SUCCESS);

    co_await dm->close_devices();
}

// ── ExpandChunk ──────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, ExpandChunk) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.dynamic_params("test_expand"));
    EXPECT_EQ(vdev->num_chunks(), 0u);

    auto chunk = co_await vdev->expand(CHUNK_SIZE);
    CO_ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(vdev->num_chunks(), 1u);

    auto chunk2 = co_await vdev->expand(CHUNK_SIZE);
    CO_ASSERT_NE(chunk2, nullptr);
    EXPECT_EQ(vdev->num_chunks(), 2u);
    EXPECT_NE(chunk->chunk_id(), chunk2->chunk_id());

    co_await dm->close_devices();
}

// ── ShrinkChunkLast ──────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, ShrinkChunkLast) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.dynamic_params("test_shrink_last"));

    co_await vdev->expand(CHUNK_SIZE);
    co_await vdev->expand(CHUNK_SIZE);
    co_await vdev->expand(CHUNK_SIZE);
    EXPECT_EQ(vdev->num_chunks(), 3u);

    auto removed_id = co_await vdev->shrink(ChunkToShrink::Last);
    EXPECT_EQ(vdev->num_chunks(), 2u);
    // The removed chunk should no longer be found.
    EXPECT_EQ(vdev->get_chunk(removed_id), nullptr);

    co_await dm->close_devices();
    co_return;
}

// ── ShrinkChunkSpecific ──────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, ShrinkChunkSpecific) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.dynamic_params("test_shrink_spec"));

    auto c0 = co_await vdev->expand(CHUNK_SIZE);
    auto c1 = co_await vdev->expand(CHUNK_SIZE);
    auto c2 = co_await vdev->expand(CHUNK_SIZE);
    EXPECT_EQ(vdev->num_chunks(), 3u);

    // Remove the middle chunk by ID.
    auto removed_id = co_await vdev->shrink(ChunkToShrink::Specific, c1->chunk_id());
    EXPECT_EQ(removed_id, c1->chunk_id());
    EXPECT_EQ(vdev->num_chunks(), 2u);

    // The other two should still be present.
    EXPECT_NE(vdev->get_chunk(c0->chunk_id()), nullptr);
    EXPECT_NE(vdev->get_chunk(c2->chunk_id()), nullptr);

    co_await dm->close_devices();
    co_return;
}

// ── ChunkPoolReuse ───────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, ChunkPoolReuse) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.dynamic_params("test_pool"));
    vdev->enable_chunk_pooling(/*pool_limit=*/4);

    auto chunk = co_await vdev->expand(CHUNK_SIZE);
    auto original_id = chunk->chunk_id();

    // Shrink returns chunk to pool (deactivated, not removed).
    co_await vdev->shrink(ChunkToShrink::Last);
    EXPECT_EQ(vdev->num_chunks(), 0u);

    // Expand again — should reuse the pooled chunk.
    auto reused = co_await vdev->expand(CHUNK_SIZE);
    EXPECT_EQ(reused->chunk_id(), original_id);

    co_await dm->close_devices();
    co_return;
}

// ── LoadRecovery ─────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, LoadRecovery) {
    uint32_t vdev_id{};
    BlkId written_blk;

    // Phase 1: create vdev, alloc blocks, write data.
    {
        auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        auto vdev = co_await dm->create_vdev(self.static_params("test_recovery", 2));
        vdev_id = vdev->vdev_id();

        blk_alloc_hints hints;
        auto status = vdev->alloc_contiguous_blks(1, hints, written_blk);
        CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

        IOBuffer wbuf{BLK_SIZE, 512};
        std::memset(wbuf.bytes(), 0xDD, BLK_SIZE);
        co_await vdev->write(wbuf, written_blk);
        co_await vdev->fsync();
        co_await dm->close_devices();
    }

    // Phase 2: reload and verify.
    {
        auto dm = DeviceManager::create(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();
        auto vdev = dm->get_vdev(vdev_id);
        CO_ASSERT_NE(vdev, nullptr);
        EXPECT_EQ(vdev->name(), "test_recovery");

        IOBuffer rbuf{BLK_SIZE, 512};
        auto ec = co_await vdev->read(rbuf, written_blk);
        CO_ASSERT_FALSE(ec);
        // Verify the pattern we wrote.
        for (uint32_t i = 0; i < BLK_SIZE; ++i) {
            EXPECT_EQ(rbuf.bytes()[i], 0xDD) << "byte " << i << " mismatch after recovery";
        }
        co_await dm->close_devices();
    }
}

// ── FullWorkflow ─────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, FullWorkflow) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_e2e", 2));

    // Format (zero data area).
    co_await vdev->format();

    // Alloc.
    BlkId blkid;
    blk_alloc_hints hints;
    auto status = vdev->alloc_contiguous_blks(2, hints, blkid);
    CO_ASSERT_EQ(status, BlkAllocStatus::SUCCESS);

    // Write.
    uint32_t total_size = 2 * BLK_SIZE;
    IOBuffer wbuf{total_size, 512};
    std::memset(wbuf.bytes(), 0xEE, total_size);
    co_await vdev->write(wbuf, blkid);

    // Fsync.
    co_await vdev->fsync();

    // Read + verify.
    IOBuffer rbuf{total_size, 512};
    auto ec = co_await vdev->read(rbuf, blkid);
    CO_ASSERT_FALSE(ec);
    EXPECT_EQ(std::memcmp(wbuf.bytes(), rbuf.bytes(), total_size), 0);

    // Free.
    vdev->free_blk(blkid);

    co_await dm->close_devices();
}

// ── MostAvailableSpaceSelector ───────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(VDevTest, MostAvailableSpaceSelector) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);

    VDevParameters p = self.static_params("test_most_avail", 4);
    p.chunk_sel_type = ChunkSelectorType::MostAvailableSpace;
    auto vdev = co_await dm->create_vdev(std::move(p));
    CO_ASSERT_NE(vdev, nullptr);
    EXPECT_EQ(vdev->chunk_selector_type(), ChunkSelectorType::MostAvailableSpace);

    // Alloc a few blocks — the selector should pick from the chunk with the most free space.
    BlkId blkid;
    blk_alloc_hints hints;
    auto status = vdev->alloc_contiguous_blks(1, hints, blkid);
    EXPECT_EQ(status, BlkAllocStatus::SUCCESS);

    co_await dm->close_devices();
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_vdev");
    HomeStoreDynamicConfig::init_settings_default();
    ::testing::InitGoogleTest(&argc, argv);
    iomanager::init_iomgr(2);
    int rc = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return rc;
}