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
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_hash.hpp>
#include "sisl/fds/enum.h"
#include "sisl/fds/utils.h"

#include "homestore/base/hs_compile_config.h"

#ifdef _PRERELEASE
#include "sisl/flip/flip.hpp"
#endif
#include <spdlog/fmt/fmt.h>
#include <nlohmann/json.hpp>

#include "common/defs.h"
#include "homestore/device/device_decl.h"

//
// This file contains declarations shared across homestore service layers and consumers above.
// Device-layer fundamentals (HSDevType, IOFlag, DevInfo) live in device/device_decl.h.
// Common smart-pointer aliases (shared<>, unique<>, intrusive<>, etc.) live in common/defs.h.
//

namespace homestore {

// ── Type aliases ─────────────────────────────────────────────────────────────────────────────────────────────────────
using uuid_t = boost::uuids::uuid;
using stream_id_t = uint32_t;

// System-wide structural limits (MAX_VDEVS_IN_SYSTEM, MAX_CHUNKS_IN_SYSTEM, MIN_CHUNK_SIZE_*) live in
// hs_compile_config.h, included above; the BlkId on-disk encoding is owned by blk.h.  The per-device
// min-chunk selection is HSSuperBlk::min_chunk_size().

} // namespace homestore
