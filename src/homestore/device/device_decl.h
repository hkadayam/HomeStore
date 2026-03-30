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
 ***************************************************************************/
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include "sisl/fds/enum.h"

namespace homestore {

// ── Device enums ─────────────────────────────────────────────────────────────────────────────────────────────────────
ENUM(HSDevType, uint8_t, Data, Fast);

ENUM(IOFlag, uint8_t,
     BUFFERED_IO, // File-backed IO without O_DIRECT; typically used in tests.
     DIRECT_IO,   // Recommended production mode (O_DIRECT).
     READ_ONLY    // Read-only mode for post-mortem checks.
);

// ── DevInfo ──────────────────────────────────────────────────────────────────────────────────────────────────────────
struct DevInfo {
    explicit DevInfo(std::string name, HSDevType type = HSDevType::Data, uint64_t size = 0) :
            dev_name{std::move(name)}, dev_type{type}, dev_size{size} {}
    std::string to_string() const { return fmt::format("{} - {} size={}", dev_name, enum_name(dev_type), dev_size); }

    std::string dev_name;
    HSDevType dev_type;
    uint64_t dev_size{0};
};

// ── Formatting helpers ───────────────────────────────────────────────────────────────────────────────────────────────
static std::string _format_decimals(double val, const char* suffix) {
    return (val != (uint64_t)val) ? fmt::format("{:.2f}{}", val, suffix) : fmt::format("{}{}", val, suffix);
}

static std::string in_bytes(uint64_t sz) {
    static constexpr std::array< std::pair< uint64_t, const char* >, 5 > arr{
        std::make_pair(1, ""), std::make_pair(1024, "kb"), std::make_pair(1048576, "mb"),
        std::make_pair(1073741824, "gb"), std::make_pair(1099511627776, "tb")};

    const double size = (double)sz;
    for (size_t i{1}; i < arr.size(); ++i) {
        if ((size / arr[i].first) < 1) { return _format_decimals(size / arr[i - 1].first, arr[i - 1].second); }
    }
    return _format_decimals(size / arr.back().first, arr.back().second);
}

} // namespace homestore
