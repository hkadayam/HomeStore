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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "iomanager/iomanager.h"
#include "base/test_defs.h"

#include "base/homestore_config.hpp"
#include "device/device_manager.h"
#include "device/virtual_dev.h"

using namespace homestore;

static constexpr uint64_t DEV_SIZE = 128 * 1024 * 1024; // 128 MB
static constexpr uint32_t BLK_SIZE = 4096;
static constexpr uint64_t CHUNK_SIZE = 16 * 1024 * 1024; // 16 MB

// ── Fixture ──────────────────────────────────────────────────────────────────────────────────────────────────────────
class DevMgrTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_devmgr_{}", i);
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

    shared< DeviceManager > make_dm() const {
        return DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    }

    static constexpr size_t num_devs_ = 3;
    std::vector< std::string > dev_paths_;
};

// ── FormatAndFirstTimeBoot ───────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, FormatAndFirstTimeBoot) {
    auto dm = self.make_dm();
    EXPECT_TRUE(dm->is_first_time_boot());
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    // After format, pdevs should be loaded.
    auto pdevs = dm->get_all_pdevs();
    EXPECT_EQ(pdevs.size(), self.num_devs_);
    co_await dm->close_devices();
}

// ── LoadAfterFormat ──────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, LoadAfterFormat) {
    // Phase 1: format.
    {
        auto dm = self.make_dm();
        co_await dm->format_devices();
    co_await dm->commit_formatting();
        co_await dm->close_devices();
    }

    // Phase 2: reload.
    {
        auto dm = self.make_dm();
        co_await dm->load_devices();
        EXPECT_FALSE(dm->is_first_time_boot());
        auto pdevs = dm->get_all_pdevs();
        EXPECT_EQ(pdevs.size(), self.num_devs_);
        co_await dm->close_devices();
    }
}

// ── CreateVdevStriped ────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, CreateVdevStriped) {
    auto dm = self.make_dm();
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    VDevParameters params;
    params.vdev_name = "striped_vdev";
    params.vdev_size = CHUNK_SIZE * self.num_devs_;
    params.num_chunks = to_u32(self.num_devs_);
    params.chunk_size = CHUNK_SIZE;
    params.blk_size = BLK_SIZE;
    params.dev_type = HSDevType::Data;
    params.multi_pdev_opts = MultiPDevOpts::AllPDevStriped;
    params.alloc_type = BlkAllocatorType::SlabCompact;
    params.chunk_sel_type = ChunkSelectorType::RoundRobin;

    auto vdev = co_await dm->create_vdev(std::move(params));
    CO_ASSERT_NE(vdev, nullptr);

    // Chunks should be distributed across multiple pdevs.
    auto chunks = vdev->get_chunks();
    std::unordered_set< uint32_t > pdev_ids;
    for (auto& c : chunks) {
        pdev_ids.insert(c->physical_dev()->pdev_id());
    }
    // With AllPDevStriped and 3 pdevs, chunks should span at least 2 pdevs.
    EXPECT_GE(pdev_ids.size(), 2u);

    co_await dm->close_devices();
}

// ── CreateMultipleVdevs ──────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, CreateMultipleVdevs) {
    auto dm = self.make_dm();
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    std::unordered_set< uint32_t > vdev_ids;
    for (int i = 0; i < 3; ++i) {
        VDevParameters params;
        params.vdev_name = fmt::format("vdev_{}", i);
        params.vdev_size = CHUNK_SIZE;
        params.num_chunks = 1;
        params.chunk_size = CHUNK_SIZE;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        auto vdev = co_await dm->create_vdev(std::move(params));
        CO_ASSERT_NE(vdev, nullptr);
        vdev_ids.insert(vdev->vdev_id());
    }

    // All 3 vdevs should have distinct IDs.
    EXPECT_EQ(vdev_ids.size(), 3u);
    co_await dm->close_devices();
}

// ── DestroyVdev ──────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, DestroyVdev) {
    auto dm = self.make_dm();
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    VDevParameters params;
    params.vdev_name = "to_destroy";
    params.vdev_size = CHUNK_SIZE;
    params.num_chunks = 1;
    params.chunk_size = CHUNK_SIZE;
    params.blk_size = BLK_SIZE;
    params.dev_type = HSDevType::Data;
    params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
    params.alloc_type = BlkAllocatorType::SlabCompact;
    params.chunk_sel_type = ChunkSelectorType::RoundRobin;

    auto vdev = co_await dm->create_vdev(std::move(params));
    auto vid = vdev->vdev_id();
    CO_ASSERT_NE(dm->get_vdev(vid), nullptr);

    co_await dm->destroy_vdev(std::move(vdev));
    EXPECT_EQ(dm->get_vdev(vid), nullptr);

    co_await dm->close_devices();
}

