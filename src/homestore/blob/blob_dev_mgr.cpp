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

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sisl/logging/logging.h"

#include <homestore/checkpoint/cp.h>     // CP
#include <homestore/checkpoint/cp_mgr.h> // CPManager

#include "blob/blob_dev.h"
#include "blob/blob_dev_mgr.h"
#include "blob/raw_blk_stream.h"
#include "blob/append_blk_stream.h"
#include "blob/append_byte_stream.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"
#include "meta/meta_client.hpp"
#include "managers.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// create
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDevManager::create() {
    LOGINFO("BlobDevManager: first boot — creating fresh manager");
    auto mgr = unique< BlobDevManager >{new BlobDevManager{co_await meta_mgr().register_client("BlobDevManager")}};
    mgr->register_with_cp_mgr();
    Managers::init_blob_dev_mgr(std::move(mgr));
    LOGINFO("BlobDevManager: ready");
}

// ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// load
//
// Scan all MetaBlks whose names match one of the three patterns:
//   <dev>_rawblk_1_<chunk_id>
//   <dev>_appendblk_2_<chunk_id>
//   <dev>_appendbyte_3_<chunk_id>
//
// Group by (dev_name, stream_type); validate each chunk exists in DeviceManager.  Then construct one BlobDev per
// dev_name and call load() with the recovered MetaBlk maps.  Streams read payload from MetaBlk on demand.
// ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace {

// Attempt to parse a MetaBlk name of the form "<dev>_<type_name>_<chunk_id>".
struct ParsedMblkName {
    std::string dev_name;
    StreamType stream_type;
    uint32_t chunk_id;
};

std::optional< ParsedMblkName > parse_mblk_name(std::string_view name) {
    static const std::pair< std::string_view, StreamType > kPatterns[] = {
        {"_rawblk_", StreamType::RawBlk},
        {"_appendblk_", StreamType::AppendBlk},
        {"_appendbyte_", StreamType::AppendByte},
    };

    for (auto& [suffix, stype] : kPatterns) {
        auto pos = name.rfind(suffix);
        if (pos == std::string_view::npos) {
            continue;
        }

        std::string_view dev = name.substr(0, pos);
        // Remainder after the suffix is just "<chunk_id>".
        std::string_view rest = name.substr(pos + suffix.size());

        uint32_t chunk_id{};
        auto rc = std::from_chars(rest.data(), rest.data() + rest.size(), chunk_id);
        if (rc.ec != std::errc{}) {
            continue;
        }

        return ParsedMblkName{std::string{dev}, stype, chunk_id};
    }
    return std::nullopt;
}

} // anonymous namespace

