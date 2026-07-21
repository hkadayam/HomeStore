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

// Forward-declared so InputParams can carry an optional ReplApplication without pulling the replication
// headers into the core.  shared<> keeps the deleter type-erased, so a forward declaration is enough here.
class ReplApplication;

// ─────────────────────────────────────────────────────────────────────────────
// First-time-boot format inputs.  Only HomeStore-level format-time params live here: per-blob-device and
// per-cow-btree params are passed by the application later when it calls the corresponding manager's
// create_<resource>(...) API.  Device SIZES are NOT here — they are auto-detected from the physical device
// (DevInfo::dev_size == 0 means "use the whole device"), so this struct is fully defaulted and never carries
// per-device sizing.
//
// All chunk_size fields are "the chunk_size each manager's vdev grows in" — managers always add new chunks of
// this size via VirtualDev::expand() as space fills.
// ─────────────────────────────────────────────────────────────────────────────
struct FormatOpts {
    uint64_t meta_chunk_size{16ull * 1024 * 1024};     // 16 MB
    uint64_t logstore_chunk_size{64ull * 1024 * 1024}; // 64 MB
    uint32_t logstore_initial_num_chunks{4};
};

// ─────────────────────────────────────────────────────────────────────────────
// Boot-time inputs (every mount).  The SAME struct feeds both first-time boot and recovery — device sizing is
// automatic, so there is no format-only device surface anymore.  format_opts is consulted ONLY when start()
// reports first-time boot and is ignored on recovery; repl_app being non-null is what enables replication.
// ─────────────────────────────────────────────────────────────────────────────
struct InputParams {
    std::vector< DevInfo > devices;
    IOFlag data_open_flags{IOFlag::DIRECT_IO};
    IOFlag fast_open_flags{IOFlag::DIRECT_IO};
    bool is_read_only{false};

    // Replication is OPTIONAL and self-gating: a non-null repl_app brings up the replication service (rpc
    // listener, executors, raft engines); a null repl_app skips it entirely — no threads, no bound port.  A
    // recovery that finds persisted replica-set SBs with a null repl_app is an error (groups nobody can host).
    shared< ReplApplication > repl_app;

    // First-time-boot-only format knobs.  Consulted by format(); ignored by load().
    FormatOpts format_opts{};
};

class HomeStore;
using HomeStoreSafePtr = shared< HomeStore >;

// ─────────────────────────────────────────────────────────────────────────────
// HomeStore
//
// Thin orchestrator over the per-subsystem managers (Device, MetaBlk, CP, BlobDev, COWBtree, LogStore,
// Replication, Resource).  Boot is phased so the application can recover its OWN MetaBlks between the
// reconstruct phase and the replay phase:
//
//   bool first = co_await start(input);          // bootstrap + first-boot detection, nothing else
//   if (first) {
//       co_await format();                        // fresh store: create every manager AND go live (no replay)
//   } else {
//       co_await load();                          // restart: reconstruct every manager (no engines, no replay)
//       co_await app->recover();                  // app rebuilds its durable state, binds listeners
//       co_await replay();                        // recovery go-live: log replay + engines + init
//   }
//
// Or, for callers/tests that do not need the recovery hook, the one-shot convenience wrapper:
//   co_await boot(input);                         // start + format (first boot) OR load + replay (recovery)
//   co_await boot(input, [&]{ return app->recover(); });   // ...with the app recovery hook on the recovery path
//
// Once up, callers reach each manager via the free-function accessors in managers.h (`meta_mgr()`,
// `cp_mgr()`, `repl_mgr()`, ...).
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

    // ── Phased boot API ───────────────────────────────────────────────────────

    /// Bootstrap: process-once setup + construct the DeviceManager (which probes the device headers).  Brings
    /// up nothing recoverable.  Returns true on first-time boot (caller follows up with format() then replay()),
    /// false on recovery (caller follows up with load(), its own recovery, then replay()).
    Async< bool > start(InputParams input);

    /// First-time boot: format devices, create every manager fresh (using input_.format_opts), and go fully
    /// live (CP timer + init_done).  A fresh store has nothing to replay, so there is NO replay() phase on the
    /// first-boot path.
    Async< void > format();

    /// Recovery: reconstruct every manager from disk (replica sets reconstructed but NOT started; log stores
    /// reconstructed but NOT replayed).  Does NOT go live — that is replay().
    Async< void > load();

    /// Recovery-only go-live: drive the deferred log replay, bring raft engines up against the replayed log,
    /// seal a recovery-baseline CP, start the CP timer, and mark the store initialized.  Not used on first boot
    /// (format() goes live on its own).
    Async< void > replay();

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
