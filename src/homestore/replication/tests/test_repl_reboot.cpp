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
// Category B — clean restart / recovery: no crash, just orderly shutdown + reboot of the whole cluster.  A clean
// shutdown checkpoints every consumer (btree + log store watermark) as its final CP, so recovery loads the full
// state from the btree with an empty replay window, and the reformed cluster must re-elect and accept writes whose
// LSNs continue past the recovered tail.  One looped scenario covers it: each round writes fresh keys, quiesces the
// cluster on a barrier (no replica restarts while peers still stream), restarts every replica, and rendezvous
// before the next round writes on the recovered tail.  Final validation checks every key from every round on every
// replica — nothing lost, nothing duplicated, across R reboots.  Crash-in-the-middle variants (where replay is
// genuinely non-empty) live in the crash category.
//
#include <gtest/gtest.h>

#include "sisl/options/options.h"
#include "sisl/flip/flip.h"
#include "sisl/flip/flip_client.h"
#include "homestore/base/hs_runtime_config.h" // HS_SETTINGS_FACTORY
#include "repl_test_base.h"

using namespace test_common;

TEST_F(ReplicaSetTest, WriteRestartAllMulti) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    constexpr uint32_t rounds = 6;
    g_helper->sync_for_test_start();

    for (uint32_t r = 0; r < rounds; ++r) {
        LOGINFO("Replica={} restart-loop round {}/{}: writing {} keys", g_helper->replica_num(), r + 1, rounds, n);
        auto const baseline = commit_count();
        write_on_leader(n);
        wait_for_commits_from(baseline, n);

        g_helper->sync_for_test_start(); // everyone holds this round's commits before anyone restarts
        g_helper->restart();
        g_helper->sync_for_test_start(); // everyone is back up before the next round writes
    }

    g_helper->sync_for_verify_start();
    validate_all_data(); // all rounds * n keys, identical on every replica
    g_helper->sync_for_cleanup_start();
}

