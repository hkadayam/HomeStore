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

#ifndef _HOMESTORE_CONFIG_HPP_
#define _HOMESTORE_CONFIG_HPP_

#include <array>
#include <cassert>
#include <cstdint>
#include <sstream>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>
#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "sisl/settings/settings.h"
#include "sisl/fds/enum.h"

#include "homestore/base/homestore_decl.h"
#include "error.h"
#include "homestore/base/generated/homestore_config_generated.h"

SETTINGS_INIT(homestorecfg::HomeStoreSettings, homestore_config);

// DM info size depends on these three parameters. If below parameter changes then we have to add
// the code for upgrade/revert.

namespace homestore {
#define HS_RUNTIME_CONFIG_WITH(...) SETTINGS(homestore_config, __VA_ARGS__)
#define HS_RUNTIME_CONFIG_THIS(...) SETTINGS_THIS(homestore_config, __VA_ARGS__)
#define HS_RUNTIME_CONFIG_WITH_CAP(...) SETTINGS_THIS_CAP1(homestore_config, __VA_ARGS__)
#define HS_RUNTIME_CONFIG(...) SETTINGS_VALUE(homestore_config, __VA_ARGS__)

#define HS_SETTINGS_FACTORY() SETTINGS_FACTORY(homestore_config)

class HomeStoreRuntimeConfig {
public:
    static const std::array< double, 9 >& default_slab_distribution() {
        // Assuming blk_size=4K [4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M ]
        static constexpr std::array< double, 9 > slab_distribution{15.0, 7.0, 7.0, 6.0, 10.0, 10.0, 10.0, 10.0, 25.0};
        return slab_distribution;
    }

    static void init_settings_default() {
        // Non-scalar defaults are now self-initialized by their respective subsystems (e.g. SlabBlkAllocConfig
        // auto-populates slab_distribution). This method is retained for any future overrides.
    }
};
} // namespace homestore

#endif
