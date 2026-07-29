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
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <folly/coro/Collect.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "homestore/base/blk.h"
#include "homestore/base/hs_runtime_config.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/physical_dev.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/device/chunk.h"
#include "homestore/managers.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;
using sisl::IoBufOwn;

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
        // DeviceManager::create() registers itself in Managers::s_device_mgr_ — drop that here so the
        // DeviceManager is destroyed within the test lifetime.  Without this, the static handle keeps the
        // DeviceManager alive until program exit, where ~BitmapBlkAllocator → ~Bitset → aligned_free races
        // against sisl's aligned_alloc_metrics() Meyers-singleton destructor.
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
    IoBufOwn rbuf{BLK_SIZE, 512};
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

    IoBufOwn wbuf{BLK_SIZE, 512};
    std::memset(wbuf.bytes(), 0xAA, BLK_SIZE);
    co_await vdev->write(wbuf, blkid);

    IoBufOwn rbuf{BLK_SIZE, 512};
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
    IoBufOwn wbuf{total_size, 512};
    for (uint32_t i = 0; i < total_size; ++i) {
        wbuf.bytes()[i] = to_u8(i & 0xFF);
    }
    co_await vdev->write(wbuf, blkid);

    IoBufOwn rbuf{total_size, 512};
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

    // Write 4 separate buffers with distinct patterns.  reserve() keeps element addresses stable (IoBufOwn is
    // move-only) so the SgList pointers below stay valid.
    std::vector< IoBufOwn > wbufs;
    wbufs.reserve(nblks);
    for (int i = 0; i < nblks; ++i) {
        wbufs.emplace_back(BLK_SIZE, 512);
        std::memset(wbufs.back().bytes(), 0xC0 + i, BLK_SIZE);
    }

    // Save copies for verification.
    std::vector< std::vector< uint8_t > > saved(nblks);
    for (int i = 0; i < nblks; ++i) {
        saved[i].assign(wbufs[i].bytes(), wbufs[i].bytes() + BLK_SIZE);
    }

    sisl::SgList wsg;
    for (auto& b : wbufs) {
        wsg.bufs.push_back(&b);
    }
    co_await vdev->writev(wsg, blkid);

    std::vector< IoBufOwn > rbufs;
    rbufs.reserve(nblks);
    for (int i = 0; i < nblks; ++i) {
        rbufs.emplace_back(BLK_SIZE, 512);
    }
    sisl::SgList rsg;
    for (auto& b : rbufs) {
        rsg.bufs.push_back(&b);
    }
    auto ec = co_await vdev->readv(rsg, blkid);
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

        IoBufOwn wbuf{BLK_SIZE, 512};
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

        IoBufOwn rbuf{BLK_SIZE, 512};
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
    IoBufOwn wbuf{total_size, 512};
    std::memset(wbuf.bytes(), 0xEE, total_size);
    co_await vdev->write(wbuf, blkid);

    // Fsync.
    co_await vdev->fsync();

    // Read + verify.
    IoBufOwn rbuf{total_size, 512};
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

// ── ConcurrentWrites ─────────────────────────────────────────────────────────────────────────────────────────────────
// Many writers hammering the SAME vdev in parallel, spread across reactors.  Every other VDev test issues IO from a
// single coroutine, so nothing exercised concurrent VirtualDev::write — a data race or a lock held across a co_await
// (e.g. chunk_mgmt_mutex_) would deadlock here.  Each block reads back its own writer's byte pattern.
CORO_TEST_F(VDevTest, ConcurrentWrites) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_conc_write", 4));

    constexpr int N = 16;
    std::vector< BlkId > blkids(N);
    for (int i = 0; i < N; ++i) {
        blk_alloc_hints hints;
        CO_ASSERT_EQ(vdev->alloc_contiguous_blks(1, hints, blkids[i]), BlkAllocStatus::SUCCESS);
    }

    const size_t nreactors = iomgr().num_reactors();
    std::vector< Async< void > > writes;
    writes.reserve(N);
    for (int i = 0; i < N; ++i) {
        // Captureless coroutine lambda invoked with by-value params — the params live in the coroutine frame, so
        // they outlive the closure temporary (a captured lambda coroutine would be a stack-use-after-scope: CP.51).
        writes.push_back(iomgr().spawn_waitable(
            ReactorTarget::reactor(i % nreactors),
            [](shared< VirtualDev > vd, BlkId bid, int val) -> Async< void > {
                IoBufOwn wbuf{BLK_SIZE, 512};
                std::memset(wbuf.bytes(), to_u8(val & 0xFF), BLK_SIZE);
                co_await vd->write(wbuf, bid);
            }(vdev, blkids[i], i)));
    }
    co_await folly::coro::collectAllRange(std::move(writes));

    for (int i = 0; i < N; ++i) {
        IoBufOwn rbuf{BLK_SIZE, 512};
        auto ec = co_await vdev->read(rbuf, blkids[i]);
        CO_ASSERT_FALSE(ec);
        EXPECT_EQ(rbuf.bytes()[0], to_u8(i & 0xFF)) << "block " << i << " has wrong content after concurrent writes";
    }
    co_await dm->close_devices();
}

