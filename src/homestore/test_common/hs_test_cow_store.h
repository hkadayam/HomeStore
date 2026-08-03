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
// Durable COWBtree-backed TestStore. Unlike MemBtreeStore, its btree lives on the blob device and survives restart,
// so it is the backend for restart/crash-recovery replication tests. The concrete implementation (which instantiates
// the btree templates) lives in hs_test_cow_store.cpp — the only TU that pays that compile cost — so category test
// files depend on this thin factory + the TestStore interface only.
//
// Lifecycle: the btree needs blob_dev_mgr + cow_btree_mgr, which exist only after HomeStore boots. So construction is
// cheap and TestStore::recover() does the real work post-boot: create a fresh btree on first boot, or load the
// persisted one (looked up by the store's stable per-replica uuid) on recovery.
//
#pragma once

#include "common/async.h" // Async<>
#include "common/defs.h"  // shared<>

#include "homestore/test_common/hs_test_store.h" // TestStore interface

namespace test_common {

// `id` is a stable per-replica identifier (e.g. replica_num) used to derive a deterministic btree uuid + blob-dev
// name so the same store is re-attached across restarts.
shared< TestStore > make_cow_btree_store(uint16_t id);

} // namespace test_common