// ── VdevPersistAcrossRestart ─────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, VdevPersistAcrossRestart) {
    uint32_t vid1{}, vid2{};

    // Phase 1: create 2 vdevs.
    {
        auto dm = self.make_dm();
        co_await dm->format_devices();
    co_await dm->commit_formatting();

        VDevParameters p1;
        p1.vdev_name = "persist_a";
        p1.vdev_size = CHUNK_SIZE;
        p1.num_chunks = 1;
        p1.chunk_size = CHUNK_SIZE;
        p1.blk_size = BLK_SIZE;
        p1.dev_type = HSDevType::Data;
        p1.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        p1.alloc_type = BlkAllocatorType::SlabCompact;
        p1.chunk_sel_type = ChunkSelectorType::RoundRobin;

        VDevParameters p2 = p1;
        p2.vdev_name = "persist_b";

        auto v1 = co_await dm->create_vdev(std::move(p1));
        vid1 = v1->vdev_id();

        auto v2 = co_await dm->create_vdev(std::move(p2));
        vid2 = v2->vdev_id();

        co_await dm->close_devices();
    }

    // Phase 2: reload and verify.
    {
        auto dm = self.make_dm();
        co_await dm->load_devices();
        EXPECT_FALSE(dm->is_first_time_boot());

        auto v1 = dm->get_vdev(vid1);
        auto v2 = dm->get_vdev(vid2);
        CO_ASSERT_NE(v1, nullptr);
        CO_ASSERT_NE(v2, nullptr);
        EXPECT_EQ(v1->name(), "persist_a");
        EXPECT_EQ(v2->name(), "persist_b");

        co_await dm->close_devices();
    }
}

// ── DynamicChunkCreation ─────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, DynamicChunkCreation) {
    uint32_t vid{};

    // Phase 1: create dynamic vdev, expand 3 chunks.
    {
        auto dm = self.make_dm();
        co_await dm->format_devices();
    co_await dm->commit_formatting();

        VDevParameters params;
        params.vdev_name = "dynamic_chunks";
        params.vdev_size = 0;
        params.num_chunks = 0;
        params.chunk_size = 0;
        params.incremental_chunk_size = CHUNK_SIZE;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::None;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        auto vdev = co_await dm->create_vdev(std::move(params));
        vid = vdev->vdev_id();

        for (int i = 0; i < 3; ++i) {
            co_await vdev->expand(CHUNK_SIZE);
        }
        EXPECT_EQ(vdev->num_chunks(), 3u);
        co_await dm->close_devices();
    }

    // Phase 2: reload, verify 3 chunks, expand 2 more.
    {
        auto dm = self.make_dm();
        co_await dm->load_devices();
        auto vdev = dm->get_vdev(vid);
        CO_ASSERT_NE(vdev, nullptr);
        EXPECT_EQ(vdev->num_chunks(), 3u);

        for (int i = 0; i < 2; ++i) {
            co_await vdev->expand(CHUNK_SIZE);
        }
        EXPECT_EQ(vdev->num_chunks(), 5u);

        co_await dm->close_devices();
    }
}

// ── CapacityQueries ──────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, CapacityQueries) {
    auto dm = self.make_dm();
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    // Total capacity across all 3 pdevs should be roughly 3 * DEV_SIZE (minus superblock overhead).
    uint64_t total = dm->total_capacity();
    EXPECT_GT(total, 0u);
    EXPECT_LE(total, self.num_devs_ * DEV_SIZE);

    // All devices are Data type, so by-type should match total.
    uint64_t data_total = dm->total_capacity_by_type(HSDevType::Data);
    EXPECT_EQ(data_total, total);

    // Alignment queries should return non-zero.
    EXPECT_GT(dm->atomic_page_size(HSDevType::Data), 0u);
    EXPECT_GT(dm->optimal_page_size(HSDevType::Data), 0u);
    EXPECT_GT(dm->align_size(HSDevType::Data), 0u);

    co_await dm->close_devices();
}

// ── AllocateAndFreeVdevId ────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, AllocateAndFreeVdevId) {
    auto dm = self.make_dm();
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    auto id1 = dm->allocate_vdev_id();
    auto id2 = dm->allocate_vdev_id();
    auto id3 = dm->allocate_vdev_id();
    CO_ASSERT_TRUE(id1.has_value());
    CO_ASSERT_TRUE(id2.has_value());
    CO_ASSERT_TRUE(id3.has_value());

    // All IDs should be unique.
    std::unordered_set< uint32_t > ids{id1.value(), id2.value(), id3.value()};
    EXPECT_EQ(ids.size(), 3u);

    // Free id2, re-allocate — should get id2 back (or at least a valid id).
    dm->free_vdev_id(id2.value());
    auto id4 = dm->allocate_vdev_id();
    CO_ASSERT_TRUE(id4.has_value());
    EXPECT_EQ(id4.value(), id2.value());

    co_await dm->close_devices();
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_dev_mgr");
    HomeStoreDynamicConfig::init_settings_default();
    ::testing::InitGoogleTest(&argc, argv);
    init_iomgr(2);
    int rc = RUN_ALL_TESTS();
    stop_iomgr();
    return rc;
}