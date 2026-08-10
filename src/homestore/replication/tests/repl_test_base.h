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
// ReplicaSetTestBase — the gtest base fixture for replication UNIT tests.  It stands up a durable COWBtree-backed
// store, hands it to the shared HSReplTestHelper (which owns the listener + cluster), and offers the common
// operations (write_on_leader / wait_for_commits / validate_data).  Being testing::Test-derived, this is
// unit-test-only; an integ driver reuses HSReplTestHelper directly instead.  Different repl unit-test files derive
// from this base.
//
// NOTE: TUs that include this compile in btree async mode (BTREE_ASYNC_MODE) to match the hs_cow_btree library.
//
#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <boost/uuid/uuid_io.hpp>
#include <folly/coro/Collect.h> // collectAllWindowed

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "homestore/test_common/hs_repl_test_common.h"
#include "homestore/test_common/hs_test_cow_store.h"

namespace test_common {

// The one multi-process helper for the run; defined in the test binary's main().
extern std::unique_ptr< HSReplTestHelper > g_helper;

// Deterministic value for a key, so every replica must converge to the exact same (key -> value).
inline uint32_t value_for(uint64_t key) {
    return static_cast< uint32_t >(key * 2654435761ull + 1);
}

class ReplicaSetTestBase : public testing::Test {
protected:
    shared< TestStore > store_;
    uint64_t written_{0};    // cumulative # of keys proposed so far — keys are [0, written_); the next write starts here
    size_t write_qdepth_{8}; // max concurrent in-flight proposals per write_on_leader batch (a test may retune this)

    void SetUp() override {
        // HomeStore is already booted by g_helper->setup() (called in main), so the durable COWBtree-backed store
        // can attach its btree now. recover() creates a fresh btree on first boot / loads it on recovery.
        store_ = make_cow_btree_store(g_helper->replica_num());
        iomgr().spawn_and_block(iomanager::ReactorTarget::any(), store_->recover());
        g_helper->register_replica_set(store_); // hand the store in; the helper creates + registers the listener
    }

    // Destroy this test's replica set + btree so the next test in the binary starts clean on the shared HomeStore
    // (tests all boot one HomeStore in main()).  Wait for the group (raft engine) teardown before dropping the btree
    // so a late on_commit can't fire into a torn-down store.
    void TearDown() override {
        g_helper->destroy_replica_set(/*wait_for_destroy=*/true);
        if (store_) {
            iomgr().spawn_and_block(iomanager::ReactorTarget::any(), store_->destroy());
            store_.reset();
        }
        written_ = 0;
    }

    shared< homestore::ReplicaSet > repl_set() { return g_helper->repl_set(); }

