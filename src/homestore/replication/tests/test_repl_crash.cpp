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

    auto const lm = wait_till_leader_elected();
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

    auto const lm = wait_till_leader_elected();
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
    auto const lm = wait_till_leader_elected();
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

    auto const lm = wait_till_leader_elected();
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

// Scenario: the leader dies suddenly mid-work.
//   1. Three replicas, healthy; a batch is written and checkpointed.
//   2. A new entry is proposed and the LEADER dies without warning in the middle of committing it.
//   3. The two survivors notice, elect a new leader, and the interrupted entry finishes under it (it had
//      already reached both survivors' logs — quorum — before the leader's local commit crashed).
//   4. The dead node reboots from its disk exactly as it was at the instant of death and rejoins as follower.
//   5. Writes resume under the new leader; all three replicas end with exactly the same data — nothing lost,
//      nothing applied twice.
TEST_F(ReplicaSetTest, LeaderCrashOutright) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    HSTestHelper::trigger_cp();
    g_helper->sync_for_test_start();

    auto const victim = wait_till_leader_elected();
    if (g_helper->replica_num() == victim) {
        g_helper->set_flip("crash_after_data_commit", 1, 100);
    }
    g_helper->sync_for_test_start();

    write_on_leader(1, false /* wait_for_results */); // the leader crashes committing this entry; both followers already hold it
    if (g_helper->replica_num() == victim) {
        g_helper->wait_for_crash_recovery();
    }
    wait_for_commits(n + 1); // survivors elect and finish the entry; the victim converges after recovery

    write_on_leader(n); // writes resume under whichever leader emerged
    wait_for_commits(2 * n + 1);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: the leader dies holding entries nobody else has.
//   1. Three replicas, healthy; a batch is written and checkpointed.
//   2. The leader appends a new entry into its OWN log and dies before any follower received it — the entry
//      is durable on its disk but unknown to the cluster.
//   3. The survivors elect a new leader and move on.
//   4. The old leader reboots carrying that orphaned log tail; consensus decides its fate (replicated if its
//      term still wins, discarded otherwise) — under no circumstance is any entry applied twice.
//   5. The same key is rewritten under the settled leader so the expected key range lands either way; all
//      replicas converge to exactly the same data.
TEST_F(ReplicaSetTest, LeaderCrashUncommittedTail) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    HSTestHelper::trigger_cp();
    g_helper->sync_for_test_start();

    auto const victim = wait_till_leader_elected();
    if (g_helper->replica_num() == victim) {
        g_helper->set_flip("crash_after_log_append", 1, 100);
    }
    g_helper->sync_for_test_start();

    write_on_leader(1, false /* wait_for_results */); // durable in the leader's log only, then the disk freezes
    if (g_helper->replica_num() == victim) {
        g_helper->wait_for_crash_recovery();
    }
    g_helper->sync_for_verify_start(); // everyone (victim recovered) before the rewrite

    written_ -= 1;      // the orphaned entry never counted as landed
    write_on_leader(1); // rewrite the same key under the settled leader (idempotent if the orphan won)
    wait_for_commits(n + 1);

    write_on_leader(n);
    wait_for_commits(2 * n + 1);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: a live leader must throw away entries it accepted alone.
//   1. Three replicas, healthy; baseline data committed and checkpointed.
//   2. Both followers go down together.  The lone leader — which stays ALIVE throughout — accepts an entry
//      into its log; without quorum it can never commit.
//   3. The followers come back, but the old leader's replication to them gets eaten by the network; their
//      election timers fire and they elect a new leader between themselves at a higher term — neither has
//      the lone entry.
//   4. The network heals.  The old leader hears the higher term, steps down while running, detects the
//      divergence, and ROLLS the entry back — the live data-rollback path (a rebooted node truncates the
//      tail silently instead; staying alive is what makes the rollback listener fire).
//   5. The key is rewritten under the settled leader; all replicas converge with the divergent proposal gone.
TEST_F(ReplicaSetTest, RollbackDivergentEntries) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    constexpr int kAppendEntriesMsgType = 3; // nuraft::msg_type::append_entries_request
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    HSTestHelper::trigger_cp();
    g_helper->sync_for_test_start();

    auto const victim = wait_till_leader_elected();
    if (g_helper->replica_num() != victim) {
        // Armed before the reboot (flip state survives an in-process restart): on return, eat the old
        // leader's append probes so the followers' election fires before it can re-adopt them.  The budget
        // is finite, so the cluster self-heals once the new term is established.
        g_helper->set_flip("simulate_drop_repl_rpc", 8, 100,
                           {{"msg_type", flip::Operator::EQUAL, kAppendEntriesMsgType}});
    }
    g_helper->sync_for_test_start();

    if (g_helper->replica_num() != victim) {
        g_helper->restart(10); // both followers down together
        written_ += 1;         // mirror the victim's nowait bookkeeping below (they never saw the call)
    } else {
        std::this_thread::sleep_for(std::chrono::seconds{3}); // let the followers actually go down
        write_on_leader(1, false /* wait_for_results */); // pre-committed live on the lone leader — no quorum, never commits
    }
    g_helper->sync_for_verify_start(); // followers elected a new term; old leader stepped down + rolled back
    wait_till_leader_elected({}, {victim});     // the deposition is what makes the rollback happen — gate on it

    written_ -= 1;      // the divergent entry is gone by design
    write_on_leader(1); // rewrite that key under the cluster's real history
    wait_for_commits(n + 1);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: the node dies in the middle of throwing the divergent entry away.
//   Same as RollbackDivergentEntries through step 4 — but the moment the stepped-down leader starts rolling
//   the divergent entry back, it crashes.  Its recovery must complete the divergence repair correctly: no
//   half-rolled-back state, no resurrected entry, and the cluster still converges to identical data.
TEST_F(ReplicaSetTest, RollbackDivergentCrash) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    constexpr int kAppendEntriesMsgType = 3; // nuraft::msg_type::append_entries_request
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    HSTestHelper::trigger_cp();
    g_helper->sync_for_test_start();

    auto const victim = wait_till_leader_elected();
    if (g_helper->replica_num() != victim) {
        g_helper->set_flip("simulate_drop_repl_rpc", 8, 100,
                           {{"msg_type", flip::Operator::EQUAL, kAppendEntriesMsgType}});
    } else {
        g_helper->set_flip("crash_after_data_rollback", 1, 100); // fires during the live rollback
    }
    g_helper->sync_for_test_start();

    if (g_helper->replica_num() != victim) {
        g_helper->restart(10);
        written_ += 1;
    } else {
        std::this_thread::sleep_for(std::chrono::seconds{3});
        write_on_leader(1, false /* wait_for_results */);          // pre-committed live; the rollback of this entry crashes us
        g_helper->wait_for_crash_recovery(); // crash mid-rollback, then recover through it
    }
    g_helper->sync_for_verify_start();
    wait_till_leader_elected({}, {victim});

    written_ -= 1;
    write_on_leader(1);
    wait_for_commits(n + 1);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}