// ── ConcurrentExpand ─────────────────────────────────────────────────────────────────────────────────────────────────
// Many expanders racing on the same vdev.  Exercises VirtualDev::expand under concurrency (chunk_mgmt_mutex_ + chunk
// allocation).  A blocking mutex held across a co_await, or an RCU grace-period wait on a reactor thread, deadlocks
// here; a state race yields a wrong final chunk count.
CORO_TEST_F(VDevTest, ConcurrentExpand) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.dynamic_params("test_conc_expand"));
    CO_ASSERT_EQ(vdev->num_chunks(), 0u);

    constexpr int N = 4;
    const size_t nreactors = iomgr().num_reactors();
    std::vector< Async< shared< Chunk > > > expands;
    expands.reserve(N);
    for (int i = 0; i < N; ++i) {
        expands.push_back(iomgr().spawn_waitable(
            ReactorTarget::reactor(i % nreactors),
            [](shared< VirtualDev > vd) -> Async< shared< Chunk > > { co_return co_await vd->expand(CHUNK_SIZE); }(vdev)));
    }
    auto chunks = co_await folly::coro::collectAllRange(std::move(expands));
    for (auto& c : chunks) {
        CO_ASSERT_NE(c, nullptr);
    }
    EXPECT_EQ(vdev->num_chunks(), to_u32(N)) << "concurrent expand produced wrong chunk count";
    co_await dm->close_devices();
}

// ── ConcurrentWritesSinglePdev ───────────────────────────────────────────────────────────────────────────────────────
// Narrows ConcurrentWrites: same interleaved concurrent-write pattern, but on a vdev with ONE chunk on ONE pdev so the
// block->offset mapping is trivial (blk_num*blk_size within a single chunk).  PhysicalDev is already proven clean, so
// if this PASSES the corruption is in VirtualDev's multi-chunk / striped mapping; if it FAILS the race is in
// VirtualDev::write's core path.
CORO_TEST_F(VDevTest, ConcurrentWritesSinglePdev) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    VDevParameters p;
    p.vdev_name = "test_conc_1pdev";
    p.initial_chunk_size = CHUNK_SIZE;
    p.initial_num_chunks = 1;
    p.blk_size = BLK_SIZE;
    p.dev_type = HSDevType::Data;
    p.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
    p.alloc_type = BlkAllocatorType::SlabCompact;
    p.chunk_sel_type = ChunkSelectorType::RoundRobin;
    auto vdev = co_await dm->create_vdev(std::move(p));

    constexpr int N = 16;
    std::vector< BlkId > blkids(N);
    for (int i = 0; i < N; ++i) {
        blk_alloc_hints hints;
        CO_ASSERT_EQ(vdev->alloc_contiguous_blks(1, hints, blkids[i]), BlkAllocStatus::SUCCESS);
    }

    const size_t nreactors = iomgr().num_reactors();
    std::vector< Async< void > > writes;
    writes.reserve(N);
    for (int i = 0; i < N; ++i) {
        writes.push_back(iomgr().spawn_waitable(
            ReactorTarget::reactor(i % nreactors),
            [](shared< VirtualDev > vd, BlkId bid, int val) -> Async< void > {
                IoBufOwn wbuf{BLK_SIZE, 512};
                std::memset(wbuf.bytes(), to_u8(val & 0xFF), BLK_SIZE);
                co_await vd->write(wbuf, bid);
            }(vdev, blkids[i], i)));
    }
    co_await folly::coro::collectAllRange(std::move(writes));

    for (int i = 0; i < N; ++i) {
        IoBufOwn rbuf{BLK_SIZE, 512};
        auto ec = co_await vdev->read(rbuf, blkids[i]);
        CO_ASSERT_FALSE(ec);
        EXPECT_EQ(rbuf.bytes()[0], to_u8(i & 0xFF)) << "block " << i << " wrong content (single-pdev, single-chunk)";
    }
    co_await dm->close_devices();
}

