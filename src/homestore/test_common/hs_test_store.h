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
// TestStore — a pluggable committed key→value store for HomeStore tests: a test applies entries into it and reads
// them back to validate.  apply()/lookup() are coroutines so a backend can do real async IO.  This header is the
// interface ONLY (lightweight — no btree), so harness/integ code can hold a shared<TestStore> without pulling in a
// concrete backend.  Concrete backends: MemBtreeStore (hs_test_mem_store.h), and later a COWBtree-backed store.
//
#pragma once

#include <cstdint>
#include <optional>

#include "common/async.h" // Async<>

namespace test_common {

class TestStore {
public:
    virtual ~TestStore() = default;

    virtual Async< void > apply(uint64_t key, uint32_t value) = 0;
    virtual Async< std::optional< uint32_t > > lookup(uint64_t key) = 0;
    virtual uint64_t size() const = 0;

    // Persistence hooks — no-ops for in-memory backends, meaningful for the COWBtree-backed store.
    virtual Async< void > checkpoint() { co_return; }
    virtual Async< void > recover() { co_return; }
    // Tear down the durable state (e.g. drop the btree) so a subsequent test starts clean on the shared HomeStore.
    virtual Async< void > destroy() { co_return; }
};

} // namespace test_common
