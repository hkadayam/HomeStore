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
//
// Shared HomeStore test scaffolding.  The boot surface is coroutine-native: HomeStore's phased boot
// (start -> format | load -> replay) is exposed as Async<> phases so a recovery test can co_await its OWN
// recover step inline between load() and replay() — no stored callbacks, no folly::Future.  The only sync
// seams are init_env() (the reactor pool must exist before any coroutine can run) and the spawn_and_block
// bridge that carries a coroutine into a synchronous gtest body.
//
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "common/async.h" // Async<>
#include "common/defs.h"  // shared<>, r_cast
#include "iomanager/iomanager.h"
#include "homestore/homestore.h"
#include "homestore/managers.h"            // cp_mgr()
#include "homestore/checkpoint/cp_mgr.h"   // CPManager::trigger_cp_flush
#include "homestore/device/device_decl.h"  // DevInfo, HSDevType, IOFlag
#include "homestore/device/hs_super_blk.h" // HSSuperBlk (raw-device zeroing)
#include "homestore/base/homestore_assert.h"

#ifdef SISL_FLIP_ENABLED
#include "sisl/flip/flip.h"
#include "sisl/flip/flip_client.h"
#include <folly/synchronization/Baton.h>
#include "homestore/base/crash_simulator.h"
#endif

SISL_OPTION_GROUP(test_common_setup,
                  (num_threads, "", "num_threads", "number of io reactor threads",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
                  (num_devs, "", "num_devs", "number of devices to create",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
                  (dev_size_mb, "", "dev_size_mb", "size of each device in MB",
                   ::cxxopts::value< uint64_t >()->default_value("2048"), "number"),
                  (device_list, "", "device_list", "Device list instead of the default generated files",
                   ::cxxopts::value< std::vector< std::string > >(), "path [...]"),
                  (num_io, "", "num_io", "number of IO operations",
                   ::cxxopts::value< uint64_t >()->default_value("300"), "number"));

using namespace homestore;

namespace test_common {

// ── HSTestHelper: owns one HomeStore instance's lifecycle for a test. ─────────────────────────────────────────────
//
// Simple test:            helper.start_homestore("mytest", {.repl_app = app});  ... helper.shutdown_homestore();
// Recovery test:          derive and override on_recover() to rebuild durable state / re-bind listeners — it runs
//                         automatically on the recovery path of start_homestore()/restart_homestore():
//   struct MyHelper : HSTestHelper { Async<void> on_recover() override { /* rebuild */ co_return; } };
// Fully custom boot:      call init_env() once, then compose the hs_start()/hs_load()/hs_replay() phases inside
//                         your own spawn_and_block.
//
class HSTestHelper {
public:
    // Boot knobs.  Replication is enabled purely by a non-null repl_app; format_opts is consulted only on
    // first-time boot.  num_io_threads == 0 falls back to --num_threads.
    struct BootParams {
        shared< ReplApplication > repl_app{nullptr};
        FormatOpts format_opts{};
        uint32_t num_io_threads{0};
    };

    virtual ~HSTestHelper() = default;

    // ── Sync setup (reactors must exist before any coroutine runs) ────────────────────────────────────────────────
    // Resolve the device set and start the reactor pool.  Idempotent across a restart (call before each boot).
    void init_env(std::string const& test_name, BootParams params, std::vector< DevInfo > devs = {}) {
        test_name_ = test_name;
        boot_ = std::move(params);
        if (!devs.empty()) {
            devs_ = std::move(devs);
        }
        if (devs_.empty()) {
            resolve_devices();
        }

        auto const num_threads =
            boot_.num_io_threads ? boot_.num_io_threads : SISL_OPTIONS["num_threads"].as< uint32_t >();
        LOGINFO("Starting iomgr with {} reactor threads", num_threads);
        iomanager::init_iomgr(num_threads);
    }

    // ── Coroutine boot phases (compose these inside your own spawn_and_block) ─────────────────────────────────────
    Async< bool > hs_start() {
        auto* hsi = HomeStore::instance();
#ifdef SISL_FLIP_ENABLED
        // Crash simulator: on a simulated crash HomeStore invokes this restart callback, which remounts the same
        // devices (recovery path, incl. on_recover()) and then releases wait_for_crash_recovery().
        hsi->with_crash_simulator([this]() {
            LOGINFO("CrashSimulator triggered — restarting homestore");
            restart_homestore();
            crash_recovered_.post();
        });
#endif
        co_return co_await hsi->start(make_input());
    }

    Async< void > hs_format() {
        co_await HomeStore::instance()->format();
    }
    Async< void > hs_load() {
        co_await HomeStore::instance()->load();
    }
    Async< void > hs_replay() {
        co_await HomeStore::instance()->replay();
    }

    // Overridable recovery hook: runs on the recovery path only, between load() and replay() — the slot where a
    // derived helper (e.g. HSReplTestHelper) or a test rebuilds its own durable state and re-binds listeners.
    // Coroutine + virtual (not a stored callback); no-op by default.
    virtual Async< void > on_recover() {
        co_return;
    }

    // Full boot: start -> format (first boot) | load -> on_recover() -> replay (recovery).
    virtual Async< void > hs_boot() {
        if (co_await hs_start()) {
            co_await hs_format(); // fresh store goes live directly — no replay phase
        } else {
            co_await hs_load();
            co_await on_recover(); // derived classes rebuild their durable state here
            co_await hs_replay();
        }
    }

    Async< void > hs_shutdown() {
        co_await HomeStore::instance()->shutdown();
    }

    // Force a CP flush.  Coroutine — co_await it from wherever you already are.
    static Async< void > hs_trigger_cp() {
        auto const ok = co_await cp_mgr().trigger_cp_flush(true /* force */);
        HS_REL_ASSERT_EQ(ok, true, "CP flush failed");
    }

    // ── Sync bridges for the gtest edge (SetUp/TearDown are synchronous) ──────────────────────────────────────────
    // Convenience for the common no-recovery case.  Recovery tests drive the phases themselves (see class doc).
    virtual void start_homestore(std::string const& test_name, BootParams params, std::vector< DevInfo > devs = {}) {
        init_env(test_name, std::move(params), std::move(devs));
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [this]() -> Async< void > { co_await hs_boot(); }());
    }

    virtual void shutdown_homestore(bool cleanup = true) {
        if (HomeStore::safe_instance() == nullptr) {
            return;
        } // already down
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(),
                                [this]() -> Async< void > { co_await hs_shutdown(); }());
        iomanager::stop_iomgr(); // stop reactors only after HomeStore has released its references
        HomeStore::reset_instance();
        if (cleanup) {
            remove_files(generated_devs_);
            generated_devs_.clear();
        }
    }

    // Remount the same devices: bounce iomgr, then re-boot via hs_boot() — which runs the virtual on_recover()
    // on the recovery path, so a derived helper's recovery is honored here without any callback plumbing.
    virtual void restart_homestore(uint32_t shutdown_delay_sec = 3) {
        shutdown_homestore(false /* cleanup */);
        std::this_thread::sleep_for(std::chrono::seconds{shutdown_delay_sec});
        init_env(test_name_, boot_, devs_);
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [this]() -> Async< void > { co_await hs_boot(); }());
    }

    static void trigger_cp() {
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), []() -> Async< void > { co_await hs_trigger_cp(); }());
    }

    static void fill_data_buf(uint8_t* buf, uint64_t size, uint64_t pattern = 0) {
        auto* ptr = r_cast< uint64_t* >(buf);
        for (uint64_t i = 0ul; i < size / sizeof(uint64_t); ++i) {
            ptr[i] = (pattern == 0) ? i : pattern;
        }
    }

    static void validate_data_buf(uint8_t const* buf, uint64_t size, uint64_t pattern = 0) {
        auto const* ptr = r_cast< uint64_t const* >(buf);
        for (uint64_t i = 0ul; i < size / sizeof(uint64_t); ++i) {
            HS_REL_ASSERT_EQ(ptr[i], ((pattern == 0) ? i : pattern), "data_buf mismatch at offset={}", i);
        }
    }

    std::vector< DevInfo > const& devices() const {
        return devs_;
    }