// ── ConcurrentWritesMultiChunkSinglePdev ─────────────────────────────────────────────────────────────────────────────
// Next narrowing after ConcurrentWritesSinglePdev: 4 chunks but still ONE pdev (no striping).  If this PASSES, the
// corruption is in the multi-pdev STRIPING offset math; if it FAILS, it's the multi-chunk / round-robin mapping.
CORO_TEST_F(VDevTest, ConcurrentWritesMultiChunkSinglePdev) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    VDevParameters p;
    p.vdev_name = "test_conc_multichunk_1pdev";
    p.initial_chunk_size = CHUNK_SIZE;
    p.initial_num_chunks = 4;
    p.blk_size = BLK_SIZE;
    p.dev_type = HSDevType::Data;
    p.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
    p.alloc_type = BlkAllocatorType::SlabCompact;
    p.chunk_sel_type = ChunkSelectorType::RoundRobin;
    auto vdev = co_await dm->create_vdev(std::move(p));

    constexpr int N = 16;
    std::vector< BlkId > blkids(N);
    for (int i = 0; i < N; ++i) {
        blk_alloc_hints hints;
        CO_ASSERT_EQ(vdev->alloc_contiguous_blks(1, hints, blkids[i]), BlkAllocStatus::SUCCESS);
    }

    const size_t nreactors = iomgr().num_reactors();
    std::vector< Async< void > > writes;
    writes.reserve(N);
    for (int i = 0; i < N; ++i) {
        writes.push_back(iomgr().spawn_waitable(
            ReactorTarget::reactor(i % nreactors),
            [](shared< VirtualDev > vd, BlkId bid, int val) -> Async< void > {
                IoBufOwn wbuf{BLK_SIZE, 512};
                std::memset(wbuf.bytes(), to_u8(val & 0xFF), BLK_SIZE);
                co_await vd->write(wbuf, bid);
            }(vdev, blkids[i], i)));
    }
    co_await folly::coro::collectAllRange(std::move(writes));

    for (int i = 0; i < N; ++i) {
        IoBufOwn rbuf{BLK_SIZE, 512};
        auto ec = co_await vdev->read(rbuf, blkids[i]);
        CO_ASSERT_FALSE(ec);
        EXPECT_EQ(rbuf.bytes()[0], to_u8(i & 0xFF)) << "block " << i << " wrong content (4 chunks, single pdev)";
    }
    co_await dm->close_devices();
}

// ── SerialWritesStriped ──────────────────────────────────────────────────────────────────────────────────────────────
// Control for ConcurrentWrites: identical striped config (4 chunks, AllPDevStriped over 2 pdevs) but writes issued
// SERIALLY.  If this PASSES, the ConcurrentWrites corruption is a concurrency race in the striped write path; if it
// FAILS, the striped block->(pdev,offset) mapping itself is wrong.
CORO_TEST_F(VDevTest, SerialWritesStriped) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_serial_striped", 4));

    constexpr int N = 16;
    std::vector< BlkId > blkids(N);
    for (int i = 0; i < N; ++i) {
        blk_alloc_hints hints;
        CO_ASSERT_EQ(vdev->alloc_contiguous_blks(1, hints, blkids[i]), BlkAllocStatus::SUCCESS);
    }

    // Serial writes.
    for (int i = 0; i < N; ++i) {
        IoBufOwn wbuf{BLK_SIZE, 512};
        std::memset(wbuf.bytes(), to_u8(i & 0xFF), BLK_SIZE);
        co_await vdev->write(wbuf, blkids[i]);
    }
    for (int i = 0; i < N; ++i) {
        IoBufOwn rbuf{BLK_SIZE, 512};
        auto ec = co_await vdev->read(rbuf, blkids[i]);
        CO_ASSERT_FALSE(ec);
        EXPECT_EQ(rbuf.bytes()[0], to_u8(i & 0xFF)) << "block " << i << " wrong content (serial striped)";
    }
    co_await dm->close_devices();
}