folly::coro::Task< void > BlobDevManager::load() {
    LOGINFO("BlobDevManager: starting recovery scan");
    auto mgr = unique< BlobDevManager >{new BlobDevManager{co_await meta_mgr().register_client("BlobDevManager")}};

    // Per-device recovery accumulator, keyed by stream type.
    using ChunkMblkMap = BlobDev::ChunkMblkMap;
    struct DevRecovery {
        ChunkMblkMap raw_blk_mblks;
        ChunkMblkMap append_blk_mblks;
        ChunkMblkMap append_byte_mblks;
    };

    std::unordered_map< std::string, DevRecovery > dev_map;

    co_await mgr->meta_client_.for_each_recovered_block(
        [&dev_map](MetaBlk blk, IOBuffer data) -> folly::coro::Task< void > {
            auto parsed = parse_mblk_name(blk.name());
            if (!parsed) {
                co_return;
            }

            LOGDEBUG("Recovered MetaBlk '{}' dev={} chunk_id={}", blk.name(), parsed->dev_name, parsed->chunk_id);

            auto* vdev = device_mgr().get_vdev(parsed->dev_name);
            if (!vdev) {
                LOGWARN("MetaBlk '{}' references unknown VDev '{}' — skipping", blk.name(), parsed->dev_name);
                co_return;
            }

            auto chunk = vdev->get_chunk(parsed->chunk_id);
            if (!chunk) {
                LOGERROR("MetaBlk '{}' references chunk_id={} not found in VDev '{}' — skipping", blk.name(),
                         parsed->chunk_id, parsed->dev_name);
                co_return;
            }

            DevRecovery& dev = dev_map[parsed->dev_name];
            auto entry = std::make_pair(std::move(blk), std::move(data));
            switch (parsed->stream_type) {
            case StreamType::RawBlk:
                dev.raw_blk_mblks.emplace(parsed->chunk_id, std::move(entry));
                break;
            case StreamType::AppendBlk:
                dev.append_blk_mblks.emplace(parsed->chunk_id, std::move(entry));
                break;
            case StreamType::AppendByte:
                dev.append_byte_mblks.emplace(parsed->chunk_id, std::move(entry));
                break;
            }
            co_return;
        });

    LOGINFO("scan complete — {} device(s) found", dev_map.size());

    // Construct one BlobDev per discovered dev_name.
    for (auto& [dev_name, recovery] : dev_map) {
        auto* vdev = device_mgr().get_vdev(dev_name);
        if (!vdev) {
            LOGWARN("VDev '{}' disappeared between scan and load — skipping", dev_name);
            continue;
        }

        LOGINFO("loading BlobDev '{}' — raw_blk={} append_blk={} append_byte={} chunk(s)", dev_name,
                recovery.raw_blk_mblks.size(), recovery.append_blk_mblks.size(), recovery.append_byte_mblks.size());

        auto device = std::make_shared< BlobDev >(dev_name, vdev->shared_from_this(), mgr->meta_client_);
        co_await device->load(std::move(recovery.raw_blk_mblks), std::move(recovery.append_blk_mblks),
                              std::move(recovery.append_byte_mblks));

        {
            std::lock_guard lk{mgr->devices_mutex_};
            mgr->devices_.emplace(dev_name, std::move(device));
        }
        LOGINFO("BlobDev '{}' loaded successfully", dev_name);
    }

    mgr->register_with_cp_mgr();
    Managers::init_blob_dev_mgr(std::move(mgr));
    LOGINFO("BlobDevManager: recovery complete — {} BlobDev(s) active", dev_map.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// register_with_cp_mgr
// ─────────────────────────────────────────────────────────────────────────────

void BlobDevManager::register_with_cp_mgr() {
    cp_mgr().register_consumer(/*CPConsumer=*/"BlobDevManager", unique< CPCallbacks >{this});
}

// ─────────────────────────────────────────────────────────────────────────────
// create_blob_dev
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< BlobDev > > BlobDevManager::create_blob_dev(std::string&& dev_name,
                                                                       VDevParameters&& params) {
    LOGINFOMOD(blob_dev, "Creating BlobDev '{}'", dev_name);
    params.vdev_name = dev_name;
    params.size_type = VDevSizeType::Dynamic;
    auto vdev = co_await device_mgr().create_vdev(std::move(params));
    auto device = std::make_shared< BlobDev >(std::move(dev_name), std::move(vdev), meta_client_);
    {
        auto guard = cp_mgr().cp_guard();
        std::lock_guard lk{devices_mutex_};
        devices_.emplace(device->name(), device);
    }
    LOGINFOMOD(blob_dev, "BlobDev '{}' created", device->name());
    co_return device;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_blob_dev
// ─────────────────────────────────────────────────────────────────────────────

shared< BlobDev > BlobDevManager::get_blob_dev(const std::string& dev_name) const {
    std::lock_guard lk{devices_mutex_};
    auto it = devices_.find(dev_name);
    return it != devices_.end() ? it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// CPCallbacks
// ─────────────────────────────────────────────────────────────────────────────

void BlobDevManager::on_switchover_cp(CP* cur_cp, CP* new_cp) {
    std::lock_guard lk{devices_mutex_};
    for (auto& [_, device] : devices_) {
        device->on_cp_switchover(cur_cp, new_cp);
    }
}

folly::coro::Task< bool > BlobDevManager::cp_flush(CP* cp) {
    std::unordered_map< std::string, shared< BlobDev > > devs;
    {
        std::lock_guard lk{devices_mutex_};
        devs = devices_;
    }

    for (auto& [_, dev] : devs) {
        if (auto s = dev->raw_blk_stream(); s && s->is_dirty(cp->id())) {
            co_await s->cp_flush(cp);
        }
        if (auto s = dev->append_blk_stream(); s && s->is_dirty(cp->id())) {
            co_await s->cp_flush(cp);
        }
        if (auto s = dev->append_byte_stream(); s && s->is_dirty(cp->id())) {
            co_await s->cp_flush(cp);
        }
    }

    co_return true;
}

void BlobDevManager::cp_cleanup(CP* /*cp*/) {
}

int BlobDevManager::cp_progress_percent() {
    return 0;
}

} // namespace homestore
