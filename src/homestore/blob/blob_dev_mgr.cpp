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

#include "homestore/checkpoint/cp.h"     // CP
#include "homestore/checkpoint/cp_mgr.h" // CPManager

#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"
#include "homestore/blob/raw_blk_stream.h"
#include "homestore/blob/append_blk_stream.h"
#include "homestore/blob/append_byte_stream.h"
#include "homestore/device/chunk.h"
#include "homestore/device/device_manager.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/meta/meta_client.h"
#include "homestore/managers.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// create
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > BlobDevManager::create() {
    LOGINFO("BlobDevManager: first boot — creating fresh manager");
    auto mgr = shared< BlobDevManager >(new BlobDevManager{co_await meta_mgr().register_client("BlobDevManager")});
    Managers::init_blob_dev_mgr(mgr);
    cp_mgr().register_consumer("BlobDevManager", mgr->shared_from_this());
    LOGINFO("BlobDevManager: ready");
}

// ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// load
//
// Scan all MetaBlks whose names match one of the three patterns:
//   <dev>_rawblk_<stream_id>_<chunk_id>
//   <dev>_appendblk_<stream_id>_<chunk_id>
//   <dev>_appendbyte_<stream_id>_<chunk_id>
//
// Group by (dev_name, stream_type, stream_id); validate each chunk exists in DeviceManager.  Then construct one
// BlobDev per dev_name and call load() with the recovered MetaBlk maps.  Streams read payload from MetaBlk on demand.
// ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace {

// Parsed MetaBlk name.  Two supported forms:
//   Per-chunk (RawBlk/AppendBlk): "<dev>_<type>_<stream_id>_<chunk_id>_<blk_size>"      (is_sb=false)
//   Per-stream sb (AppendByte):   "<dev>_appendbyte_sb_<stream_id>"                     (is_sb=true)
struct ParsedMblkName {
    std::string dev_name;
    StreamType stream_type;
    uint64_t stream_id;
    bool is_sb{false};    // true for the single per-stream sb MetaBlk
    uint32_t chunk_id{0}; // valid only when !is_sb
    uint32_t blk_size{0}; // valid only when !is_sb (0 means use vdev default)
};

std::optional< ParsedMblkName > parse_mblk_name(std::string_view name) {
    // Per-stream sb form: "<dev>_appendbyte_sb_<stream_id>".
    {
        constexpr std::string_view sb_suffix = "_appendbyte_sb_";
        auto pos = name.rfind(sb_suffix);
        if (pos != std::string_view::npos) {
            std::string_view dev = name.substr(0, pos);
            std::string_view sid_part = name.substr(pos + sb_suffix.size());
            uint64_t stream_id{};
            auto rc = std::from_chars(sid_part.data(), sid_part.data() + sid_part.size(), stream_id);
            if (rc.ec == std::errc{} && rc.ptr == sid_part.data() + sid_part.size()) {
                return ParsedMblkName{std::string{dev}, StreamType::AppendByte, stream_id, /*is_sb=*/true};
            }
        }
    }

    // Per-chunk form: "<dev>_<type>_<stream_id>_<chunk_id>_<blk_size>".
    static const std::pair< std::string_view, StreamType > kPatterns[] = {
        {"_rawblk_", StreamType::RawBlk},
        {"_appendblk_", StreamType::AppendBlk},
    };

    for (auto& [suffix, stype] : kPatterns) {
        auto pos = name.rfind(suffix);
        if (pos == std::string_view::npos) {
            continue;
        }

        std::string_view dev = name.substr(0, pos);
        // Remainder after the suffix is "<stream_id>_<chunk_id>_<blk_size>".
        std::string_view rest = name.substr(pos + suffix.size());

        auto sep = rest.find('_');
        if (sep == std::string_view::npos) {
            continue;
        }

        uint64_t stream_id{};
        auto rc1 = std::from_chars(rest.data(), rest.data() + sep, stream_id);
        if (rc1.ec != std::errc{}) {
            continue;
        }

        std::string_view after_sid = rest.substr(sep + 1);
        auto sep2 = after_sid.find('_');
        if (sep2 == std::string_view::npos) {
            continue;
        }

        uint32_t chunk_id{};
        auto rc2 = std::from_chars(after_sid.data(), after_sid.data() + sep2, chunk_id);
        if (rc2.ec != std::errc{}) {
            continue;
        }

        std::string_view bs_part = after_sid.substr(sep2 + 1);
        uint32_t blk_size{0};
        auto rc3 = std::from_chars(bs_part.data(), bs_part.data() + bs_part.size(), blk_size);
        if (rc3.ec != std::errc{}) {
            continue;
        }

        return ParsedMblkName{std::string{dev}, stype, stream_id, /*is_sb=*/false, chunk_id, blk_size};
    }
    return std::nullopt;
}

} // anonymous namespace