#ifdef SISL_FLIP_ENABLED
    // ── Crash simulation ─────────────────────────────────────────────────────────────────────────────────────────
    // Block until the crash-simulator's restart-and-recover cycle (wired in hs_start) has completed.
    void wait_for_crash_recovery() {
        crash_recovered_.wait();
        crash_recovered_.reset();
    }
#endif

#ifdef SISL_FLIP_ENABLED
    // ── Fault injection (flip) ───────────────────────────────────────────────────────────────────────────────────
    // Compact flip-condition spec: `{{"param_name", flip::Operator::EQUAL, value}}` at the call site, one
    // brace-triple per fire-site parameter (matched positionally).  `param_name` is documentation only —
    // flip matches by position — but keeps call sites self-describing.
    struct FlipCond {
        std::string name;
        flip::Operator oper;
        std::variant< int, long, double, bool, std::string > value;
    };

    // Fire `flip_name` `count` times, `percent`% of the eligible calls, on fire sites whose parameters match
    // `conds` (empty = unconditional).
    void set_flip(std::string const& flip_name, uint32_t count = 1, uint32_t percent = 100,
                  std::vector< FlipCond > const& conds = {}) {
        flip::FlipClient::instance().inject_noreturn_flip(flip_name, make_conditions(conds), make_freq(count, percent));
        LOGDEBUG("Flip {} set (count={} percent={})", flip_name, count, percent);
    }

    // Fire `flip_name` but delay the caller by `delay_usec` instead of failing it.
    void set_delay_flip(std::string const& flip_name, uint64_t delay_usec, uint32_t count = 1, uint32_t percent = 100,
                        std::vector< FlipCond > const& conds = {}) {
        flip::FlipClient::instance().inject_delay_flip(flip_name, make_conditions(conds), make_freq(count, percent),
                                                       delay_usec);
        LOGDEBUG("Flip {} set (delay {}us, {} condition(s))", flip_name, delay_usec, conds.size());
    }

    // Make the `set_minimum_chunk_size` flip return `chunk_size` so managers size their first chunk small.
    void set_min_chunk_size(uint64_t chunk_size) {
        flip::FlipClient::instance().inject_retval_flip< long >("set_minimum_chunk_size", {}, make_freq(2000000, 100),
                                                                chunk_size);
        LOGINFO("Set minimum chunk size {}", chunk_size);
    }

    void remove_flip(std::string const& flip_name) {
        flip::FlipClient::instance().remove_flip(flip_name);
        LOGDEBUG("Flip {} removed", flip_name);
    }
