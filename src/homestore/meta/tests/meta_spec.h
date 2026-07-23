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
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/test_common/hs_test_harness.h"

namespace homestore::test {

// Boot layer for MetaBlkManager.  Passive: no prepare(), and no explicit stop — Managers::reset() drops it.
struct MetaSpec {
    static Async< void > start(BootCfg const& c) {
        if (c.format) {
            co_await MetaBlkManager::create(c.meta_vdev_size);
        } else {
            co_await MetaBlkManager::load();
        }
    }

    static Async< void > stop(BootCfg const&) { co_return; }
};

} // namespace homestore::test
