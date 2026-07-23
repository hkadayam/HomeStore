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
 *
 ***************************************************************************/
#pragma once

#include "common/async.h"
#include "homestore/logstore/log_store_mgr.h"
#include "homestore/test_common/hs_test_harness.h"

namespace homestore::test {

// Boot layer for LogStoreManager.  Passive: its own timers/flush are drained inside its shutdown().  Log stores
// are created/opened/recovered by the test body after start(), not here.
struct LogStoreSpec {
    static Async< void > start(BootCfg const& c) {
        if (c.format) {
            co_await LogStoreManager::create(c.logstore_chunk_size, c.logstore_initial_chunks);
        } else {
            co_await LogStoreManager::load();
        }
    }

    static Async< void > stop(BootCfg const&) { co_await log_store_mgr().shutdown(); }
};

} // namespace homestore::test