#endif

protected:
    InputParams make_input() const {
        InputParams input;
        input.devices = devs_;
        input.data_open_flags = IOFlag::BUFFERED_IO; // file-backed test devices don't support O_DIRECT
        input.fast_open_flags = IOFlag::BUFFERED_IO;
        input.repl_app = boot_.repl_app;
        input.format_opts = boot_.format_opts;
        return input;
    }

#ifdef SISL_FLIP_ENABLED
    // Flip frequency (flatbuffer object-API): fire up to `count` times, `percent`% of eligible calls.
    static flip::FlipFrequencyT make_freq(uint32_t count, uint32_t percent) {
        flip::FlipFrequencyT freq;
        freq.count = count;
        flip::PercentFrequencyT pf;
        pf.v = percent;
        freq.kind.Set(pf);
        return freq;
    }

    static std::vector< flip::FlipConditionT > make_conditions(std::vector< FlipCond > const& conds) {
        std::vector< flip::FlipConditionT > out;
        out.reserve(conds.size());
        for (auto const& c : conds) {
            std::visit(
                [&](auto const& v) { out.push_back(flip::FlipClient::instance().create_condition(c.name, c.oper, v)); },
                c.value);
        }
        return out;
    }
#endif

    // Resolve the device set: (1) --device_list (raw devices, zeroed), else (2) generated /tmp/<name>_<n> files
    // sized by --dev_size_mb.  First device is Fast, the rest Data.
    void resolve_devices() {
        if (SISL_OPTIONS.count("device_list")) {
            auto const devs = SISL_OPTIONS["device_list"].as< std::vector< std::string > >();
            for (uint32_t i{0}; i < devs.size(); ++i) {
                devs_.emplace_back(devs[i], (i == 0) ? HSDevType::Fast : HSDevType::Data);
                init_raw_device(devs_.back());
            }
            return;
        }

        auto const n = SISL_OPTIONS["num_devs"].as< uint32_t >();
        auto const dev_size = SISL_OPTIONS["dev_size_mb"].as< uint64_t >() * 1024ul * 1024ul;
        for (uint32_t i{0}; i < n; ++i) {
            auto const fname = std::string{"/tmp/" + test_name_ + "_" + std::to_string(i + 1)};
            generated_devs_.push_back(fname);
            init_file(fname, dev_size);
            devs_.emplace_back(std::filesystem::canonical(fname).string(), (i == 0) ? HSDevType::Fast : HSDevType::Data,
                               dev_size);
        }
    }

    void remove_files(std::vector< std::string > const& file_paths) {
        for (auto const& fpath : file_paths) {
            if (std::filesystem::exists(fpath)) {
                std::filesystem::remove(fpath);
            }
        }
    }

    void init_file(std::string const& fpath, uint64_t dev_size) {
        if (std::filesystem::exists(fpath)) {
            std::filesystem::remove(fpath);
        }
        std::ofstream ofs{fpath, std::ios::binary | std::ios::out | std::ios::trunc};
        std::filesystem::resize_file(fpath, dev_size);
    }

    void init_raw_device(DevInfo const& dinfo) {
        auto const zero_size = HSSuperBlk::first_block_size() * 1024;
        std::vector< uint8_t > zeros(zero_size, 0);

        HS_REL_ASSERT(std::filesystem::exists(dinfo.dev_name), "Device {} does not exist", dinfo.dev_name);
        auto fd = ::open(dinfo.dev_name.c_str(), O_RDWR, 0640);
        HS_REL_ASSERT(fd != -1, "Failed to open device {}", dinfo.dev_name);
        auto const write_sz = ::pwrite(fd, zeros.data(), zero_size, HSSuperBlk::first_block_offset());
        HS_REL_ASSERT(write_sz == (ssize_t)zero_size, "Failed to zero device {}", dinfo.dev_name);
        ::close(fd);
        LOGINFO("Zeroed the first {} bytes of raw device {}", zero_size, dinfo.dev_name);
    }

protected:
    std::string test_name_;
    BootParams boot_;
    std::vector< DevInfo > devs_;
    std::vector< std::string > generated_devs_;
#ifdef SISL_FLIP_ENABLED
    folly::Baton<> crash_recovered_; // posted by the crash restart_cb, awaited by wait_for_crash_recovery()
#endif
};

} // namespace test_common