    // Propose the next n entries: keys [written_, written_ + n).  written_ advances on EVERY replica (so followers
    // track the same expected total), but exactly one member actually proposes — whoever holds leadership when the
    // batch becomes writable.  Leader identity is never a safe exit for the rest: a claim can be stale (the
    // claimed leader is down) or a downed leader can legitimately return and re-win, and no observer can tell the
    // two apart.  A non-leader therefore leaves only on proof the batch landed: commit count >= written_ + n.
    // Commit counts are cumulative across in-process restarts (the helper re-binds the same listener on
    // recovery), so the target is absolute on every replica, restarted or not.
    // `wait_for_results=false` proposes and returns immediately, asserting nothing: for tests whose proposals are
    // EXPECTED to die (no quorum, or a crash swallows them).  Such a caller owns reconciling written_ with what
    // actually landed (typically `written_ -= n` plus a normal rewrite once the cluster settles), and a non-leader
    // returns at once rather than waiting for commits that will never come.
    void write_on_leader(uint64_t n, bool wait_for_results = true) {
        uint64_t const start = written_;
        written_ += n;

        auto rs = repl_set();
        if (!rs) { // group not yet bound on this replica
            return;
        }

        while (true) {
            if (rs->get_leader_id() == g_helper->my_replica_id()) {
                break; // I am the leader — the batch is mine
            }
            if (!wait_for_results) {
                return; // the batch may never land; nobody waits on it
            }
            if (commit_count() >= start + n) {
                LOGINFO("Replica={} saw the batch of {} land under another leader (count={})",
                        g_helper->replica_num(), n, commit_count());
                return;
            }
            LOGINFO("Replica={} not the leader — waiting for leadership or batch commits ({}/{})",
                    g_helper->replica_num(), commit_count(), start + n);
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }

        // A freshly elected leader must commit any carried-over entries from prior terms before it can accept new
        // proposals — gate on is_ready_for_traffic() exactly as the old harness did.  A detached caller skips the
        // gate: it proposes precisely because the group is NOT healthy (no quorum), so readiness never comes.
        while (wait_for_results && !rs->is_ready_for_traffic()) {
            LOGINFO("Replica={} leader not yet ready for traffic, waiting", g_helper->replica_num());
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }

        LOGINFO("Replica={} is the leader — proposing {} entries [{}, {}) at qdepth {}{}", g_helper->replica_num(), n,
                start, start + n, write_qdepth_, wait_for_results ? "" : " (detached)");
        if (!wait_for_results) {
            for (uint64_t k = 0; k < n; ++k) {
                iomgr().spawn_detached(iomanager::ReactorTarget::any(), [rs, key = start + k]() -> Async< void > {
                    uint32_t const val = value_for(key);
                    sisl::IoBufSpan header{to_cu8ptr(&key), to_u32(sizeof(key)), false};
                    sisl::IoBufView value{to_u32(sizeof(val))};
                    std::memcpy(value.bytes(), &val, sizeof(val));
                    co_await rs->write(header, value);
                });
            }
            return;
        }

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
    // Member index currently holding leadership per this replica's raft view, or nullopt while unelected.
    std::optional< uint16_t > leader_member_idx() {
        auto rs = repl_set();
        if (!rs) {
            return std::nullopt;
        }
        auto const leader = rs->get_leader_id();
        if (leader.is_nil()) {
            return std::nullopt;
        }
        auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
        for (uint16_t i = 0; i < replicas; ++i) {
            if (g_helper->replica_id(i) == leader) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Block until this replica's raft view shows an elected leader that satisfies the caller's constraints, and
    // return its member index.  `allowed` (when non-empty) is the allow-list of members that count as settled;
    // `denied` is the deny-list — e.g. a just-deposed leader, whose own view still names itself for a while, so
    // proposing there would be routed to a node about to step down.  Both empty: any elected leader will do.
    uint16_t wait_till_leader_elected(std::vector< uint16_t > const& allowed = {},
                                      std::vector< uint16_t > const& denied = {}) {
        auto const listed = [](std::vector< uint16_t > const& v, uint16_t m) {
            return std::find(v.begin(), v.end(), m) != v.end();
        };
        for (uint32_t waited_ms = 0;; waited_ms += 500) {
            if (auto const lm = leader_member_idx();
                lm && (allowed.empty() || listed(allowed, *lm)) && !listed(denied, *lm)) {
                return *lm;
            }
            RELEASE_ASSERT(waited_ms < 120000u, "no leader matching the allow/deny lists within 120s");
            LOGINFO("Replica={} waiting for an acceptable leader to be elected", g_helper->replica_num());
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
    }

    // Committed-and-applied count on this replica.  On a follower the listener is created only when the
    // leader's join_cluster_request arrives, which trails the creator's self-election — a null listener here
    // is a normal transient, counted as 0.
    static uint64_t commit_count() {
        auto l = g_helper->listener();
        return l ? l->commit_count() : 0ul;
    }

    void wait_for_commits(uint64_t total) {
        while (commit_count() < total) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            LOGINFO("Replica={} received {} commits, expected {}", g_helper->replica_num(), commit_count(), total);
        }
        LOGINFO("Replica={} received {} commits as expected", g_helper->replica_num(), total);
    }

    // Baseline-relative wait: `n` NEW commits on top of `baseline` (a commit_count() snapshot taken before
    // the writes).  Loop-shaped tests use this so the expected count never depends on what a restart's
    // replay did or didn't re-deliver.
    void wait_for_commits_from(uint64_t baseline, uint64_t n) {
        while (commit_count() < baseline + n) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            LOGINFO("Replica={} received {} of {} commits (baseline {})", g_helper->replica_num(), commit_count(),
                    baseline + n, baseline);
        }
        LOGINFO("Replica={} received {} new commits as expected", g_helper->replica_num(), n);
    }

    // Force a checkpoint on this replica's store so its committed state is flushed durably to disk — a subsequent
    // restart then recovers that state from the btree, with only the post-CP tail coming from raft log replay.
    void trigger_cp() { iomgr().spawn_and_block(iomanager::ReactorTarget::any(), store_->checkpoint()); }

    // Validate keys [0, total): every replica must hold the identical (key -> value_for(key)) mapping.  `total` is
    // the CUMULATIVE count across all writes so far.  Applies can still be streaming in when this is entered
    // (replay re-delivery, raft re-commits after a restart), so the check polls to convergence: retry until clean
    // or the deadline expires.  Real divergence still fails at the deadline — with the exact keys logged.
    void validate_data(uint64_t total) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        uint64_t mismatches = 0;
        std::vector< std::pair< uint64_t, std::optional< uint32_t > > > bad;
        while (true) {
            mismatches = 0;
            bad.clear();
            iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [this, total, &mismatches,
                                                                      &bad]() -> Async< void > {
                for (uint64_t k = 0; k < total; ++k) {
                    auto const v = co_await store_->lookup(k);
                    if (!v.has_value() || *v != value_for(k)) {
                        ++mismatches;
                        if (bad.size() < 8) {
                            bad.emplace_back(k, v);
                        }
                    }
                }
                co_return;
            }());
            if ((mismatches == 0) || (std::chrono::steady_clock::now() > deadline)) {
                break;
            }
            LOGINFO("Replica={} validate: {} of {} keys not converged yet, retrying", g_helper->replica_num(),
                    mismatches, total);
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
        for (auto const& [k, v] : bad) {
            LOGERROR("Replica={} diverged key={} expected={} got={}", g_helper->replica_num(), k, value_for(k),
                     v.has_value() ? std::to_string(*v) : "missing");
        }
        ASSERT_EQ(mismatches, 0u) << "replica " << g_helper->replica_num() << " diverged on " << mismatches << " of "
                                  << total << " keys";
    }

    // Convenience: wait for / validate ALL entries written so far (across every write_on_leader call).  Prefer
    // these in tests that write in multiple batches so the cumulative total is never miscounted.
    void wait_for_all_commits() { wait_for_commits(written_); }
    void validate_all_data() { validate_data(written_); }
};

// Shared gtest fixture for every replication test category file. All categories compile into one binary (btree
// compile cost is paid once, in hs_test_cow_store.cpp); each category lives in its own .cpp for readability.
class ReplicaSetTest : public ReplicaSetTestBase {};

} // namespace test_common