folly::coro::Task< void > BlobDevManager::load() {
    LOGINFO("BlobDevManager: starting recovery scan");
    auto mgr = shared< BlobDevManager >(new BlobDevManager{co_await meta_mgr().register_client("BlobDevManager")});

    // Per-device recovery accumulator.
    using StreamMblkMap = BlobDev::StreamMblkMap;
    using AppendByteSbMap = BlobDev::AppendByteSbMap;
    struct DevRecovery {
        StreamMblkMap raw_blk_mblks;
        StreamMblkMap append_blk_mblks;
        AppendByteSbMap append_byte_sbs;
    };

    std::unordered_map< std::string, DevRecovery > dev_map;

    co_await mgr->meta_client_.for_each_recovered_block(
        [&dev_map](MetaBlk blk, sisl::ByteView data) -> folly::coro::Task< void > {
            auto parsed = parse_mblk_name(blk.name());
            if (!parsed) {
                co_return;
            }

            LOGDEBUG("Recovered MetaBlk '{}' dev={} stream_id={} chunk_id={} is_sb={}", blk.name(), parsed->dev_name,
                     parsed->stream_id, parsed->chunk_id, parsed->is_sb);

            auto vdev = device_mgr().get_vdev(parsed->dev_name);
            if (!vdev) {
                LOGWARN("MetaBlk '{}' references unknown VDev '{}' — skipping", blk.name(), parsed->dev_name);
                co_return;
            }

            DevRecovery& dev = dev_map[parsed->dev_name];

            if (parsed->is_sb) {
                // Single per-stream sb MetaBlk (AppendByteStream) — no chunk lookup here; chunk_ids are inside the
                // sb payload and resolved when the stream itself loads.
                dev.append_byte_sbs[parsed->stream_id] = BlobDev::AppendByteSbInfo{std::move(blk), std::move(data)};
                co_return;
            }

            // Per-chunk MetaBlk — validate the chunk still exists.
            auto chunk = vdev->get_chunk(parsed->chunk_id);
            if (!chunk) {
                LOGERROR("MetaBlk '{}' references chunk_id={} not found in VDev '{}' — skipping", blk.name(),
                         parsed->chunk_id, parsed->dev_name);
                co_return;
            }

            auto entry = std::make_pair(std::move(blk), std::move(data));
            auto populate = [&](StreamMblkMap& m) {
                auto& info = m[parsed->stream_id];
                info.blk_size = parsed->blk_size;
                info.chunk_mblks.emplace(parsed->chunk_id, std::move(entry));
            };
            switch (parsed->stream_type) {
            case StreamType::RawBlk:
                populate(dev.raw_blk_mblks);
                break;
            case StreamType::AppendBlk:
                populate(dev.append_blk_mblks);
                break;
            case StreamType::AppendByte:
                // Per-chunk form is not produced for AppendByte — fall through without populating.
                break;
            }
            co_return;
        });

    LOGINFO("scan complete — {} device(s) found", dev_map.size());

    // Construct one BlobDev per discovered dev_name.
    for (auto& [dev_name, recovery] : dev_map) {
        auto vdev = device_mgr().get_vdev(dev_name);
        if (!vdev) {
            LOGWARN("VDev '{}' disappeared between scan and load — skipping", dev_name);
            continue;
        }

        LOGINFO("loading BlobDev '{}' — raw_blk={} append_blk={} append_byte={} stream(s)", dev_name,
                recovery.raw_blk_mblks.size(), recovery.append_blk_mblks.size(), recovery.append_byte_sbs.size());

        auto device = std::make_shared< BlobDev >(dev_name, vdev, mgr->meta_client_);
        co_await device->load(std::move(recovery.raw_blk_mblks), std::move(recovery.append_blk_mblks),
                              std::move(recovery.append_byte_sbs));

        {
            std::lock_guard lk{mgr->devices_mutex_};
            mgr->devices_.emplace(dev_name, std::move(device));
        }
        LOGINFO("BlobDev '{}' loaded successfully", dev_name);
    }

    Managers::init_blob_dev_mgr(mgr);
    cp_mgr().register_consumer("BlobDevManager", mgr->shared_from_this());
    LOGINFO("BlobDevManager: recovery complete — {} BlobDev(s) active", dev_map.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// create_blob_dev
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< shared< BlobDev > > BlobDevManager::create_blob_dev(std::string&& dev_name,
                                                                       VDevParameters&& params) {
    LOGINFOMOD(blob_dev, "Creating BlobDev '{}'", dev_name);
    params.vdev_name = dev_name;
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
// shutdown
// ─────────────────────────────────────────────────────────────────────────────

void BlobDevManager::shutdown() {
    {
        std::lock_guard lk{devices_mutex_};
        devices_.clear();
    }
    Managers::init_blob_dev_mgr(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// CPCallbacks
// ─────────────────────────────────────────────────────────────────────────────

void BlobDevManager::on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) {
}

folly::coro::Task< bool > BlobDevManager::cp_flush(CP* cp) {
    std::unordered_map< std::string, shared< BlobDev > > devs;
    {
        std::lock_guard lk{devices_mutex_};
        devs = devices_;
    }

    for (auto& [_, dev] : devs) {
        co_await dev->cp_flush(cp);
    }

    co_return true;
}

void BlobDevManager::cp_cleanup(CP* /*cp*/) {
}

int BlobDevManager::cp_progress_percent() {
    return 0;
}

} // namespace homestore
