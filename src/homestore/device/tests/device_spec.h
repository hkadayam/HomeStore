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
#include "homestore/device/device_manager.h"
#include "homestore/managers.h"
#include "homestore/test_common/hs_test_harness.h"

namespace homestore::test {

// Boot layer for DeviceManager.  create()/create_and_format() self-install into Managers, so device_mgr() is live
// after start() — the Spec keeps no handle.
struct DeviceSpec {
    static Async< void > start(BootCfg const& c) {
        if (c.format) {
            co_await DeviceManager::create_and_format(c.dev_infos(), c.data_open_flags, c.fast_open_flags);
        } else {
            DeviceManager::create(c.dev_infos(), c.data_open_flags, c.fast_open_flags);
            co_await device_mgr().load_devices();
        }
    }

    static Async< void > stop(BootCfg const&) { co_await device_mgr().close_devices(); }
};

} // namespace homestore::test
