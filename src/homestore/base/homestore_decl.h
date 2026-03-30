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

#ifdef _PRERELEASE
#include "sisl/flip/flip.hpp"
#endif
#include <spdlog/fmt/fmt.h>
#include <nlohmann/json.hpp>

#include "common/defs.h"
#include "device/device_decl.h"

//
// This file contains declarations shared across homestore service layers and consumers above.
// Device-layer fundamentals (HSDevType, IOFlag, DevInfo) live in device/device_decl.h.
// Common smart-pointer aliases (shared<>, unique<>, intrusive<>, etc.) live in common/defs.h.
//

namespace homestore {

// ── Type aliases ─────────────────────────────────────────────────────────────────────────────────────────────────────
using seq_id_t = int64_t;
using uuid_t = boost::uuids::uuid;
using hs_uuid_t = time_t;
using stream_id_t = uint32_t;

// ── Size limits ──────────────────────────────────────────────────────────────────────────────────────────────────────
constexpr uint32_t BLK_NUM_BITS{32};
constexpr uint32_t NBLKS_BITS{8};
constexpr uint32_t CHUNK_NUM_BITS{8};
constexpr uint32_t BLKID_SIZE_BITS{BLK_NUM_BITS + NBLKS_BITS + CHUNK_NUM_BITS};
constexpr uint64_t MAX_CHUNK_ID{((uint64_cast(1) << CHUNK_NUM_BITS) - 2)};
constexpr uint64_t BLKID_SIZE{(BLKID_SIZE_BITS / 8) + (((BLKID_SIZE_BITS % 8) != 0) ? 1 : 0)};
constexpr uint32_t BLKS_PER_PORTION{1024};
constexpr uint32_t TOTAL_SEGMENTS{8};
constexpr uint64_t MAX_BLK_NUM_BITS_PER_CHUNK{((uint64_cast(1) << BLK_NUM_BITS) - 1)};

inline uint64_t MIN_DATA_CHUNK_SIZE(uint32_t blk_size) { return blk_size * BLKS_PER_PORTION * TOTAL_SEGMENTS; }
inline uint64_t MAX_DATA_CHUNK_SIZE(uint32_t blk_size) {
    return uint64_cast(sisl::round_down((MAX_BLK_NUM_BITS_PER_CHUNK * blk_size), MIN_DATA_CHUNK_SIZE(blk_size)));
}

constexpr uint32_t MAX_CHUNKS{128};
constexpr uint32_t HDD_MAX_CHUNKS{254};
constexpr uint32_t HS_MAX_CHUNKS{HDD_MAX_CHUNKS};
constexpr uint32_t MAX_VDEVS{16};
constexpr uint32_t MAX_PDEVS{8};
static constexpr uint32_t INVALID_PDEV_ID{std::numeric_limits< uint32_t >::max()};
static constexpr uint32_t INVALID_VDEV_ID{std::numeric_limits< uint32_t >::max()};
static constexpr uint32_t INVALID_CHUNK_ID{std::numeric_limits< uint32_t >::max()};
static constexpr uint32_t INVALID_DEV_ID{std::numeric_limits< uint32_t >::max()};
static constexpr uint64_t INVALID_DEV_OFFSET{std::numeric_limits< uint64_t >::max()};
constexpr uint16_t MAX_UUID_LEN{128};
static constexpr hs_uuid_t INVALID_SYSTEM_UUID{0};

// ── Legacy enums (used by old code and hs_format_params; new device/ code uses BlkAllocatorType, ChunkSelectorType
// from virtual_dev.h) ────────────────────────────────────────────────────────────────────────────────────────────────
ENUM(blk_allocator_type_t, uint8_t, none, fixed, varsize, append);
ENUM(chunk_selector_type_t, uint8_t,
     NONE,
     ROUND_ROBIN,
     CUSTOM,
     RANDOM,
     MOST_AVAILABLE_SPACE,
     ALWAYS_CALLER_CONTROLLED
);


// ── Homestore configuration structs ──────────────────────────────────────────────────────────────────────────────────
struct hs_format_params {
    HSDevType dev_type{HSDevType::Data};
    float size_pct;
    uint32_t num_chunks{1};
    uint64_t chunk_size{0};
    uint32_t block_size{0};
    blk_allocator_type_t alloc_type{blk_allocator_type_t::varsize};
    chunk_selector_type_t chunk_sel_type{chunk_selector_type_t::ROUND_ROBIN};
};

struct hs_input_params {
public:
    std::vector< DevInfo > devices;
    IOFlag data_open_flags{IOFlag::DIRECT_IO};
    IOFlag fast_open_flags{IOFlag::DIRECT_IO};

    uint64_t app_mem_size{static_cast< uint64_t >(1024) * static_cast< uint64_t >(1024) *
                          static_cast< uint64_t >(1024)};
    uint64_t hugepage_size{0};
    int max_data_size{0};
    int max_snapshot_batch_size{0};
    bool is_read_only{false};
    bool auto_recovery{true};

#ifdef _PRERELEASE
    bool force_reinit{false};
#endif

    nlohmann::json to_json() const;
    std::string to_string() const { return to_json().dump(4); }
    uint64_t io_mem_size() const { return (hugepage_size != 0) ? hugepage_size : app_mem_size; }
    bool has_fast_dev() const {
        return std::any_of(devices.begin(), devices.end(),
                           [](const DevInfo& d) { return d.dev_type == HSDevType::Fast; });
    }
};

struct hs_engine_config {
    uint64_t max_chunks{MAX_CHUNKS};
    uint64_t max_vdevs{MAX_VDEVS};
    uint64_t max_pdevs{MAX_PDEVS};
    uint32_t max_blks_in_blkentry{1};

    nlohmann::json to_json() const;
};

struct stream_info_t {
    uint32_t num_streams = 0;
    uint64_t stream_cur = 0;
    std::vector< stream_id_t > stream_id;
    std::vector< void* > chunk_list;
};

} // namespace homestore
