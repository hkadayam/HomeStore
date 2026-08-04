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
#include "repl_test_base.h"

using namespace test_common;

TEST_F(ReplicaSetTest, WriteRestartMultiple) {
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
