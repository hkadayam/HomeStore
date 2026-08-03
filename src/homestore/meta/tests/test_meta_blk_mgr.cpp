/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <algorithm>
#include "common/async.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "sisl/fds/buffer.h"
#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "iomanager/iomanager.h"
#include "homestore/base/test_defs.h"

#include "common/defs.h"
#include "homestore/base/blk.h"
#include "homestore/base/crc.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/physical_dev.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/device/chunk.h"
#include "homestore/meta/meta_blk.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/meta/meta_client.h"
#include "homestore/managers.h"

using namespace homestore;
using namespace iomanager;
using sisl::IoBuf;

static constexpr uint64_t DEV_SIZE = 256 * 1024 * 1024;      // 256 MB per device
static constexpr uint64_t META_VDEV_SIZE = 64 * 1024 * 1024; // 64 MB for meta vdev

// ── Pattern helpers ─────────────────────────────────────────────────────────────────────────────────────────────────
// Each block is identified by a uint64_t id that seeds a deterministic byte pattern, so we can verify data integrity
// without keeping the original buffer around.
static sisl::IoBufShared make_pattern_buf(uint64_t id, size_t size) {
    auto buf = sisl::make_io_buf_shared(to_u32(size));
    auto* p = buf->bytes();
    for (size_t i = 0; i < size; ++i) {
        p[i] = static_cast< uint8_t >((id + i) & 0xFF);
    }
    return buf;
}

static bool verify_pattern(const sisl::IoBufView& buf, uint64_t id, size_t size) {
    if (buf.size() < size)
        return false;
    const auto* p = buf.cbytes();
    for (size_t i = 0; i < size; ++i) {
        if (p[i] != static_cast< uint8_t >((id + i) & 0xFF))
            return false;
    }
    return true;
}

