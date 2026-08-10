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
#include "sisl/flip/flip.h"
#include "sisl/flip/flip_client.h"
#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "homestore/base/crash_simulator.h"
#include "homestore/base/hs_runtime_config.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/device/chunk.h"
#include "homestore/device/physical_dev.h"
#include "homestore/managers.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;

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
        // Drop any DeviceManager still parked in Managers::s_device_mgr_ (installed by DeviceManager::create at
        // device_manager.cpp:72).  Without this the last test's DM lives until process exit and its ChunkProvisioner
        // bitmap's aligned_free races the AlignedAllocatorMetrics singleton teardown, SEGVing at buffer.h:174.
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

    shared< DeviceManager > make_dm() const {
        return DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    }

    // ── Crash simulation helpers ────────────────────────────────────────────────────────────────────────────────────
    // Install a fresh CrashSimulator with a no-op restart callback so crash_now() marks crashed_=true (gating every
    // PhysicalDev write to a silent no-op) but does not raise(SIGKILL) — the test process survives.  Fresh instance
    // resets crashed_ to false.  Call this before arming a flip AND again after the crash to unblock subsequent
    // reload writes.
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
    params.initial_chunk_size = CHUNK_SIZE;
    params.initial_num_chunks = to_u32(self.num_devs_);
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
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 1;
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
    params.initial_chunk_size = CHUNK_SIZE;
    params.initial_num_chunks = 1;
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
        p1.initial_chunk_size = CHUNK_SIZE;
        p1.initial_num_chunks = 1;
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
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 0;
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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// DegradedModeBoot — format with 3 devs, reload with only 2 devs.  DeviceManager compares expected pdev count
// (recorded in the first block header) against the actual dev_infos_ size and sets boot_in_degraded_mode when they
// disagree; the store still boots with the surviving pdev set.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, DegradedModeBoot) {
    // Phase 1: format with all 3 devs.
    {
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();
        co_await dm->close_devices();
    }

    // Phase 2: reload with only 2 of the 3 devs (drop the last).
    std::vector< DevInfo > partial;
    partial.emplace_back(self.dev_paths_[0], HSDevType::Data, DEV_SIZE);
    partial.emplace_back(self.dev_paths_[1], HSDevType::Data, DEV_SIZE);

    auto dm = DeviceManager::create(std::move(partial), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    co_await dm->load_devices();
    EXPECT_FALSE(dm->is_first_time_boot());
    EXPECT_TRUE(dm->is_boot_in_degraded_mode());
    EXPECT_EQ(dm->get_all_pdevs().size(), 2u);
    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// DataVsFastDeviceTypes — format with a mixed Fast + Data pool, verify per-type capacity queries and that a vdev
// created with dev_type=Fast has its chunks placed exclusively on Fast pdevs.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(DevMgrTest, DataVsFastDeviceTypes) {
    std::vector< DevInfo > mixed;
    mixed.emplace_back(self.dev_paths_[0], HSDevType::Fast, DEV_SIZE);
    mixed.emplace_back(self.dev_paths_[1], HSDevType::Data, DEV_SIZE);
    mixed.emplace_back(self.dev_paths_[2], HSDevType::Data, DEV_SIZE);

    auto dm = DeviceManager::create(std::move(mixed), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
    co_await dm->format_devices();
    co_await dm->commit_formatting();

    // Per-type queries: exactly one Fast pdev, two Data.
    EXPECT_EQ(dm->get_pdevs_by_dev_type(HSDevType::Fast).size(), 1u);
    EXPECT_EQ(dm->get_pdevs_by_dev_type(HSDevType::Data).size(), 2u);

    // Per-type capacity: Data (2 devs) should report roughly double Fast (1 dev).  Both non-zero.  Use ratio > 1.5
    // instead of exact 2× because each pdev subtracts some overhead for the super-block region.
    auto const cap_fast = dm->total_capacity_by_type(HSDevType::Fast);
    auto const cap_data = dm->total_capacity_by_type(HSDevType::Data);
    EXPECT_GT(cap_fast, 0u);
    EXPECT_GT(cap_data, cap_fast);
    EXPECT_GT(cap_data * 10, cap_fast * 15); // cap_data > 1.5 × cap_fast

    // Create a vdev pinned to Fast — every chunk must land on the Fast pdev.
    VDevParameters params;
    params.vdev_name = "fast_only_vdev";
    params.initial_chunk_size = CHUNK_SIZE;
    params.initial_num_chunks = 1;
    params.blk_size = BLK_SIZE;
    params.dev_type = HSDevType::Fast;
    params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
    params.alloc_type = BlkAllocatorType::SlabCompact;
    params.chunk_sel_type = ChunkSelectorType::RoundRobin;

    auto vdev = co_await dm->create_vdev(std::move(params));
    CO_ASSERT_NE(vdev, nullptr);

    auto const fast_pdev_id = dm->get_pdevs_by_dev_type(HSDevType::Fast).front()->pdev_id();
    for (auto const& chunk : vdev->get_chunks()) {
        EXPECT_EQ(chunk->physical_dev()->pdev_id(), fast_pdev_id) << "Fast vdev chunk landed on a Data pdev";
    }

    co_await dm->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Crash-window tests — one per named flip in device_manager.cpp / virtual_dev.cpp / physical_dev.cpp.  Common shape:
//   1. install_fresh_crash_sim (nop restart cb — process survives crash_now)
//   2. arm the target flip
//   3. run the operation that hits it
//   4. assert is_crash_simulated() (crash was taken)
//   5. remove flip, install fresh crash sim (resets crashed_ so reload writes go through)
//   6. close current DM (writes gated to no-op if still crashed), reload, assert recovery contract
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

// CP1 — crash BEFORE commit_formatting: managers formatted and durable, but no pdev's formatting_done marker was
// written.  Recovery must treat the device as first-time and re-format from scratch.
CORO_TEST_F(DevMgrTest, CrashBeforeCommitFormatting) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();

        self.arm_crash_flip("crash_before_commit_formatting");
        co_await dm->commit_formatting();
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_before_commit_formatting");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    // Reload — formatting_done never persisted, so this must look like a first-time boot again.
    auto dm = self.make_dm();
    EXPECT_TRUE(dm->is_first_time_boot());
    // Re-format cleanly must succeed with no dangling state.
    co_await dm->format_devices();
    co_await dm->commit_formatting();
    co_await dm->close_devices();
}

// CP2 — crash after a chunk's info record is written, before its slot bit is set.  Recovery iterates SET bits only
// (physical_dev.cpp::load_chunks), so the chunk vanishes harmlessly and can be re-created into the same slot later.
CORO_TEST_F(DevMgrTest, CrashAfterChunkInfoWriteBeforeSlotBit) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();

        self.arm_crash_flip("crash_after_chunk_info_write");

        VDevParameters params;
        params.vdev_name = "chunk_crash_vdev";
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 1;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        try {
            (void)co_await dm->create_vdev(std::move(params));
        } catch (...) {
            // create may or may not throw depending on where the crash lands; either way is_crash_simulated() is set.
        }
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_after_chunk_info_write");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    // Reload — the chunk's slot bit was never persisted, so load_chunks skips it; the vdev never got its VDevInfo
    // written either, so it's absent.  A subsequent vdev create in the same pdev must succeed.
    auto dm = self.make_dm();
    co_await dm->load_devices();
    EXPECT_EQ(dm->get_vdev("chunk_crash_vdev"), nullptr);
    co_await dm->close_devices();
}

