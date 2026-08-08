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
