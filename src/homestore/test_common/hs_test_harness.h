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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include "common/async.h"
#include "iomanager/iomanager.h"

#include "homestore/base/homestore_decl.h" // DevInfo, HSDevType, IOFlag
#include "homestore/managers.h"

namespace homestore::test {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// BootCfg
//
// One plain aggregate carrying everything a layered HomeStore test stack needs to boot.  Every field has a default
// for the common case, so a test overrides only what differs, e.g.:
//     BootCfg{.num_devs = 3}
//     BootCfg{.logstore_chunk_size = 8 * 1024 * 1024}
//
// The `format` flag is managed by the Harness: true on the first start(), flipped to false on restart() so each
// Spec picks its create-vs-load path from it.  Each layer's Spec reads only the fields it needs.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct BootCfg {
    // ── Devices / iomgr ──────────────────────────────────────────────────────
    uint32_t num_devs{2};
    uint64_t dev_size{256ull * 1024 * 1024};
    HSDevType dev_type{HSDevType::Data};
    IOFlag data_open_flags{IOFlag::BUFFERED_IO};
    IOFlag fast_open_flags{IOFlag::BUFFERED_IO};
    std::string dev_prefix{"/tmp/hs_test"};
    uint32_t num_reactors{2};

    // ── Per-manager params (read only by the corresponding Spec) ─────────────
    uint64_t meta_vdev_size{64ull * 1024 * 1024};
    uint64_t logstore_chunk_size{4ull * 1024 * 1024};
    uint32_t logstore_initial_chunks{1};

    // ── Managed by the Harness ───────────────────────────────────────────────
    bool format{true};                    // true = first boot (create); false = restart (load)
    std::vector< std::string > dev_paths; // filled in by Harness::create_dev_files()

    std::vector< DevInfo > dev_infos() const {
        std::vector< DevInfo > v;
        v.reserve(dev_paths.size());
        for (auto const& p : dev_paths) {
            v.emplace_back(p, dev_type, dev_size);
        }
        return v;
    }
};

// A Spec defines `prepare()` only if its manager needs a phase-1 quiesce (CP, ResourceMgr).  This concept lets the
// Harness call prepare() on exactly those specs and compile-time-skip the rest.
template < class S >
concept HasPrepare = requires(BootCfg const& c) { S::prepare(c); };

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Harness< Specs... >
//
// A test's HomeStore stack, expressed as the list of manager Specs it boots — e.g.
//     Harness< DeviceSpec, MetaSpec, CpSpec, LogStoreSpec >
//
// Drives the whole lifecycle from the MAIN thread (the model the restart-heavy tests already use): it owns iomgr,
// creates/removes the backing device files, and runs each Spec's coroutine boot/teardown on a reactor via
// spawn_and_block.  start() boots the Specs in listed order; stop()/restart() run a phase-1 prepare pass (only the
// Specs that define prepare(), in reverse order) followed by a phase-2 stop pass (all Specs, reverse order).
//
// restart() bounces iomgr (kills reactor thread-locals — notably CPManager's t_cp_info_ — that would otherwise
// dangle into freed managers) and re-boots with format=false.  The Managers::reset() position intentionally
// differs: on restart it runs on the reactor before stop_iomgr(); at final stop() it runs on main AFTER
// stop_iomgr() (so reactor TLS deleters fire while their owning containers are still alive).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
template < class... Specs >
class Harness {
public:
    explicit Harness(BootCfg cfg = {}) : cfg_{std::move(cfg)} {}

    BootCfg& cfg() { return cfg_; }
    BootCfg const& cfg() const { return cfg_; }

    /// First boot: init iomgr, create device files, boot all Specs (format=true) in listed order.  Call from a
    /// test fixture's SetUp() (main thread).
    void start() {
        iomanager::init_iomgr(cfg_.num_reactors);
        create_dev_files();
        cfg_.format = true;
        run_on_reactor(boot_< 0 >());
    }

    /// Final teardown: quiesce + stop all Specs (reverse), then stop iomgr, drop the managers, remove files.
    /// Call from TearDown() (main thread).
    void stop() {
        run_on_reactor(teardown_(/*reset_inside=*/false));
        iomanager::stop_iomgr();
        Managers::reset();
        remove_dev_files();
    }

    /// Restart: teardown all Specs (incl. Managers::reset on the reactor), bounce iomgr, re-boot with format=false.
    /// Call ad hoc from a test body (main thread) — a test may restart several times.
    void restart() {
        run_on_reactor(teardown_(/*reset_inside=*/true));
        iomanager::stop_iomgr();
        iomanager::init_iomgr(cfg_.num_reactors);
        cfg_.format = false;
        run_on_reactor(boot_< 0 >());
    }

private:
    template < size_t I >
    using NthSpec = std::tuple_element_t< I, std::tuple< Specs... > >;

    // Boot Specs in listed (ascending) order.
    template < size_t I >
    Async< void > boot_() {
        if constexpr (I < sizeof...(Specs)) {
            co_await NthSpec< I >::start(cfg_);
            co_await boot_< I + 1 >();
        }
        co_return;
    }

    // Phase 1 (prepare, reverse order) then phase 2 (stop, reverse order); optional Managers::reset at the end.
    Async< void > teardown_(bool reset_inside) {
        co_await prepare_< 0 >();
        co_await stop_< 0 >();
        if (reset_inside) {
            Managers::reset();
        }
        co_return;
    }

    // Recurse to the end, then act on the unwind → reverse order.  Only Specs with a prepare() contribute.
    template < size_t I >
    Async< void > prepare_() {
        if constexpr (I < sizeof...(Specs)) {
            co_await prepare_< I + 1 >();
            if constexpr (HasPrepare< NthSpec< I > >) {
                co_await NthSpec< I >::prepare(cfg_);
            }
        }
        co_return;
    }

    template < size_t I >
    Async< void > stop_() {
        if constexpr (I < sizeof...(Specs)) {
            co_await stop_< I + 1 >();
            co_await NthSpec< I >::stop(cfg_);
        }
        co_return;
    }

    void run_on_reactor(Async< void > task) {
        iomanager::iomgr().spawn_and_block(iomanager::ReactorTarget::any(), std::move(task));
    }

    void create_dev_files() {
        cfg_.dev_paths.clear();
        for (uint32_t i = 0; i < cfg_.num_devs; ++i) {
            auto path = fmt::format("{}_{}", cfg_.dev_prefix, i);
            cfg_.dev_paths.push_back(path);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            ofs.seekp(static_cast< std::streamoff >(cfg_.dev_size - 1));
            ofs.put('\0');
            ofs.close();
        }
    }

    void remove_dev_files() {
        for (auto const& p : cfg_.dev_paths) {
            std::filesystem::remove(p);
        }
        cfg_.dev_paths.clear();
    }

    BootCfg cfg_;
};

} // namespace homestore::test