// CP3 — crash after a chunk's info is free-marked, before its slot bit is cleared.  Tested via a full vdev destroy:
// the vdev-destroy path calls remove_chunks (batch), which hits the flip after writing free-marked ChunkInfos but
// before clearing the bitmap.  Recovery paths:
//   - load_chunks materialises the free-marked records at their still-set bits (no is_allocated check today).
//   - The vdev's VDevInfo was ALSO free-marked earlier in destroy_vdev (before this crash point), so load_vdevs
//     puts vdev_id into stale_slot_vdev_ids and cleanup_stale_slot_vdevs removes them anyway.
// End state: vdev absent, no live chunks pointing at it.
CORO_TEST_F(DevMgrTest, CrashAfterChunkInfoFreeBeforeBitClears) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();

        VDevParameters params;
        params.vdev_name = "chunk_free_crash_vdev";
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 2;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        auto vdev = co_await dm->create_vdev(std::move(params));

        self.arm_crash_flip("crash_after_chunk_info_free");
        try {
            co_await dm->destroy_vdev(std::move(vdev));
        } catch (...) {}
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_after_chunk_info_free");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    auto dm = self.make_dm();
    co_await dm->load_devices();
    EXPECT_EQ(dm->get_vdev("chunk_free_crash_vdev"), nullptr);
    co_await dm->close_devices();
}

