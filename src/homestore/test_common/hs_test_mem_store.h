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
// MemBtreeStore — the in-memory TestStore backend over the production MemBtree.  Contents are lost on restart, so
// it fits sync/liveness tests (bring-up, write, cross-replica convergence) but not restart-recovery.  A persistent
// COWBtree-backed store plugs in behind the same TestStore interface later.
//
// NOTE: this header pulls in MemBtree/btree, which are compiled in sync mode (BtreeTask<T> == T) to match the
// hs_mem_btree library it links against — so the btree ops below are plain synchronous calls, not co_awaits.
// Mixing in BTREE_ASYNC_MODE here would disagree with hs_mem_btree on NodeCore/BtreeSharedMutex layout (an ODR
// violation).  Only the TU that actually uses MemBtreeStore takes this dependency — code that only needs the
// interface includes hs_test_store.h instead.
//
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "common/async.h" // Async<>
#include "common/defs.h"  // shared<>

#include "homestore/test_common/hs_test_store.h" // TestStore interface
#include "homestore/index/btree/node_variant/simple_node.h"
#include "homestore/index/btree/node_variant/varlen_node.h"
#include "homestore/index/mem_btree/mem_btree.h"
#include "homestore/index/btree/btree.ipp"               // Btree<K,V> template body (put_one/get_one) at the call site
#include "homestore/index/btree/tests/btree_test_kvs.h" // TestFixedKey / TestFixedValue (global namespace)

namespace test_common {

// In-memory backend over the production MemBtree.  Contents are lost on restart, so it fits sync/liveness tests
// (bring-up, write, cross-replica convergence) but not restart-recovery tests.
class MemBtreeStore : public TestStore {
public:
    MemBtreeStore() {
        homestore::BtreeConfig cfg;
        cfg.node_size_ = 4096;
        cfg.leaf_node_type_ = homestore::BtreeNodeType::FIXED;
        cfg.int_node_type_ = homestore::BtreeNodeType::FIXED;
        cfg.finalize(sizeof(homestore::NodeCore::PersistentHeader));
        bt_ = homestore::MemBtree::create< TestFixedKey, TestFixedValue >(cfg);
    }

    Async< void > apply(uint64_t key, uint32_t value) override {
        TestFixedKey k{key};
        TestFixedValue v{value};
        auto const r = bt_->put_one(k, v, homestore::BtreePutType::UPSERT, nullptr, nullptr);
        RELEASE_ASSERT(r.hasValue(), "MemBtreeStore put_one failed for key={}", key);
        size_.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    Async< std::optional< uint32_t > > lookup(uint64_t key) override {
        TestFixedKey k{key};
        auto const r = bt_->get_one(k);
        if (!r.hasValue()) { co_return std::nullopt; }
        co_return r.value().value();
    }

    uint64_t size() const override { return size_.load(std::memory_order_relaxed); }

private:
    shared< homestore::Btree< TestFixedKey, TestFixedValue > > bt_;
    std::atomic< uint64_t > size_{0};
};

} // namespace test_common
