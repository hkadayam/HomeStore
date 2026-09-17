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

// Membership and leadership operations.  A member joining mid-test has no process until a test forks one:
// identities are derived from the member index, so any index is addressable before it exists.  The forked
// process runs only these cases (its command line carries a matching --gtest_filter), and the barriers below
// are sized to include it, so its boot time is simply absorbed by the first barrier it shares.

#include <gtest/gtest.h>

#include "sisl/options/options.h"
#include "repl_test_base.h"

using namespace test_common;

// The member index a test forks when it needs a fourth member, and the barrier width while it is in the group.
static uint16_t joiner_idx() { return to_u16(SISL_OPTIONS["replicas"].as< uint32_t >()); }
static uint32_t with_joiner() { return SISL_OPTIONS["replicas"].as< uint32_t >() + 1; }

// Every case here forks a process for the joining member, which is impossible when the run supplies devices
// explicitly (that list is sliced across the bootstrap members only).
#define SKIP_IF_MEMBERS_CANNOT_BE_ADDED()                                                                              \
    if (!HSReplTestHelper::can_add_members()) {                                                                        \
        GTEST_SKIP() << "membership tests need auto-generated devices (--replica_dev_list given)";                      \
    }

// Scenario: a new member joins an existing group.
//   1. Three replicas, healthy, with data already written and committed.
//   2. A fourth process is forked and the leader invites it into the group.
//   3. The new member catches up on everything written before it existed.
//   4. Further writes commit across all four; every replica ends with identical data, and each one's view of
//      the group lists four members.
TEST_F(ReplicaSetTest, AddMemberJoinsAndCatchesUp) {
    SKIP_IF_MEMBERS_CANNOT_BE_ADDED();
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const joiner = joiner_idx();

    if (g_helper->replica_num() != joiner) {
        g_helper->sync_for_test_start(); // bootstrap members only — the joiner has no process yet
        write_on_leader(n);
        wait_for_commits(n);
        g_helper->spawn_member(joiner, "*AddMemberJoinsAndCatchesUp*"); // driver-only; others no-op

        add_member_with_retry(joiner);
    } else {
        wait_for_group_bound(); // the invitation binds this process into the group
        written_ = n;           // everything committed before this member existed
    }

    // First barrier the joiner shares: the sitting members wait here through its boot and catch-up.  Every
    // replica is told about the join; only the leader can also count the roster.
    wait_for_member_added(HSReplTestHelper::replica_id(joiner));
    wait_for_member_count(with_joiner());
    g_helper->sync_for_test_start(with_joiner());

    wait_for_commits(n); // the joiner replays the pre-join history
    write_on_leader(n);  // and takes part in new writes
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start(with_joiner());
    validate_all_data();
    g_helper->sync_for_cleanup_start(with_joiner());
}

// Scenario: a member joins as a learner, then is promoted to a voter.
//   1. Three replicas with committed data; a fourth is forked and added as a LEARNER.
//   2. A learner replicates the log but does not count toward the commit quorum — writes keep committing
//      while it catches up.
//   3. The learner is promoted to a voter and from then on counts toward quorum.
//   4. All four hold identical data.
TEST_F(ReplicaSetTest, AddLearnerThenPromote) {
    SKIP_IF_MEMBERS_CANNOT_BE_ADDED();
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const joiner = joiner_idx();

    if (g_helper->replica_num() != joiner) {
        g_helper->sync_for_test_start();
        write_on_leader(n);
        wait_for_commits(n);
        g_helper->spawn_member(joiner, "*AddLearnerThenPromote*");

        add_member_with_retry(joiner, /*learner=*/true);
    } else {
        wait_for_group_bound();
        written_ = n;
    }

    wait_for_member_added(HSReplTestHelper::replica_id(joiner));
    wait_for_member_count(with_joiner());
    g_helper->sync_for_test_start(with_joiner());

    // Writes commit while the learner is present but non-voting.
    write_on_leader(n);
    wait_for_commits(2 * n);
    g_helper->sync_for_test_start(with_joiner());

    promote_learner_with_retry(joiner);
    g_helper->sync_for_test_start(with_joiner());

    write_on_leader(n); // committed under the widened quorum
    wait_for_commits(3 * n);

    g_helper->sync_for_verify_start(with_joiner());
    validate_all_data();
    g_helper->sync_for_cleanup_start(with_joiner());
}