// Followers restart one at a time while the leader keeps accepting writes.  Each downed follower misses a
// full batch and can only reach the cumulative commit count by catching up from the raft log on return.
// Every replica calls write_on_leader once per round — the restarted member after it returns (bookkeeping
// only, since it is not the leader), the rest while it is down.
TEST_F(ReplicaSetTest, FollowerRestarts) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    uint64_t expected = 0;
    for (uint32_t m = 1; m < replicas; ++m) { // member 0 created the group and leadership is pinned on it
        g_helper->sync_for_test_start();
        if (g_helper->replica_num() == m) {
            LOGINFO("Replica={} follower going down", g_helper->replica_num());
            g_helper->restart();
            write_on_leader(n);
        } else {
            write_on_leader(n); // the leader proposes this batch while m is down
        }
        expected += n;
        wait_for_commits(expected); // the restarted follower reaches this only by catch-up
        g_helper->sync_for_test_start();
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// The LEADER restarts and returns before any election can fire: the election window is raised far above the
// restart blip, so followers hold their fire, nothing commits during the blip, and the returned leader itself
// proposes the round's batch.  The blip must look like a pause, never a leadership change.
TEST_F(ReplicaSetTest, LeaderQuickRestart) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    // Widen the election window in place on every member — engine raft params are per-node, so each replica
    // updates its own.  Survivors then cannot start an election while the leader blips: pre-vote liveness
    // makes them deny any candidacy until a full window passes with no heartbeat.  The same gate holds the
    // returned leader out too, so post-blip leadership goes to whichever timer wins once liveness lapses —
    // the returned member (retrying on its narrow persisted window) or a survivor.  Either is legal here;
    // the round's batch belongs to whoever holds leadership, never to the blipping member's stale claim.
    constexpr int32_t wide_elect_low_ms = 30000;
    constexpr int32_t wide_elect_high_ms = 35000;
    iomgr().spawn_and_block(iomanager::ReactorTarget::any(), []() -> Async< void > {
        co_await g_helper->repl_set()->update_raft_params([](nuraft::raft_params& p) {
            p.election_timeout_lower_bound_ = wide_elect_low_ms;
            p.election_timeout_upper_bound_ = wide_elect_high_ms;
        });
    }());

    constexpr uint32_t rounds = 2;
    uint64_t expected = 0;
    for (uint32_t r = 0; r < rounds; ++r) {
        g_helper->sync_for_test_start();
        auto const lm = wait_till_leader_elected();
        if (g_helper->replica_num() == lm) {
            LOGINFO("Replica={} leader quick-restarting (round {})", g_helper->replica_num(), r);
            g_helper->restart(0);
        }
        write_on_leader(n); // post-blip leader (returned or takeover) proposes; the rest exit on commit proof
        expected += n;
        wait_for_commits(expected);
        g_helper->sync_for_test_start();
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// The LEADER restarts with a sleep LONGER than the election window: the survivors must elect a new leader
// during the gap and accept the round's writes mid-downtime; the old leader returns as a follower and
// catches up from the log.  Leadership rolls to a new member every round.
TEST_F(ReplicaSetTest, LeaderSlowRestart) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    constexpr uint32_t rounds = 2;
    constexpr uint32_t restart_sleep_secs = 15; // > elect_to_high (10s): takeover is guaranteed inside the gap
    uint64_t expected = 0;
    for (uint32_t r = 0; r < rounds; ++r) {
        g_helper->sync_for_test_start();
        auto const lm = wait_till_leader_elected();
        if (g_helper->replica_num() == lm) {
            LOGINFO("Replica={} leader slow-restarting (round {})", g_helper->replica_num(), r);
            g_helper->restart(restart_sleep_secs);
        }
        write_on_leader(n); // the takeover leader proposes mid-gap; the returned member exits on commit proof
        expected += n;
        wait_for_commits(expected); // the returned member reaches this only by catch-up
        g_helper->sync_for_test_start();
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// A lagging replica (every data dispatch delayed via flip) stays behind while both fast replicas restart one
// at a time; commits keep flowing throughout (raft quorum acks log appends, not applies).  The slow replica
// must converge to the full commit count while still slow, and validation must match on every replica.
TEST_F(ReplicaSetTest, SlowReplicaRollingRestart) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    auto const slow = replicas - 1; // applies slowly and is never restarted
    g_helper->sync_for_test_start();

    if (g_helper->replica_num() == slow) {
        // Delay only DATA dispatches (journal type <= HS_DATA_INDIRECT): ctrl commits (destroy/replace) stay
        // fast so teardown isn't slowed.
        g_helper->set_delay_flip(
            "simulate_slow_replica_commit", 3000 /*usec per apply*/, 1000000, 100,
            {{"journal_type", flip::Operator::LESS_THAN_OR_EQUAL, to_int(homestore::JournalType::HS_DATA_INDIRECT)}});
        LOGINFO("Replica={} armed simulate_slow_replica_commit for data types", g_helper->replica_num());
    }
    g_helper->sync_for_test_start();

    uint64_t expected = 0;
    for (uint32_t m = 0; m + 1 < replicas; ++m) { // restart every fast replica, one at a time
        g_helper->sync_for_test_start();
        if (g_helper->replica_num() == m) {
            // Sleep past the election window: if this member happened to be the leader, the survivors
            // (including the slow one) elect a takeover inside the gap and the round's writes proceed.
            g_helper->restart(15);
        }
        write_on_leader(n);
        expected += n;
        if (g_helper->replica_num() != slow) {
            wait_for_commits(expected); // fast replicas gate each round; the slow one lags by design
        }
        g_helper->sync_for_test_start();
    }

    // Converge while STILL slow — a lagging replica isn't cured before it catches up; it must reach the full
    // commit count at its delayed pace.  The flip is removed afterwards only so it can't leak into later
    // tests (flips are process-global and survive in-process restarts).
    wait_for_commits(expected);
    if (g_helper->replica_num() == slow) {
        g_helper->remove_flip("simulate_slow_replica_commit");
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// A destroy interrupted by a clean restart: HS_CTRL_DESTROY commits (destroy_pending persisted in every
// member's SB) but the cluster restarts before the reaper's grace elapses.  On recovery the pending destroy
// is re-staged from the SB, and the reaper must erase the group on every member WITHOUT anyone re-issuing a
// destroy — the poll below goes straight to the service, bypassing the helper's leader-side re-issue path.
TEST_F(ReplicaSetTest, DestroyPendingRestartReap) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    auto const gid = repl_set()->group_id();
    g_helper->sync_for_test_start();

    // wait_for_destroy=false returns once THIS replica sees destroy_pending committed, so every member's SB
    // carries it before anyone restarts.
    g_helper->destroy_replica_set(false /* wait_for_destroy */);
    g_helper->sync_for_test_start();
    g_helper->restart(0);

    for (uint32_t waited_ms = 0;; waited_ms += 200) {
        auto const rs = homestore::repl_service().get_replica_set(gid);
        if (!rs.hasValue() || !rs.value()) {
            break; // reaper finished the reloaded destroy on its own
        }
        RELEASE_ASSERT(waited_ms < 30000u, "reloaded destroy_pending set not reaped within 30s");
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    LOGINFO("Replica={} reloaded destroy_pending group reaped without a re-issued destroy", g_helper->replica_num());

    g_helper->sync_for_verify_start();
    g_helper->sync_for_cleanup_start(); // no data validation — the group's store is gone by design
}

// A follower restarts with the leader's append stream live against it — no quiescing barrier, no commit
// wait.  The leader keeps committing on the surviving quorum through the blip; the returned follower joins
// the stream mid-batch and converges to the full count.
TEST_F(ReplicaSetTest, TrafficInflightFollowerRestart) {
    auto const n = 4 * SISL_OPTIONS["num_io"].as< uint64_t >(); // batch large enough to stay in flight across the blip
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    auto const lm = wait_till_leader_elected();
    auto const victim = (lm + 1) % replicas;
    if (g_helper->replica_num() == victim) {
        while (commit_count() == 0) { // restart mid-stream, not before traffic reaches this replica
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        LOGINFO("Replica={} restarting with {} entries in flight against it", g_helper->replica_num(), n);
        g_helper->restart(0);
    }
    write_on_leader(n);
    wait_for_commits(n); // the victim reaches this by rejoining the stream + catch-up

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// BOTH followers restart at once — quorum is lost while they are down.  The leader's proposals stall (nothing
// can commit on a 1/3 cluster) and must resume exactly-once when the followers return; leadership never moves
// (expiry is pinned) so the same leader carries the batch through the outage.
TEST_F(ReplicaSetTest, TwoFollowersSimultaneousRestart) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    auto const lm = wait_till_leader_elected();
    if (g_helper->replica_num() != lm) {
        g_helper->restart(5); // both followers down together
    }
    write_on_leader(n); // the leader proposes into the outage; commits resume with the returning quorum
    wait_for_commits(n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Full-cluster restart with a rotating boot order: each round staggers who returns first/last, so the leader
// boots first in some rounds and last in others.  Early booters hold a stale leader claim for a full election
// window; every ordering must settle to a leader that takes the round's writes.
TEST_F(ReplicaSetTest, BootOrderRotation) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    constexpr uint32_t rounds = 3;
    g_helper->sync_for_test_start();

    uint64_t expected = 0;
    for (uint32_t r = 0; r < rounds; ++r) {
        g_helper->sync_for_test_start();
        write_on_leader(n);
        expected += n;
        wait_for_commits(expected);
        g_helper->sync_for_test_start(); // everyone holds this round's commits before anyone restarts
        g_helper->restart(3u * ((g_helper->replica_num() + r) % replicas)); // rotate who boots first each round
        g_helper->sync_for_test_start(); // everyone is back before the next round writes
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Back-to-back full-cluster restarts with NOTHING written between them: every boot must see an empty replay
// window and identical SB/watermark state, with no drift across iterations.  A single batch at the end proves
// the group is still fully live.
TEST_F(ReplicaSetTest, ZeroWriteRestartChurn) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    constexpr uint32_t churns = 4;
    g_helper->sync_for_test_start();

    for (uint32_t i = 0; i < churns; ++i) {
        g_helper->sync_for_test_start();
        g_helper->restart(0);
        g_helper->sync_for_test_start();
    }
    write_on_leader(n);
    wait_for_commits(n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: election messages get lost.
//   1. Three replicas, healthy, baseline data committed.
//   2. The leader goes away, forcing an election — but the vote messages themselves get eaten by the network:
//      candidates ask for votes and hear nothing, so rounds of elections fail.
//   3. The network heals (the drop budget runs out); the very next round elects exactly one leader.
//   4. Writes resume and all replicas converge — the failed rounds did no damage, and at no point did two
//      nodes act as leader for the same term (divergence would fail the exact-state validation).
TEST_F(ReplicaSetTest, DroppedVotesElection) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    constexpr int kRequestVoteMsgType = 1; // nuraft::msg_type::request_vote_request
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    g_helper->sync_for_test_start();

    auto const lm = wait_till_leader_elected();
    // Every replica eats its first few inbound vote requests — several election rounds die on the wire.
    g_helper->set_flip("simulate_drop_repl_rpc", 4, 100, {{"msg_type", flip::Operator::EQUAL, kRequestVoteMsgType}});
    g_helper->sync_for_test_start();

    if (g_helper->replica_num() == lm) {
        g_helper->restart(8); // leader away past the election window; survivors campaign into the drop window
    }
    write_on_leader(n); // blocks on every replica until a leader finally emerges, then lands the batch
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: a node reboots while being invited into a group.
//   1. A replica set is being formed: the creator sends each member an invitation to join.
//   2. One member's invitation is lost — the member is rebooting at that moment and never processes it.
//   3. The creator does not give up: it keeps re-inviting while the member boots back up.
//   4. The re-invitation lands after the reboot; the member joins normally; the final membership is complete
//      and writes across the full group prove it — the group never forms half-made.
TEST_F(ReplicaSetTest, JoinInvitationInFlightRestart) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    constexpr int kJoinClusterMsgType = 12; // nuraft::msg_type::join_cluster_request
    g_helper->sync_for_test_start();

    // The fixture's group is already formed; rebuild it with the fault armed so the JOIN itself is the thing
    // under test.
    g_helper->destroy_replica_set(/*wait_for_destroy=*/true);
    g_helper->sync_for_test_start();

    auto const victim = 1u; // any non-creator member
    if (g_helper->replica_num() == victim) {
        // Eat the first invitation, and reboot so the retry lands on the fresh incarnation.
        g_helper->set_flip("simulate_drop_repl_rpc", 1, 100,
                           {{"msg_type", flip::Operator::EQUAL, kJoinClusterMsgType}});
        g_helper->restart(0);
    }
    g_helper->register_replica_set(store_); // creator re-creates the group; its add-member retries ride out both
                                            // the eaten invitation and the victim's reboot window
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}
