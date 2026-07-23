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
#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/test_common/hs_test_harness.h"

namespace homestore::test {

// Boot layer for CPManager.  Active: prepare() runs the phase-1 quiesce (final flush + stop timer) before the
// reverse teardown, so consumers are still alive when CP flushes them.  The timer is left off (start_timer not
// called) — tests drive flushes explicitly via trigger_cp_flush.
struct CpSpec {
    static Async< void > start(BootCfg const& c) {
        auto cp = CPManager::create();
        co_await cp->start(c.format);
    }

    static Async< void > prepare(BootCfg const&) { co_await cp_mgr().prepare_shutdown(); }

    static Async< void > stop(BootCfg const&) { co_await cp_mgr().shutdown(); }
};

} // namespace homestore::test