// Scenario: a member is removed from the group.
//   1. Four members (a fourth is forked and added), all holding the same data.
//   2. The leader removes the fourth member.
//   3. The remaining three see a three-member group and keep committing writes with the recomputed quorum.
TEST_F(ReplicaSetTest, RemoveMemberShrinksGroup) {
    SKIP_IF_MEMBERS_CANNOT_BE_ADDED();
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const joiner = joiner_idx();

    if (g_helper->replica_num() != joiner) {
        g_helper->sync_for_test_start();
        write_on_leader(n);
        wait_for_commits(n);
        g_helper->spawn_member(joiner, "*RemoveMemberShrinksGroup*");

        add_member_with_retry(joiner);
    } else {
        wait_for_group_bound();
        written_ = n;
    }

    wait_for_member_added(HSReplTestHelper::replica_id(joiner));
    wait_for_member_count(with_joiner());
    g_helper->sync_for_test_start(with_joiner());
    wait_for_commits(n);

    // Everyone is in and caught up; now drop the joiner back out.
    g_helper->sync_for_verify_start(with_joiner());
    validate_all_data();
    g_helper->sync_for_cleanup_start(with_joiner()); // the joiner leaves the barriers after this point

    if (g_helper->replica_num() == joiner) {
        return; // removed member: its process ends here
    }

    if (repl_set() && (repl_set()->get_leader_id() == g_helper->my_replica_id())) {
        auto const res =
            iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [rs = repl_set(), joiner]() -> Async< ReplResult<> > {
                co_return co_await rs->remove_member(member_info(joiner));
            }());
        EXPECT_TRUE(res.hasValue()) << "remove_member failed";
    }

    wait_for_member_count(SISL_OPTIONS["replicas"].as< uint32_t >());
    write_on_leader(n); // quorum recomputed over the surviving three
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: one member is replaced by another in a single operation.
//   1. Three replicas with committed data; a fourth process is forked to stand in for the outgoing member.
//   2. The leader calls replace_member(out, in) — internally staging the newcomer as a learner, waiting for
//      it to catch up, promoting it to a voter, and removing the outgoing member.
//   3. Every replica's listener is notified at the start and at the completion of the replacement, carrying
//      the same task id on both.
//   4. The group ends at three members with the newcomer in and the outgoing member out, data identical.
TEST_F(ReplicaSetTest, ReplaceMemberEndToEnd) {
    SKIP_IF_MEMBERS_CANNOT_BE_ADDED();
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const joiner = joiner_idx();
    auto const leaving = to_u16(SISL_OPTIONS["replicas"].as< uint32_t >() - 1); // a follower, never the creator

    if (g_helper->replica_num() != joiner) {
        g_helper->sync_for_test_start();
        write_on_leader(n);
        wait_for_commits(n);
        g_helper->spawn_member(joiner, "*ReplaceMemberEndToEnd*");
    } else {
        written_ = n; // everything committed before this member existed
    }

    // replace_member stages the newcomer itself and cannot be re-issued like a bare invitation, so the
    // newcomer announces at this barrier that it has booted and is listening before the swap is ordered.
    g_helper->sync_for_test_start(with_joiner());

    if ((g_helper->replica_num() != joiner) && repl_set() &&
        (repl_set()->get_leader_id() == g_helper->my_replica_id())) {
        auto const res = iomgr().spawn_and_block(
            iomanager::ReactorTarget::any(), [rs = repl_set(), joiner, leaving]() -> Async< ReplResult<> > {
                co_return co_await rs->replace_member(member_info(leaving), member_info(joiner));
            }());
        EXPECT_TRUE(res.hasValue()) << "replace_member failed";
    }

    if (g_helper->replica_num() == joiner) {
        wait_for_group_bound();
        wait_for_commits(n); // the newcomer catches up as part of the replacement
    }

    // The outgoing member drops out of the group; the rest (incl. the newcomer) settle at the same size.
    if (g_helper->replica_num() != leaving) {
        wait_for_member_count(SISL_OPTIONS["replicas"].as< uint32_t >());
    }

    // Both hooks fired on this replica, naming the same pair and the same task id.
    if (auto const l = g_helper->listener(); l && (g_helper->replica_num() != leaving)) {
        auto const started = l->replace_started();
        auto const completed = l->replace_completed();
        ASSERT_FALSE(started.empty()) << "on_start_replace_member never fired";
        ASSERT_FALSE(completed.empty()) << "on_complete_replace_member never fired";
        EXPECT_EQ(started.back().out, HSReplTestHelper::replica_id(leaving));
        EXPECT_EQ(started.back().in, HSReplTestHelper::replica_id(joiner));
        EXPECT_EQ(completed.back().out, started.back().out);
        EXPECT_EQ(completed.back().in, started.back().in);
        EXPECT_EQ(completed.back().task_id, started.back().task_id) << "replace task id differs across the two hooks";
        EXPECT_FALSE(started.back().task_id.empty());
    }

    g_helper->sync_for_verify_start(with_joiner());
    if (g_helper->replica_num() != leaving) {
        validate_all_data();
    }
    g_helper->sync_for_cleanup_start(with_joiner());
}

