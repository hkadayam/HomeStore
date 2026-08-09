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

// Crash-matrix cases (TEST_MATRIX.md category C): a CrashSimulator flip freezes the victim's disk at a named
// product crash point (all device writes fake-succeed from that instant, the dying shutdown's final CP
// included) and reboots it in-process through ordinary recovery, which must restore exact state from the
// frozen image — non-empty replay windows, torn structures and all.

#include <gtest/gtest.h>

#include "sisl/options/options.h"
#include "repl_test_base.h"

using namespace test_common;

// A follower crashes AFTER applying a commit but BEFORE any checkpoint covers it.  The simulated crash
// freezes its disk mid-batch (all writes fake-succeed from the crash instant) and reboots it through ordinary
// recovery: the first NON-empty replay window in the suite.  The proof-gated replay must re-deliver exactly
// the applied-but-uncheckpointed suffix (unproven log tail left to nuraft re-commit), idempotently — final
// state exact on every replica.
TEST_F(ReplicaSetTest, CrashAfterCommitFollower) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    HSTestHelper::trigger_cp(); // checkpoint the first batch so the crash window is exactly the second batch
    g_helper->sync_for_test_start();

    auto const lm = wait_leader_member();
    auto const victim = (lm + 1) % replicas;
    if (g_helper->replica_num() == victim) {
        g_helper->set_flip("crash_after_data_commit", 1, 100);
    }
    g_helper->sync_for_test_start();

    write_on_leader(n); // the victim crashes applying its first commit of this batch, mid-stream
    if (g_helper->replica_num() == victim) {
        g_helper->wait_for_crash_recovery();
    }
    // The victim's count overshoots 2n by the replayed re-deliveries (by design); exact state is what the
    // validation asserts.
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// C1: a follower crashes AFTER a batch is durable in its raft log and acked to the leader, but BEFORE any of
// it is applied.  Recovery must leave the unproven durable tail to nuraft re-commit (or drop it) — zero
// double-applies, exact state everywhere.
TEST_F(ReplicaSetTest, CrashAfterLogAppendFollower) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    HSTestHelper::trigger_cp(); // checkpoint batch 1 so the crash window is exactly batch 2
    g_helper->sync_for_test_start();

    auto const lm = wait_leader_member();
    auto const victim = (lm + 1) % replicas;
    if (g_helper->replica_num() == victim) {
        g_helper->set_flip("crash_after_log_append", 1, 100);
    }
    g_helper->sync_for_test_start();

    write_on_leader(n); // the victim crashes on its first durable append of this batch
    if (g_helper->replica_num() == victim) {
        g_helper->wait_for_crash_recovery();
    }
    wait_for_commits(2 * n); // the victim converges via nuraft re-commit / catch-up

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// A follower crashes at its destroy_pending SB write: the CTRL_DESTROY committed to its log, but the local
// persist never happened.  Recovery replays the destroy (proof-gated), re-stages it, and the reaper erases
// the group — no replica may leak the group and nobody re-issues a destroy.
TEST_F(ReplicaSetTest, CrashBeforeDestroySbWrite) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    auto const gid = repl_set()->group_id();
    auto const lm = wait_leader_member();
    auto const victim = (lm + 1) % replicas;
    if (g_helper->replica_num() == victim) {
        g_helper->set_flip("crash_before_sb_write", 1, 100,
                           {{"client", flip::Operator::EQUAL, std::string{"ReplicaSet"}}});
    }
    g_helper->sync_for_test_start();

    // The victim must not run the helper's destroy poll loop — it would race its own crash-restart while
    // the managers are mid-swap.  It is not the leader (destroy is leader-issued), so it just rides the
    // crash and recovery.
    if (g_helper->replica_num() == victim) {
        g_helper->wait_for_crash_recovery();
    } else {
        g_helper->destroy_replica_set(false /* wait_for_destroy */);
    }

    for (uint32_t waited_ms = 0;; waited_ms += 200) {
        auto const rs = homestore::repl_service().get_replica_set(gid);
        if (!rs.hasValue() || !rs.value()) {
            break; // reaper finished the destroy on this replica
        }
        RELEASE_ASSERT(waited_ms < 60000u, "group not destroyed within 60s after destroy-SB crash");
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    g_helper->sync_for_verify_start();
    g_helper->sync_for_cleanup_start(); // no data validation — the group's store is gone by design
}

// A follower crashes at a raft-state save (term/vote persist) during a forced re-election.  Recovery boots
// with the pre-crash term; nuraft's vote-safety must hold — one leader per term, writes exactly-once.
TEST_F(ReplicaSetTest, CrashOnRaftStateSave) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    g_helper->sync_for_test_start();

    auto const lm = wait_leader_member();
    auto const victim = (lm + 1) % replicas;
    if (g_helper->replica_num() == victim) {
        g_helper->set_flip("crash_before_sb_write", 1, 100,
                           {{"client", flip::Operator::EQUAL, std::string{"ReplicaRaftConfig"}}});
    }
    g_helper->sync_for_test_start();

    if (g_helper->replica_num() == lm) {
        g_helper->restart(15); // > election window: survivors elect, the victim's save_state fires the crash
    }
    write_on_leader(n); // the takeover leader (or returned member) carries the batch
    if (g_helper->replica_num() == victim) {
        g_helper->wait_for_crash_recovery();
    }
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}
