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
#include <atomic>
#include <memory>
#include <mutex>

#include <sisl/logging/logging.h>
#include <sisl/fds/atomic_counter.h>
#include <sisl/fds/enum.h>
#include <folly/futures/SharedPromise.h>

#include "base/homestore_assert.hpp" // HS_SUBMOD_LOG, HS_PERIODIC_DETAILED_LOG

/*
 * These are the design requirements of this class. If we don't follow these requirements then there can be serious
 * consequences in btree.
 * 1. It doesn't allow a cp to start if io is still in cp critical section. CP critical section is code between
 * cp_io_enter() and cp_io_exit().
 * 2. It doesn't allow two cps to start simultanously. second CP doesn't start until cp_done is not called in first cp.
 * 3. It call cp prepare. Purpose of this function is to create new cp and also to decide what operations we want to do
 * in that CP.
 * 4. New cp doesn't start until cp prepare is not called on a current cp.
 *
 * These are the stages of CP :-
 * CP prepare :- When cp is prepared to start flush
 * CP attach :- When new cp is created
 * Both these stages are combines in one API prepare_attach
 *
 * CP trigger :- It trigger current cp to flush
 * CP start :- It start the flush when all ios have called cp_io_exit on that cp
 * CP end :- when cp flush is completed. It frees the CP.
 */
namespace homestore {

#define CP_PERIODIC_LOG(level, cp_id, msg, ...)                                                                        \
    HS_PERIODIC_DETAILED_LOG(level, cp, "cp_id", cp_id, , , msg, ##__VA_ARGS__)
#define CP_LOG(level, cp_id, msg, ...) HS_SUBMOD_LOG(level, cp, , "cp_id", cp_id, msg, ##__VA_ARGS__)

typedef int64_t cp_id_t;
ENUM(cp_status_t, uint8_t,
     cp_unknown, // It is not inited yet.

     ////////////// IO Phase //////////////////
     cp_io_ready, // IOs can start in a CP
     cp_trigger,  // cp is triggered

     ////////////// Flush Phase ///////////////
     cp_flush_prepare, // after switchover flush is called
     cp_flushing,      // Waiting for enter cnt to be zero. User can start flush data to disk
     cp_flush_done,    // Data flush is done.

     ////////////// Cleanup Phase //////////////
     cp_cleaning);

class CPManager;

using CPConsumer = std::string_view;

struct CP {
    std::atomic< cp_status_t > cp_status_{cp_status_t::cp_unknown};
    sisl::AtomicCounter< int64_t > enter_cnt_;
    CPManager* cp_mgr_;
    cp_id_t cp_id_;
    folly::SharedPromise< bool > comp_promise_;
    bool is_on_shutdown_{false};

public:
    CP(CPManager* mgr) : cp_mgr_{mgr} {}

    cp_id_t id() const { return cp_id_; }
    cp_status_t get_status() const { return cp_status_.load(); }

    std::string to_string() const {
        return fmt::format("CP={}: status={}, enter_count={}", cp_id_, enum_name(get_status()), enter_cnt_.get());
    }
};
} // namespace homestore