// Scenario: membership changes while writes are in flight.
//   1. Three replicas taking a steady stream of writes.
//   2. A fourth member is forked and added in the middle of that stream — no quiescing.
//   3. Writes continue throughout and all four converge on identical data.
TEST_F(ReplicaSetTest, AddMemberUnderActiveWrites) {
    SKIP_IF_MEMBERS_CANNOT_BE_ADDED();
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const joiner = joiner_idx();

    if (g_helper->replica_num() != joiner) {
        g_helper->sync_for_test_start();
        write_on_leader(n);
        wait_for_commits(n);
        g_helper->spawn_member(joiner, "*AddMemberUnderActiveWrites*");

        // Invite the newcomer and keep proposing without waiting for it to settle.
        add_member_with_retry(joiner);
        write_on_leader(n); // in flight across the config change
        wait_for_commits(2 * n);
    } else {
        wait_for_group_bound();
        written_ = 2 * n;
    }

    wait_for_member_added(HSReplTestHelper::replica_id(joiner));
    wait_for_member_count(with_joiner());
    g_helper->sync_for_test_start(with_joiner());
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start(with_joiner());
    validate_all_data();
    g_helper->sync_for_cleanup_start(with_joiner());
}

// ── Leadership operations (no joining member — the bootstrap group suffices) ──────────────────────────────────