// ── StripedChunkIdsGloballyUnique ────────────────────────────────────────────────────────────────────────────────────
// Directly asserts the fix: in a striped vdev spanning both pdevs, every chunk_id — and its narrowed chunk_num_t
// (uint16) form used inside BlkId — must be globally distinct.  The old formula chunk_id = pdev_id*MAX_CHUNKS + cslot
// overflowed the uint16 chunk_num for pdev_id >= 1, so pdev-1's chunks aliased pdev-0's and writes corrupted each
// other.  With chunk_id drawn from the DeviceManager's global pool, all ids are unique in [0, 64K).
CORO_TEST_F(VDevTest, StripedChunkIdsGloballyUnique) {
    auto dm = co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    auto vdev = co_await dm->create_vdev(self.static_params("test_unique_ids", 4)); // striped over 2 pdevs

    const uint32_t n = vdev->num_chunks();
    CO_ASSERT_EQ(n, 4u);

    std::set< uint32_t > ids;
    std::set< uint16_t > narrowed;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t cid = vdev->get_nth_chunk(i)->chunk_id();
        ids.insert(cid);
        narrowed.insert(to_u16(cid));
    }
    EXPECT_EQ(ids.size(), n) << "chunk_ids collide across pdevs in a striped vdev";
    EXPECT_EQ(narrowed.size(), n) << "chunk_num_t (uint16) forms collide — BlkId would address the wrong chunk";

    co_await dm->close_devices();
}

// ── ChunkIdPoolRebuiltOnRecovery ─────────────────────────────────────────────────────────────────────────────────────
// Validates that the in-memory global chunk-id pool is rebuilt from loaded ChunkInfos on boot (mark_chunk_id_used).
// Create a striped vdev, record its chunk_ids, reload, then create a SECOND striped vdev: its freshly-allocated
// chunk_ids must not collide with the ones recovered from disk.  Without the rebuild, the pool would restart at 0 and
// the new vdev's chunks would reuse the recovered ids.
CORO_TEST_F(VDevTest, ChunkIdPoolRebuiltOnRecovery) {
    std::set< uint32_t > first_ids;

    // Phase 1: create a striped vdev, capture its chunk_ids.
    {
        auto dm =
            co_await DeviceManager::create_and_format(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        auto vdev = co_await dm->create_vdev(self.static_params("test_recov_v1", 4));
        for (uint32_t i = 0; i < vdev->num_chunks(); ++i) {
            first_ids.insert(vdev->get_nth_chunk(i)->chunk_id());
        }
        co_await dm->close_devices();
    }

    // Phase 2: reload (pool rebuilt from disk), then create a second vdev and check for collisions.
    {
        auto dm = DeviceManager::create(self.make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();

        auto vdev2 = co_await dm->create_vdev(self.static_params("test_recov_v2", 4));
        for (uint32_t i = 0; i < vdev2->num_chunks(); ++i) {
            const uint32_t cid = vdev2->get_nth_chunk(i)->chunk_id();
            EXPECT_EQ(first_ids.count(cid), 0u)
                << "chunk_id " << cid << " reused after recovery — global pool was not rebuilt from disk";
        }
        co_await dm->close_devices();
    }
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_vdev");
    HomeStoreRuntimeConfig::init_settings_default();
    ::testing::InitGoogleTest(&argc, argv);
    iomanager::init_iomgr(2);
    int rc = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return rc;
}