// ── Fixture ─────────────────────────────────────────────────────────────────────────────────────────────────────────
//
// Each test creates temp device files, formats a DeviceManager, then uses MetaBlkManager::create() or load() to
// exercise the meta layer. "Restart" tests destroy the DeviceManager + MetaBlkManager, then reconstruct from the
// same files to validate persistence.
class MetaBlkMgrTest : public ::testing::Test {
public:
    void SetUp() override {
        for (size_t i = 0; i < num_devs_; ++i) {
            auto path = fmt::format("/tmp/hs_test_meta_{}", i);
            dev_paths_.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(DEV_SIZE - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void TearDown() override {
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

    // Format devices and create a fresh MetaBlkManager.
    Async< shared< DeviceManager > > format_and_create_meta() {
        auto dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->format_devices();
        co_await dm->commit_formatting();
        co_await MetaBlkManager::create(META_VDEV_SIZE);
        co_return dm;
    }

    // Reload devices and load the existing MetaBlkManager from disk.
    Async< shared< DeviceManager > > reload_meta() {
        Managers::reset();
        auto dm = DeviceManager::create(make_dev_infos(), IOFlag::BUFFERED_IO, IOFlag::BUFFERED_IO);
        co_await dm->load_devices();
        co_await MetaBlkManager::load();
        co_return dm;
    }

    static constexpr size_t num_devs_ = 2;
    std::vector< std::string > dev_paths_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 1: Format → create MetaBlkManager → tear down → reload.
// Validates the basic lifecycle: format, create, close, load succeeds without errors.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, CreateDropReload) {
    auto dm = co_await self.format_and_create_meta();
    co_await dm->close_devices();

    auto dm2 = co_await self.reload_meta();
    co_await dm2->close_devices();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 2: Register 3 clients → restart → re-register the same names.
// Verifies that each client recovers with the correct name and zero blocks.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, RegisterClientsRestart) {
    const std::vector< std::string > names = {"client_alpha", "client_beta", "client_gamma"};

    {
        auto dm = co_await self.format_and_create_meta();
        for (const auto& name : names) {
            auto client = co_await meta_mgr().register_client(name);
            EXPECT_EQ(co_await client.client_name(), name);
            EXPECT_EQ(co_await client.num_meta_blks(), 0u);
        }
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        for (const auto& name : names) {
            auto client = co_await meta_mgr().register_client(name);
            EXPECT_EQ(co_await client.client_name(), name);
            EXPECT_EQ(co_await client.num_meta_blks(), 0u);
        }
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 3: Create 2 clients, write 2 blocks each → restart → validate all 4 blocks recovered with correct data.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, WriteDataRestartValidate) {
    struct BlockRecord {
        uint64_t id;
        size_t size;
    };

    std::unordered_map< std::string, std::vector< BlockRecord > > expected;
    const size_t data_size = 128;

    {
        auto dm = co_await self.format_and_create_meta();
        uint64_t next_id = 1;

        for (const auto& cname : {"writer_a", "writer_b"}) {
            auto client = co_await meta_mgr().register_client(cname);
            auto& records = expected[cname];

            for (int j = 0; j < 2; ++j) {
                uint64_t id = next_id++;
                auto blk = co_await client.create_meta_blk(fmt::format("blk_{}", id), data_size);
                co_await client.write_meta_blk(blk, make_pattern_buf(id, data_size));
                records.push_back({id, data_size});
            }
            EXPECT_EQ(co_await client.num_meta_blks(), 2u);
        }
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        for (const auto& [cname, records] : expected) {
            auto client = co_await meta_mgr().register_client(cname);
            EXPECT_EQ(co_await client.num_meta_blks(), records.size());

            size_t found = 0;
            co_await client.for_each_recovered_block(
                [&found, &records](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                    ++found;
                    bool matched = false;
                    for (const auto& rec : records) {
                        if (verify_pattern(data, rec.id, rec.size)) {
                            matched = true;
                            break;
                        }
                    }
                    EXPECT_TRUE(matched) << "Recovered block data does not match any expected pattern";
                    co_return;
                });
            EXPECT_EQ(found, records.size());
        }
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 4: Write 5 blocks, remove 3 (first, middle, last) → restart → verify only 2 remain.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, RemoveBlocksRestart) {
    const size_t data_size = 64;

    struct BlkInfo {
        MetaBlk blk;
        uint64_t id;
    };

    std::vector< uint64_t > surviving_ids;

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("remover");

        std::vector< BlkInfo > blks;
        for (uint64_t id = 100; id < 105; ++id) {
            auto blk = co_await client.create_meta_blk(fmt::format("blk_{}", id), data_size);
            co_await client.write_meta_blk(blk, make_pattern_buf(id, data_size));
            blks.push_back({std::move(blk), id});
        }
        EXPECT_EQ(co_await client.num_meta_blks(), 5u);

        // Remove indices 0 (first), 2 (middle), 4 (last).
        co_await client.remove_meta_blk(blks[0].blk);
        co_await client.remove_meta_blk(blks[2].blk);
        co_await client.remove_meta_blk(blks[4].blk);
        EXPECT_EQ(co_await client.num_meta_blks(), 2u);

        surviving_ids = {blks[1].id, blks[3].id};
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("remover");
        EXPECT_EQ(co_await client.num_meta_blks(), 2u);

        size_t found = 0;
        co_await client.for_each_recovered_block(
            [&found, &surviving_ids, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                bool matched = false;
                for (uint64_t id : surviving_ids) {
                    if (verify_pattern(data, id, data_size)) {
                        matched = true;
                        break;
                    }
                }
                EXPECT_TRUE(matched) << "Unexpected data in recovered block";
                co_return;
            });
        EXPECT_EQ(found, 2u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 5: Two clients — remove ALL blocks from "victim", leave "survivor" intact → restart → verify isolation.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, RemoveAllBlocksRestart) {
    const size_t data_size = 64;

    {
        auto dm = co_await self.format_and_create_meta();
        auto victim = co_await meta_mgr().register_client("victim");
        auto survivor = co_await meta_mgr().register_client("survivor");

        std::vector< MetaBlk > victim_blks;
        for (uint64_t id = 200; id < 203; ++id) {
            auto blk = co_await victim.create_meta_blk(fmt::format("vblk_{}", id), data_size);
            co_await victim.write_meta_blk(blk, make_pattern_buf(id, data_size));
            victim_blks.push_back(std::move(blk));
        }

        for (uint64_t id = 300; id < 302; ++id) {
            auto blk = co_await survivor.create_meta_blk(fmt::format("sblk_{}", id), data_size);
            co_await survivor.write_meta_blk(blk, make_pattern_buf(id, data_size));
        }

        for (auto& blk : victim_blks) {
            co_await victim.remove_meta_blk(blk);
        }
        EXPECT_EQ(co_await victim.num_meta_blks(), 0u);
        EXPECT_EQ(co_await survivor.num_meta_blks(), 2u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto victim = co_await meta_mgr().register_client("victim");
        auto survivor = co_await meta_mgr().register_client("survivor");

        EXPECT_EQ(co_await victim.num_meta_blks(), 0u);
        EXPECT_EQ(co_await survivor.num_meta_blks(), 2u);

        size_t found = 0;
        co_await survivor.for_each_recovered_block(
            [&found, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                bool ok = verify_pattern(data, 300, data_size) || verify_pattern(data, 301, data_size);
                EXPECT_TRUE(ok) << "Survivor block data mismatch";
                co_return;
            });
        EXPECT_EQ(found, 2u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 6: Deregister a client → restart → verify the deregistered client has 0 blocks, others survive.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, DeregisterClientRestart) {
    const size_t data_size = 48;

    {
        auto dm = co_await self.format_and_create_meta();
        auto keeper_1 = co_await meta_mgr().register_client("keeper_1");
        auto doomed = co_await meta_mgr().register_client("doomed");
        auto keeper_3 = co_await meta_mgr().register_client("keeper_3");

        for (auto* c : std::initializer_list< MetaClient* >{&keeper_1, &doomed, &keeper_3}) {
            auto blk = co_await c->create_meta_blk("sb", data_size);
            co_await c->write_meta_blk(blk, make_pattern_buf(42, data_size));
        }

        // Remove doomed's block before deregistering the client.
        auto doomed_blk = co_await doomed.get_meta_blk("sb");
        CO_ASSERT_TRUE(doomed_blk.has_value());
        co_await doomed.remove_meta_blk(*doomed_blk);
        co_await meta_mgr().deregister_client(doomed);

        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto keeper_1 = co_await meta_mgr().register_client("keeper_1");
        auto doomed = co_await meta_mgr().register_client("doomed"); // re-registers as brand new
        auto keeper_3 = co_await meta_mgr().register_client("keeper_3");

        EXPECT_EQ(co_await keeper_1.num_meta_blks(), 1u);
        EXPECT_EQ(co_await doomed.num_meta_blks(), 0u);
        EXPECT_EQ(co_await keeper_3.num_meta_blks(), 1u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 7: Overflow blocks — write small (inline), large (overflow), and xlarge data → restart → validate all three.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, OverflowBlocks) {
    const size_t small_sz = 100;
    const size_t large_sz = 512 * 1024;   // 512 KB — likely overflow
    const size_t xlarge_sz = 1024 * 1024; // 1 MB — definitely overflow

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("overflow_test");

        auto blk_s = co_await client.create_meta_blk("small", small_sz);
        co_await client.write_meta_blk(blk_s, make_pattern_buf(1, small_sz));

        auto blk_l = co_await client.create_meta_blk("large", large_sz);
        co_await client.write_meta_blk(blk_l, make_pattern_buf(2, large_sz));

        auto blk_xl = co_await client.create_meta_blk("xlarge", xlarge_sz);
        co_await client.write_meta_blk(blk_xl, make_pattern_buf(3, xlarge_sz));

        EXPECT_EQ(co_await client.num_meta_blks(), 3u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("overflow_test");
        EXPECT_EQ(co_await client.num_meta_blks(), 3u);

        size_t found = 0;
        co_await client.for_each_recovered_block(
            [&found, small_sz, large_sz, xlarge_sz](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                auto name = blk.name();
                LOGINFO("OverflowBlocks visitor: found={} name={} data_size={}", found, name, data.size());
                if (name == "small") {
                    EXPECT_TRUE(verify_pattern(data, 1, small_sz));
                } else if (name == "large") {
                    EXPECT_TRUE(verify_pattern(data, 2, large_sz));
                } else if (name == "xlarge") {
                    EXPECT_TRUE(verify_pattern(data, 3, xlarge_sz));
                } else {
                    ADD_FAILURE() << "Unknown block name: " << name;
                }
                co_return;
            });
        EXPECT_EQ(found, 3u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 8: Update a block in-place → read back without restart → restart → verify updated data persisted.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, UpdateInPlace) {
    const size_t data_size = 200;

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("updater");

        auto blk = co_await client.create_meta_blk("cfg", data_size);
        co_await client.write_meta_blk(blk, make_pattern_buf(10, data_size));

        // Read back v1. Use get_meta_blk since blk was moved into the map by write_meta_blk.
        auto opt_blk1 = co_await client.get_meta_blk("cfg");
        CO_ASSERT_TRUE(opt_blk1.has_value());
        auto read1 = co_await client.read_meta_blk(*opt_blk1);
        EXPECT_TRUE(verify_pattern(read1, 10, data_size));

        // Overwrite with v2.
        co_await client.write_meta_blk(*opt_blk1, make_pattern_buf(20, data_size));

        // Read back v2.
        auto opt_blk2 = co_await client.get_meta_blk("cfg");
        CO_ASSERT_TRUE(opt_blk2.has_value());
        auto read2 = co_await client.read_meta_blk(*opt_blk2);
        EXPECT_TRUE(verify_pattern(read2, 20, data_size));

        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("updater");
        EXPECT_EQ(co_await client.num_meta_blks(), 1u);

        size_t found = 0;
        co_await client.for_each_recovered_block(
            [&found, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                EXPECT_TRUE(verify_pattern(data, 20, data_size)) << "Expected v2 data after restart";
                co_return;
            });
        EXPECT_EQ(found, 1u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 9: Size transitions — small → large → small, with restart verification after each transition.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, SizeTransitions) {
    const size_t small_sz = 64;
    const size_t large_sz = 256 * 1024; // 256 KB

    // Step 1: create with small data.
    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("resizer");
        auto blk = co_await client.create_meta_blk("data", small_sz);
        co_await client.write_meta_blk(blk, make_pattern_buf(1, small_sz));
        co_await dm->close_devices();
    }

    // Step 2: reload, update small → large.
    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("resizer");
        EXPECT_EQ(co_await client.num_meta_blks(), 1u);

        auto opt_blk = co_await client.get_meta_blk("data");
        CO_ASSERT_TRUE(opt_blk.has_value());
        co_await client.write_meta_blk(*opt_blk, make_pattern_buf(2, large_sz));
        co_await dm->close_devices();
    }

    // Step 3: reload, verify large data, then shrink back to small.
    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("resizer");

        auto opt_blk = co_await client.get_meta_blk("data");
        CO_ASSERT_TRUE(opt_blk.has_value());

        auto read_data = co_await client.read_meta_blk(*opt_blk);
        EXPECT_TRUE(verify_pattern(read_data, 2, large_sz)) << "Large data mismatch after restart";

        co_await client.write_meta_blk(*opt_blk, make_pattern_buf(3, small_sz));
        co_await dm->close_devices();
    }

    // Step 4: reload, verify small data.
    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("resizer");

        size_t found = 0;
        co_await client.for_each_recovered_block(
            [&found, small_sz](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                EXPECT_TRUE(verify_pattern(data, 3, small_sz)) << "Small data mismatch after shrink+restart";
                co_return;
            });
        EXPECT_EQ(found, 1u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 9a: Rewrite a NON-TAIL block through the cached handle create() returned, then restart.
//
// This is the case the earlier in-place tests missed: they only ever rewrote a single-block (tail) chain, and always
// re-fetched via get_meta_blk() first.  Here A is rewritten while B sits after it in the chain, and A is rewritten
// through the same handle create() handed back — never re-fetched.  With a per-copy MetaBlk that stale handle used to
// overwrite the chain linkage and orphan B; with the shared holder the linkage is authoritative and B survives.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, RewriteNonTailThroughCachedHandle) {
    const size_t data_size = 100;

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("rewriter");

        // A becomes the chain head.  Keep the handle create() returned — do NOT re-fetch via get_meta_blk.
        auto blk_a = co_await client.create_meta_blk("A", data_size);
        co_await client.write_meta_blk(blk_a, make_pattern_buf(1, data_size));

        // B is appended after A, so A is now a non-tail (mid-chain) block.
        auto blk_b = co_await client.create_meta_blk("B", data_size);
        co_await client.write_meta_blk(blk_b, make_pattern_buf(2, data_size));

        // Rewrite A through its cached handle.  The block is already linked, so this is a pure in-place payload update.
        co_await client.write_meta_blk(blk_a, make_pattern_buf(11, data_size));

        EXPECT_EQ(co_await client.num_meta_blks(), 2u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("rewriter");
        EXPECT_EQ(co_await client.num_meta_blks(), 2u) << "B must survive the rewrite of non-tail block A";

        bool saw_a = false;
        bool saw_b = false;
        co_await client.for_each_recovered_block(
            [&saw_a, &saw_b, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                if (blk.name() == "A") {
                    saw_a = true;
                    EXPECT_TRUE(verify_pattern(data, 11, data_size)) << "A must hold its rewritten (v2) data";
                } else if (blk.name() == "B") {
                    saw_b = true;
                    EXPECT_TRUE(verify_pattern(data, 2, data_size)) << "B must be intact";
                }
                co_return;
            });
        EXPECT_TRUE(saw_a) << "A missing after restart";
        EXPECT_TRUE(saw_b) << "B orphaned by A's rewrite";
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 9b: Repeatedly re-persist the head block through its cached handle with two successors behind it, then restart.
//
// Mirrors a COWBtree's per-flush pattern: three SB blocks (incr_map + full_map[0..1]) created up front, then the first
// one re-written on every CP.  Every block must survive, and the repeatedly-rewritten one must hold its last value.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, RewriteHeadRepeatedlyThenRestart) {
    const size_t data_size = 80;

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("flusher");

        auto blk0 = co_await client.create_meta_blk("s0", data_size);
        co_await client.write_meta_blk(blk0, make_pattern_buf(1000, data_size));
        auto blk1 = co_await client.create_meta_blk("s1", data_size);
        co_await client.write_meta_blk(blk1, make_pattern_buf(1001, data_size));
        auto blk2 = co_await client.create_meta_blk("s2", data_size);
        co_await client.write_meta_blk(blk2, make_pattern_buf(1002, data_size));

        // Re-persist s0 (the head, with s1/s2 behind it) five times through its cached handle.
        for (uint64_t v = 0; v < 5; ++v) {
            co_await client.write_meta_blk(blk0, make_pattern_buf(2000 + v, data_size));
        }
        EXPECT_EQ(co_await client.num_meta_blks(), 3u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("flusher");
        EXPECT_EQ(co_await client.num_meta_blks(), 3u) << "all three blocks must survive repeated head rewrites";

        std::unordered_map< std::string, bool > seen{{"s0", false}, {"s1", false}, {"s2", false}};
        co_await client.for_each_recovered_block(
            [&seen, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                auto it = seen.find(blk.name());
                if (it == seen.end()) {
                    co_return;
                }
                it->second = true;
                if (blk.name() == "s0") {
                    EXPECT_TRUE(verify_pattern(data, 2004, data_size)) << "s0 must hold its last rewrite";
                } else if (blk.name() == "s1") {
                    EXPECT_TRUE(verify_pattern(data, 1001, data_size)) << "s1 corrupted";
                } else {
                    EXPECT_TRUE(verify_pattern(data, 1002, data_size)) << "s2 corrupted";
                }
                co_return;
            });
        for (auto& [name, ok] : seen) {
            EXPECT_TRUE(ok) << "block " << name << " missing after restart";
        }
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 9c: Remove an ENTIRE chain, then build a fresh chain, then restart — mirrors a table (COWBtree) being dropped
// and a new one created on the same device before the next boot.  Only the second chain must survive; none of the
// first chain's blocks may reappear.  Removal follows the drop order: tail first, then head-first for the rest.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, RemoveChainThenReAddThenRestart) {
    const size_t data_size = 64;

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("dev");

        // Round A: chain a0 -> a1 -> a2 -> a3 (a3 appended last, like a node mblk written after the stream SBs).
        std::vector< MetaBlk > a;
        for (uint64_t id = 0; id < 4; ++id) {
            auto blk = co_await client.create_meta_blk(fmt::format("a{}", id), data_size);
            co_await client.write_meta_blk(blk, make_pattern_buf(id, data_size));
            a.push_back(std::move(blk));
        }

        // Drop the whole chain in the COWBtree destroy order: tail (a3) first, then head-first (a0, a1, a2).
        co_await client.remove_meta_blk(a[3]);
        co_await client.remove_meta_blk(a[0]);
        co_await client.remove_meta_blk(a[1]);
        co_await client.remove_meta_blk(a[2]);
        EXPECT_EQ(co_await client.num_meta_blks(), 0u);

        // Round B: a fresh chain b10..b13 on the now-empty client.
        for (uint64_t id = 10; id < 14; ++id) {
            auto blk = co_await client.create_meta_blk(fmt::format("b{}", id), data_size);
            co_await client.write_meta_blk(blk, make_pattern_buf(id, data_size));
        }
        EXPECT_EQ(co_await client.num_meta_blks(), 4u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("dev");
        EXPECT_EQ(co_await client.num_meta_blks(), 4u) << "only the round-B chain should survive the drop+recreate";

        size_t found = 0;
        co_await client.for_each_recovered_block(
            [&found](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                EXPECT_EQ(blk.name().substr(0, 1), std::string{"b"})
                    << "stale round-A block '" << blk.name() << "' reappeared after restart";
                co_return;
            });
        EXPECT_EQ(found, 4u);
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 10: Multiple restart cycles with interleaved write/remove.
//   Cycle 1: create 3 blocks.
//   Cycle 2: reload, verify 3, remove 1, add 2 more → 4 blocks.
//   Cycle 3: reload, verify 4 blocks.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, MultipleRestarts) {
    const size_t data_size = 80;
    std::vector< uint64_t > live_ids;

    {
        auto dm = co_await self.format_and_create_meta();
        auto client = co_await meta_mgr().register_client("multi_restart");
        for (uint64_t id : {400u, 401u, 402u}) {
            auto blk = co_await client.create_meta_blk(fmt::format("b_{}", id), data_size);
            co_await client.write_meta_blk(blk, make_pattern_buf(id, data_size));
        }
        live_ids = {400, 401, 402};
        EXPECT_EQ(co_await client.num_meta_blks(), 3u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("multi_restart");
        EXPECT_EQ(co_await client.num_meta_blks(), 3u);

        auto blk_to_remove = co_await client.get_meta_blk("b_401");
        CO_ASSERT_TRUE(blk_to_remove.has_value());
        co_await client.remove_meta_blk(*blk_to_remove);

        for (uint64_t id : {500u, 501u}) {
            auto blk = co_await client.create_meta_blk(fmt::format("b_{}", id), data_size);
            co_await client.write_meta_blk(blk, make_pattern_buf(id, data_size));
        }
        live_ids = {400, 402, 500, 501};
        EXPECT_EQ(co_await client.num_meta_blks(), 4u);
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        auto client = co_await meta_mgr().register_client("multi_restart");
        EXPECT_EQ(co_await client.num_meta_blks(), 4u);

        size_t found = 0;
        co_await client.for_each_recovered_block(
            [&found, &live_ids, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                ++found;
                bool matched = false;
                for (uint64_t id : live_ids) {
                    if (verify_pattern(data, id, data_size)) {
                        matched = true;
                        break;
                    }
                }
                EXPECT_TRUE(matched) << "Unexpected data in cycle-3 recovery";
                co_return;
            });
        EXPECT_EQ(found, 4u);
        co_await dm->close_devices();
    }
}

// ────────────────────────�����────��───��──────────────────────────────────────────────────────────────────────────────────
// Test 11: Register 20 clients, each with 1 block → restart → verify all 20 recovered.
// ───────────────────────────���────────────────────────��────────────────────���───────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, ManyClients) {
    const size_t data_size = 32;
    constexpr size_t num_clients = 20;

    {
        auto dm = co_await self.format_and_create_meta();
        for (size_t i = 0; i < num_clients; ++i) {
            auto client = co_await meta_mgr().register_client(fmt::format("client_{}", i));
            auto blk = co_await client.create_meta_blk("sb", data_size);
            co_await client.write_meta_blk(blk, make_pattern_buf(i, data_size));
        }
        co_await dm->close_devices();
    }

    {
        auto dm = co_await self.reload_meta();
        for (size_t i = 0; i < num_clients; ++i) {
            auto client = co_await meta_mgr().register_client(fmt::format("client_{}", i));
            EXPECT_EQ(co_await client.num_meta_blks(), 1u) << "client_" << i;

            size_t found = 0;
            co_await client.for_each_recovered_block(
                [&found, i, data_size](const MetaBlk& blk, const sisl::IoBufView& data) -> Async< void > {
                    ++found;
                    EXPECT_TRUE(verify_pattern(data, i, data_size));
                    co_return;
                });
            EXPECT_EQ(found, 1u);
        }
        co_await dm->close_devices();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Test 12: Write a block, read it back, verify the CRC in MetaBlkHeader matches the data payload.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
CORO_TEST_F(MetaBlkMgrTest, CrcIntegrity) {
    const size_t data_size = 256;

    auto dm = co_await self.format_and_create_meta();
    auto client = co_await meta_mgr().register_client("crc_test");

    auto blk = co_await client.create_meta_blk("payload", data_size);
    co_await client.write_meta_blk(blk, make_pattern_buf(77, data_size));

    auto opt_blk = co_await client.get_meta_blk("payload");
    CO_ASSERT_TRUE(opt_blk.has_value());
    auto read_data = co_await client.read_meta_blk(*opt_blk);
    uint32_t computed_crc = crc32_ieee(0, read_data.bytes(), to_u32(data_size));
    EXPECT_EQ(opt_blk->header().data_crc, computed_crc) << "CRC mismatch between header and computed";

    co_await dm->close_devices();
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_meta_blk_mgr");
    ::testing::InitGoogleTest(&argc, argv);
    iomanager::init_iomgr(2);
    int rc = RUN_ALL_TESTS();
    iomanager::stop_iomgr();
    return rc;
}