// Scenario: leadership is handed to a specific member while writes are flowing.
//   1. Three replicas under a steady write stream.
//   2. A follower is asked to take over leadership.
//   3. Leadership settles on that member, writes continue through the handover, and data stays identical.
TEST_F(ReplicaSetTest, LeadershipTransferUnderWrites) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    auto const lm = wait_till_leader_elected();
    auto const target = to_u16((lm + 1) % replicas);
    g_helper->sync_for_test_start();

    g_helper->assign_leader(target); // collective: the target asks, everyone waits for it to settle
    EXPECT_EQ(wait_till_leader_elected({target}), target);

    write_on_leader(n); // writes resume under the new leader
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: every role transition is reported to the application.
//   1. Three replicas; leadership is moved to a follower and then moved back.
//   2. Each replica's listener must have been told about the transitions it went through — the node that took
//      leadership saw LEADER, the one that gave it up saw FOLLOWER afterwards.
TEST_F(ReplicaSetTest, RoleChangeNotifications) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    auto const first = wait_till_leader_elected();
    auto const second = to_u16((first + 1) % replicas);
    g_helper->sync_for_test_start();

    g_helper->assign_leader(second);
    EXPECT_EQ(wait_till_leader_elected({second}), second);
    g_helper->sync_for_test_start();

    g_helper->assign_leader(first); // and back again
    EXPECT_EQ(wait_till_leader_elected({first}), first);
    g_helper->sync_for_test_start();

    if (auto const l = g_helper->listener(); l) {
        auto const history = l->role_history();
        ASSERT_FALSE(history.empty()) << "on_change_in_role never fired on this replica";
        if (g_helper->replica_num() == second) {
            EXPECT_NE(std::find(history.begin(), history.end(), ReplicaRole::LEADER), history.end())
                << "the member that took leadership was never told it became LEADER";
        }
        if (g_helper->replica_num() == first) {
            EXPECT_EQ(l->role(), ReplicaRole::LEADER) << "the final leader's last reported role should be LEADER";
        }
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: election priority is propagated and honoured.
//   1. Three replicas; one member's election priority is raised above the others.
//   2. Every replica's replication status reports the new priority for that member.
//   3. Writes keep committing — a priority change is a config change like any other.
TEST_F(ReplicaSetTest, ElectionPriorityPropagates) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    auto const lm = wait_till_leader_elected();
    auto const target = to_u16((lm + 1) % replicas);
    constexpr int32_t kRaisedPriority = 150; // members default to 100, so this is genuinely a promotion
    g_helper->sync_for_test_start();

    if (repl_set() && (repl_set()->get_leader_id() == g_helper->my_replica_id())) {
        auto const res = iomgr().spawn_and_block(
            iomanager::ReactorTarget::any(), [rs = repl_set(), target]() -> Async< ReplResult<> > {
                co_return co_await rs->set_priority(HSReplTestHelper::replica_id(target), kRaisedPriority);
            }());
        EXPECT_TRUE(res.hasValue()) << "set_priority failed";
    }

    // The engine reports per-member config (priority, learner flag) only through the leader's peer table, so
    // the new value is asserted there; the other replicas assert the change did no harm by converging below.
    if (i_am_leader()) {
        for (uint32_t waited_ms = 0;; waited_ms += 500) {
            bool seen = false;
            for (auto rs = repl_set();
                 auto const& p : iomgr().spawn_and_block(iomanager::ReactorTarget::any(),
                                                         [rs]() -> Async< std::vector< PeerInfo > > {
                                                             co_return co_await rs->get_replication_status();
                                                         }())) {
                if ((p.id_ == HSReplTestHelper::replica_id(target)) && (p.priority_ == uint32_t(kRaisedPriority))) {
                    seen = true;
                }
            }
            if (seen) {
                break;
            }
            RELEASE_ASSERT(waited_ms < 60000u, "raised priority not visible on the leader within 60s");
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
    }

    // A priority change can move leadership (that is its purpose), so let the group settle before proposing —
    // otherwise the write races the handover and waits on a leader that is stepping down.
    g_helper->sync_for_test_start();
    wait_till_leader_elected();

    write_on_leader(n);
    wait_for_commits(2 * n);

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Scenario: replication status reflects the true membership and each peer's progress.
//   1. Three replicas with committed data.
//   2. The leader's status lists exactly the group's members (the engine keeps the peer table on the leader,
//      so a follower's status is empty by design).
//   3. Each follower's reported replication index has advanced past the start — the status tracks real
//      progress rather than a static roster.
TEST_F(ReplicaSetTest, StatusReflectsMembershipAndProgress) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    auto const replicas = SISL_OPTIONS["replicas"].as< uint32_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    g_helper->sync_for_test_start();

    // Membership and per-peer progress are reported through the leader's peer table; a follower's status is
    // empty by contract, so both assertions belong on the leader.
    if (i_am_leader()) {
        // The peer table holds the leader's peers, so it names every member except the leader itself.
        auto const peers = current_members();
        EXPECT_EQ(peers.size(), size_t(replicas - 1));
        for (uint16_t i{0}; i < to_u16(replicas); ++i) {
            auto const id = HSReplTestHelper::replica_id(i);
            if (id == g_helper->my_replica_id()) {
                EXPECT_EQ(peers.find(id), peers.end()) << "the leader should not list itself as a peer";
                continue;
            }
            EXPECT_NE(peers.find(id), peers.end()) << "member " << i << " missing from replication status";
        }

        auto rs = repl_set();
        auto const status =
            iomgr().spawn_and_block(iomanager::ReactorTarget::any(), [rs]() -> Async< std::vector< PeerInfo > > {
                co_return co_await rs->get_replication_status();
            }());
        for (auto const& p : status) {
            EXPECT_GT(p.replication_idx_, 0ull) << "follower reported no replication progress";
        }
    }

    g_helper->sync_for_verify_start();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}
