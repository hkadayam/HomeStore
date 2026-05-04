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
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <homestore/base/blk.h> // BlkId
#include <homestore/base/crc.h> // crc32_ieee

namespace homestore {

// ──────────────────────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t META_CLIENT_INFO_MAGIC = 0xCEEDBEEDu;
static constexpr uint16_t META_CLIENT_INFO_VERSION = 0x1u;
static constexpr size_t META_CLIENT_INFO_SIZE = 512;
static constexpr size_t MAX_CLIENT_NAME_LEN = 232;
static constexpr size_t MAX_META_CLIENTS = 255;

// ──────────────────────────────────────────────────────────────────────────────
// MetaClientInfo
//
// On-disk record for one registered MetaClient.  Exactly 512 bytes so that
// the client-info area (super-header + MAX_META_CLIENTS slots) aligns cleanly
// to the device block size.
//
// Layout: 4+2+1+1+4+8+4+232+256 = 512 bytes.
// ──────────────────────────────────────────────────────────────────────────────
#pragma pack(1)
struct MetaClientInfo {
    uint32_t magic{0};                       // META_CLIENT_INFO_MAGIC
    uint16_t version{0};                     // META_CLIENT_INFO_VERSION
    uint8_t client_id{0};                    // Slot index (0 … MAX_META_CLIENTS-1)
    uint8_t slot_allocated{0};               // 1 = allocated, 0 = free
    uint8_t padding1[4]{};                   // Alignment padding
    BlkId first_blkid{};                     // Head of this client's meta-block chain
    uint32_t crc{0};                         // CRC32 over the whole struct (crc field = 0)
    char client_name[MAX_CLIENT_NAME_LEN]{}; // Name, null-terminated
    uint8_t padding2[256]{};                 // Pad to exactly 512 bytes

    static constexpr size_t SIZE = META_CLIENT_INFO_SIZE;

    // ── Lifecycle helpers ─────────────────────────────────────────────────────

    static MetaClientInfo make_free() {
        MetaClientInfo info;
        info.magic = META_CLIENT_INFO_MAGIC;
        info.version = META_CLIENT_INFO_VERSION;
        info.update_crc();
        return info;
    }

    // ── Allocation state ──────────────────────────────────────────────────────

    bool is_allocated() const { return slot_allocated == 1 && magic == META_CLIENT_INFO_MAGIC && validate_crc(); }

    void set_allocated() {
        magic = META_CLIENT_INFO_MAGIC;
        slot_allocated = 1;
        update_crc();
    }

    void set_free() {
        slot_allocated = 0;
        update_crc();
    }

    // ── CRC ───────────────────────────────────────────────────────────────────

    void update_crc() {
        crc = 0;
        crc = crc32_ieee(0, reinterpret_cast< const unsigned char* >(this), SIZE);
    }

    bool validate_crc() const {
        MetaClientInfo copy = *this;
        copy.crc = 0;
        uint32_t computed = crc32_ieee(0, reinterpret_cast< const unsigned char* >(&copy), SIZE);
        return computed == crc;
    }

    // ── Name helpers ──────────────────────────────────────────────────────────

    void set_client_name(std::string_view name) {
        size_t len = std::min(name.size(), MAX_CLIENT_NAME_LEN - 1);
        std::memcpy(client_name, name.data(), len);
        client_name[len] = '\0';
        update_crc();
    }

    std::string get_client_name() const {
        size_t null_pos = 0;
        while (null_pos < MAX_CLIENT_NAME_LEN && client_name[null_pos] != '\0')
            ++null_pos;
        return std::string(client_name, null_pos);
    }
};
#pragma pack()

static_assert(sizeof(MetaClientInfo) == META_CLIENT_INFO_SIZE,
              "MetaClientInfo must be exactly META_CLIENT_INFO_SIZE bytes");

} // namespace homestore
