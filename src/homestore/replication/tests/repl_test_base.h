/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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
// ReplicaSetTestBase — the gtest base fixture for replication UNIT tests.  It stands up a MemBtree-backed store,
// hands it to the shared HSReplTestHelper (which owns the listener + cluster), and offers the common operations
// (write_on_leader / wait_for_commits / validate_data).  Being testing::Test-derived, this is unit-test-only; an
// integ driver reuses HSReplTestHelper directly instead.  Different repl unit-test files derive from this base.
//
// NOTE: TUs that include this (via MemBtreeStore) compile in sync btree mode to match the hs_mem_btree library.
//
#pragma once

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include <boost/uuid/uuid_io.hpp>
#include <folly/coro/Collect.h> // collectAllWindowed

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "homestore/test_common/hs_repl_test_common.h"
#include "homestore/test_common/hs_test_mem_store.h"

namespace test_common {

// The one multi-process helper for the run; defined in the test binary's main().
extern std::unique_ptr< HSReplTestHelper > g_helper;

// Deterministic value for a key, so every replica must converge to the exact same (key -> value).
inline uint32_t value_for(uint64_t key) {
    return static_cast< uint32_t >(key * 2654435761ull + 1);
}

class ReplicaSetTestBase : public testing::Test {
protected:
    shared< MemBtreeStore > store_;
    uint64_t written_{0};    // cumulative # of keys proposed so far — keys are [0, written_); the next write starts here
    size_t write_qdepth_{8}; // max concurrent in-flight proposals per write_on_leader batch (a test may retune this)

    void SetUp() override {
        store_ = std::make_shared< MemBtreeStore >();
        g_helper->register_replica_set(store_); // hand the store in; the helper creates + registers the listener
    }

    shared< homestore::ReplicaSet > repl_set() { return g_helper->repl_set(); }

    // Propose the next n entries: keys [written_, written_ + n).  written_ advances on EVERY replica (so followers
    // track the same expected total), but only the elected leader actually proposes — followers receive the entries
    // by replication.  Waits for leader election and is_ready_for_traffic() before writing.
    void write_on_leader(uint64_t n) {
        uint64_t const start = written_;
        written_ += n;

        auto rs = repl_set();
        if (!rs) { // group not yet bound on this replica
            return;
        }

        while (true) {
            auto const leader = rs->get_leader_id();
            if (leader.is_nil()) {
                LOGINFO("Replica={} waiting for leader election", g_helper->replica_num());
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                continue;
            }
            if (leader != g_helper->my_replica_id()) {
                LOGINFO("Replica={} is not the leader ({}); {} entries are written on the leader",
                        g_helper->replica_num(), boost::uuids::to_string(leader), n);
                return;
            }
            break; // I am the leader
        }

        // A freshly elected leader must commit any carried-over entries from prior terms before it can accept new
        // proposals — gate on is_ready_for_traffic() exactly as the old harness did.
        while (!rs->is_ready_for_traffic()) {
            LOGINFO("Replica={} leader not yet ready for traffic, waiting", g_helper->replica_num());
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }

        LOGINFO("Replica={} is the leader — proposing {} entries [{}, {}) at qdepth {}", g_helper->replica_num(), n,
                start, start + n, write_qdepth_);
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [rs, start, n, qd = write_qdepth_]() -> Async< void > {
            // One coroutine per entry, each owning its own key + buffers (key is a by-value coroutine param, so it
            // lives in the frame the header points into).  Issue them with up to `qd` concurrent in-flight proposals
            // — a real client at queue depth — rather than one-at-a-time-and-block.
            auto propose = [](shared< homestore::ReplicaSet > rs, uint64_t key) -> Async< homestore::ReplResult<> > {
                uint32_t const val = value_for(key);
                sisl::IoBufSpan header{to_cu8ptr(&key), to_u32(sizeof(key)), false};
                sisl::IoBufView value{to_u32(sizeof(val))};
                std::memcpy(value.bytes(), &val, sizeof(val));
                co_return co_await rs->write(header, value);
            };

            std::vector< Async< homestore::ReplResult<> > > tasks;
            tasks.reserve(n);
            for (uint64_t k = 0; k < n; ++k) {
                tasks.push_back(propose(rs, start + k));
            }
            auto const results = co_await folly::coro::collectAllWindowed(std::move(tasks), qd);
            for (auto const& r : results) {
                RELEASE_ASSERT(r.hasValue(), "a replicated write failed");
            }
            co_return;
        }());
    }

    // Poll this replica's committed-and-applied count (the listener's commit_count(), bumped in on_commit after the
    // store apply) until it reaches `total` — this is the CUMULATIVE expected count across all write_on_leader
    // calls, not a per-call count.  Same quantity the old harness's wait_for_commits() polled.
    void wait_for_commits(uint64_t total) {
        while (g_helper->listener()->commit_count() < total) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            LOGINFO("Replica={} received {} commits, expected {}", g_helper->replica_num(),
                    g_helper->listener()->commit_count(), total);
        }
        LOGINFO("Replica={} received {} commits as expected", g_helper->replica_num(), total);
    }

    // Validate keys [0, total): every replica must hold the identical (key -> value_for(key)) mapping.  `total` is
    // the CUMULATIVE count across all writes so far.
    void validate_data(uint64_t total) {
        uint64_t mismatches = 0;
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [this, total, &mismatches]() -> Async< void > {
            for (uint64_t k = 0; k < total; ++k) {
                auto const v = co_await store_->lookup(k);
                if (!v.has_value() || *v != value_for(k)) {
                    ++mismatches;
                }
            }
            co_return;
        }());
        ASSERT_EQ(mismatches, 0u) << "replica " << g_helper->replica_num() << " diverged on " << mismatches << " of "
                                  << total << " keys";
    }

    // Convenience: wait for / validate ALL entries written so far (across every write_on_leader call).  Prefer
    // these in tests that write in multiple batches so the cumulative total is never miscounted.
    void wait_for_all_commits() { wait_for_commits(written_); }
    void validate_all_data() { validate_data(written_); }
};

} // namespace test_common
