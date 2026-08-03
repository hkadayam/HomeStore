/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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
//
// Durable COWBtree-backed TestStore implementation. This is the ONLY TU that instantiates the btree templates for
// the replication test suite, so category test files stay cheap to compile (they see only hs_test_cow_store.h).
//
#include "homestore/test_common/hs_test_cow_store.h"

#include <atomic>
#include <fmt/format.h>

#include "homestore/base/homestore_assert.h"
#include "homestore/managers.h"                         // blob_dev_mgr(), cow_btree_mgr(), device_mgr(), cp_mgr()
#include "homestore/device/device_manager.h"            // is_first_time_boot()
#include "homestore/blob/blob_dev_mgr.h"                // create_blob_dev / get_blob_dev
#include "homestore/index/cow_btree/cow_btree_mgr.h"    // create_cow_btree / load_cow_btree / list_persisted_btrees
#include "homestore/index/cow_btree/cow_btree.h"
#include "homestore/index/cow_btree/cow_btree_mgr.ipp"  // create_cow_btree / load_cow_btree template bodies
#include "homestore/index/btree/btree.ipp"              // Btree<K,V> template body (put_one/get_one)
#include "homestore/index/btree/tests/btree_test_kvs.h" // TestFixedKey / TestFixedValue

namespace test_common {
using namespace homestore;

// One blob-dev + one COWBtree per replica process (a single replication group per process). Names/config are stable
// across restarts so recovery re-attaches to the same btree.
class CowBtreeStore : public TestStore {
public:
    // Short names: the blob dev's append-blk streams derive metablk names as "<dev_name>_appendblk_<ids>", which
    // must fit MetaBlkHeader's 31-byte cap — so keep the prefix tiny.
    explicit CowBtreeStore(uint16_t id) :
            blob_dev_name_{fmt::format("rcs{}", id)}, btree_name_{fmt::format("rcb{}", id)} {}

    // Create (first boot) or load (recovery) the btree. Must run after HomeStore has booted (blob_dev_mgr +
    // cow_btree_mgr live) and after device_mgr()/cow_btree_mgr() recovery has read the persisted metablks.
    Async< void > recover() override {
        BtreeConfig cfg{};
        cfg.btree_name_ = btree_name_;
        cfg.node_size_ = 4096;
        cfg.leaf_node_type_ = BtreeNodeType::FIXED;
        cfg.int_node_type_ = BtreeNodeType::FIXED;
        cfg.finalize(sizeof(NodeCore::PersistentHeader));

        // Reuse the one blob-dev across create/destroy cycles (there is no per-blob-dev destroy API); create it the
        // first time it's needed.  Being self-aware here — rather than keying off HomeStore's is_first_time_boot() —
        // lets many tests share one booted HomeStore: each creates/destroys its own btree independently.
        blob_dev_ = blob_dev_mgr().get_blob_dev(blob_dev_name_);
        if (!blob_dev_) {
            VDevParameters params{};
            params.initial_chunk_size = 32 * 1024 * 1024;
            params.blk_size = 4096;
            params.dev_type = HSDevType::Data;
            params.alloc_type = BlkAllocatorType::SlabCompact;
            params.chunk_sel_type = ChunkSelectorType::RoundRobin;
            blob_dev_ = co_await blob_dev_mgr().create_blob_dev(std::string{blob_dev_name_}, std::move(params));
            HS_REL_ASSERT(blob_dev_, "CowBtreeStore: create_blob_dev '{}' returned null", blob_dev_name_);
        }

        // Load my btree if it's persisted (a restart within the test), else create a fresh one (first boot, or after
        // a prior test destroyed it).  Match by name so btrees belonging to other tests are ignored.
        COWBtreeSuperBlock const* my_sb = nullptr;
        for (auto const* sb : cow_btree_mgr().list_persisted_btrees()) {
            if (btree_name_ == sb->btree_name) {
                my_sb = sb;
                break;
            }
        }
        if (my_sb) {
            bt_ = co_await cow_btree_mgr().load_cow_btree< TestFixedKey, TestFixedValue >(cfg, blob_dev_, *my_sb);
            LOGINFO("CowBtreeStore[{}]: loaded persisted btree", btree_name_);
        } else {
            bt_ = co_await cow_btree_mgr().create_cow_btree< TestFixedKey, TestFixedValue >(cfg, blob_dev_);
            LOGINFO("CowBtreeStore[{}]: created fresh btree", btree_name_);
        }
        HS_REL_ASSERT(bt_, "CowBtreeStore: btree null after recover");
        co_return;
    }

    // Drop the btree (streams + metablk) so the next test starts clean.  The blob-dev is reused (no per-dev destroy
    // API); size_ resets for the next lifecycle.
    Async< void > destroy() override {
        if (bt_) {
            cshared< BtreeBase > base = bt_;
            co_await cow_btree_mgr().destroy_cow_btree(base);
            bt_.reset();
        }
        size_.store(0, std::memory_order_relaxed);
        co_return;
    }

    Async< void > apply(uint64_t key, uint32_t value) override {
        TestFixedKey k{key};
        TestFixedValue v{value};
        auto const r = co_await bt_->put_one(k, v, BtreePutType::UPSERT, nullptr, nullptr);
        HS_REL_ASSERT(r.hasValue(), "CowBtreeStore: put_one failed for key={}", key);
        auto const n = size_.fetch_add(1, std::memory_order_relaxed) + 1;
        LOGINFO("CowBtreeStore[{}]: applied key={} val={} store_size={}", btree_name_, key, value, n);
        co_return;
    }

    Async< std::optional< uint32_t > > lookup(uint64_t key) override {
        TestFixedKey k{key};
        auto const r = co_await bt_->get_one(k);
        if (!r.hasValue()) {
            co_return std::nullopt;
        }
        co_return r.value().value();
    }

    uint64_t size() const override { return size_.load(std::memory_order_relaxed); }

    // Force the btree's dirty state to disk so a subsequent crash/restart recovers it. Triggers a CP flush.
    Async< void > checkpoint() override {
        co_await cp_mgr().trigger_cp_flush(true /* force */);
        co_return;
    }

private:
    std::string blob_dev_name_;
    std::string btree_name_;
    shared< BlobDev > blob_dev_;
    shared< Btree< TestFixedKey, TestFixedValue > > bt_;
    std::atomic< uint64_t > size_{0};
};

shared< TestStore > make_cow_btree_store(uint16_t id) {
    return std::make_shared< CowBtreeStore >(id);
}

} // namespace test_common
