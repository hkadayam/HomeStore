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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

#include "common/async.h"

#include "common/defs.h"                   // shared<>, unique<>
#include "homestore/base/homestore_decl.h" // DevInfo, IOFlag, HSDevType

namespace homestore {

#ifdef _PRERELEASE
class CrashSimulator;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Memory budget — variant of either an absolute byte count or a fraction of system RAM.
// Default is ProportionalMem{0.5}.  Consumers (homeblks, homedb etc) pick which form.
// ─────────────────────────────────────────────────────────────────────────────
struct AbsoluteMem {
    uint64_t bytes;
};
struct ProportionalMem {
    double fraction; // 0.0–1.0 of total system RAM
};
using AppMemSize = std::variant< AbsoluteMem, ProportionalMem >;

// ─────────────────────────────────────────────────────────────────────────────
// Boot-time inputs (every mount).  No format-time-only knobs here — those live in FormatOpts.
// ─────────────────────────────────────────────────────────────────────────────
struct InputParams {
    std::vector< DevInfo > devices;
    IOFlag data_open_flags{IOFlag::DIRECT_IO};
    IOFlag fast_open_flags{IOFlag::DIRECT_IO};
    AppMemSize mem_size{ProportionalMem{0.5}};
    bool is_read_only{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// First-time-boot format inputs.  Only HomeStore-level format-time params live here:
// per-blob-device and per-cow-btree params are passed by the application later when it calls
// the corresponding manager's create_<resource>(...) API.
//
// All chunk_size fields are "the chunk_size each manager's vdev grows in" — managers always add
// new chunks of this size via VirtualDev::expand() as space fills.
// ─────────────────────────────────────────────────────────────────────────────
struct FormatOpts {
    uint64_t meta_chunk_size{16ull * 1024 * 1024};     // 16 MB
    uint64_t logstore_chunk_size{64ull * 1024 * 1024}; // 64 MB
    uint32_t logstore_initial_num_chunks{4};
};

class HomeStore;
using HomeStoreSafePtr = shared< HomeStore >;

// ─────────────────────────────────────────────────────────────────────────────
// HomeStore
//
// Thin orchestrator over the per-subsystem managers (Device, MetaBlk, CP, BlobDev, COWBtree,
// LogStore, Resource).  HomeStore::start() brings them up in the correct order; on first-time
// boot the caller follows up with format_and_start(FormatOpts).  Once up, callers reach each
// manager via the free-function accessors in managers.h (`meta_mgr()`, `cp_mgr()`, ...).
//
// ─────────────────────────────────────────────────────────────────────────────
class HomeStore {
public:
    static HomeStore* instance();
    static HomeStoreSafePtr safe_instance() { return s_instance_; }
    static void reset_instance() { s_instance_.reset(); }

    HomeStore() = default;
    virtual ~HomeStore() = default;
    HomeStore(const HomeStore&) = delete;
    HomeStore& operator=(const HomeStore&) = delete;
    HomeStore(HomeStore&&) = delete;
    HomeStore& operator=(HomeStore&&) = delete;

    /// Boot the system.  Returns true on first-time boot (caller MUST follow up with
    /// format_and_start(FormatOpts)); false if recovery completed and HomeStore is fully up.
    Async< bool > start(InputParams input);

    /// First-time-boot only.  Formats devices, creates each manager's vdev, brings managers up,
    /// commits formatting.  Do NOT call on subsequent mounts — start() handles those itself.
    Async< void > format_and_start(FormatOpts opts);

    /// Tear down all managers in reverse start order.  Idempotent.
    Async< void > shutdown();

    bool is_first_time_boot() const;
    bool is_initialized() const { return init_done_.load(std::memory_order_acquire); }

#ifdef _PRERELEASE
    HomeStore& with_crash_simulator(std::function< void() > restart_cb);
    CrashSimulator& crash_simulator() {
        return *crash_simulator_;
    }
#endif

private:
    /// Resolve InputParams.mem_size variant to a concrete byte budget for ResourceMgr.
    static uint64_t resolve_mem_cap(AppMemSize const& mem);

private:
    static HomeStoreSafePtr s_instance_;
    std::atomic< bool > init_done_{false};
    InputParams input_{};

#ifdef _PRERELEASE
    unique< CrashSimulator > crash_simulator_;
#endif
};

inline HomeStore* hs() {
    return HomeStore::instance();
}

} // namespace homestore