// CP4 — crash after a vdev's chunks are created and durable, before write_vdev_info runs.  Recovery's dangling-chunk
// sweep (device_manager.cpp:546-561) walks pdevs, finds chunks with a vdev_id not in loaded_vdev_ids, and
// remove_chunks_for_vdev reclaims them.
CORO_TEST_F(DevMgrTest, CrashAfterVdevChunksCreate) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();

        self.arm_crash_flip("crash_after_vdev_chunks_create");

        VDevParameters params;
        params.vdev_name = "vdev_chunks_only";
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 2;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;
        try {
            (void)co_await dm->create_vdev(std::move(params));
        } catch (...) {}
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_after_vdev_chunks_create");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    auto dm = self.make_dm();
    co_await dm->load_devices();
    EXPECT_EQ(dm->get_vdev("vdev_chunks_only"), nullptr);
    co_await dm->close_devices();
}

// CP5 — crash after VDevInfo is written, before the vdev slot bitmap advances.  Recovery reads a bitmap without the
// new slot, so the vdev doesn't get loaded; the vdev's chunks are dangling by vdev_id and get swept.
CORO_TEST_F(DevMgrTest, CrashAfterVdevInfoWriteBeforeSlotBit) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();

        self.arm_crash_flip("crash_after_vdev_info_write");

        VDevParameters params;
        params.vdev_name = "vdev_info_only";
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 1;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;
        try {
            (void)co_await dm->create_vdev(std::move(params));
        } catch (...) {}
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_after_vdev_info_write");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    auto dm = self.make_dm();
    co_await dm->load_devices();
    EXPECT_EQ(dm->get_vdev("vdev_info_only"), nullptr);
    co_await dm->close_devices();
}

// CP6 — crash after a vdev's VDevInfo is free-marked, before its chunks are removed from disk.  Recovery's slow-scan
// finds a free-marked vinfo at a set slot → stale_slot_vdev_ids → cleanup_stale_slot_vdevs removes the chunks and
// frees the slot.
CORO_TEST_F(DevMgrTest, CrashAfterVdevInfoFreeBeforeChunksRemoved) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();

        VDevParameters params;
        params.vdev_name = "vdev_info_free_crash";
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 1;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        auto vdev = co_await dm->create_vdev(std::move(params));

        self.arm_crash_flip("crash_after_vdev_info_free");
        try {
            co_await dm->destroy_vdev(std::move(vdev));
        } catch (...) {}
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_after_vdev_info_free");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    auto dm = self.make_dm();
    co_await dm->load_devices();
    EXPECT_EQ(dm->get_vdev("vdev_info_free_crash"), nullptr);
    co_await dm->close_devices();
}

// CP7 — crash after all of a vdev's chunks are removed from disk, before its slot bit clears.  The stale-slot sweep
// still runs at load (VDevInfo is free-marked, but the slot bit is set), and free_vdev_id + write_vdev_slot_bitmap
// finish reclaiming the slot on this boot.
CORO_TEST_F(DevMgrTest, CrashAfterVdevChunksRemoveBeforeSlotBit) {
    {
        self.install_fresh_crash_sim();
        auto dm = self.make_dm();
        co_await dm->format_devices();
        co_await dm->commit_formatting();

        VDevParameters params;
        params.vdev_name = "vdev_chunks_removed_crash";
        params.initial_chunk_size = CHUNK_SIZE;
        params.initial_num_chunks = 1;
        params.blk_size = BLK_SIZE;
        params.dev_type = HSDevType::Data;
        params.multi_pdev_opts = MultiPDevOpts::SingleFirstPDev;
        params.alloc_type = BlkAllocatorType::SlabCompact;
        params.chunk_sel_type = ChunkSelectorType::RoundRobin;

        auto vdev = co_await dm->create_vdev(std::move(params));

        self.arm_crash_flip("crash_after_vdev_chunks_remove");
        try {
            co_await dm->destroy_vdev(std::move(vdev));
        } catch (...) {}
        EXPECT_TRUE(is_crash_simulated());

        self.remove_flip("crash_after_vdev_chunks_remove");
        self.install_fresh_crash_sim();
        co_await dm->close_devices();
    }

    auto dm = self.make_dm();
    co_await dm->load_devices();
    EXPECT_EQ(dm->get_vdev("vdev_chunks_removed_crash"), nullptr);
    co_await dm->close_devices();
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_dev_mgr");
    HomeStoreRuntimeConfig::init_settings_default();
    ::testing::InitGoogleTest(&argc, argv);
    iomanager::init_iomgr(2);
    int rc = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return rc;
}