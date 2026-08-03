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
// Category B — clean restart / recovery: no crash, just orderly shutdown + reboot of the whole cluster. These probe
// the two durability sources that must compose on recovery: the state machine's own checkpointed btree (loaded by
// on_recover) and the raft log tail (re-applied by replay). Every case asserts no key is lost or duplicated across
// the reboot. Crash-in-the-middle variants live in the crash category; here shutdown is always graceful.
//
#include <gtest/gtest.h>

#include "sisl/options/options.h"
#include "repl_test_base.h"

using namespace test_common;

// Pure log-replay recovery: write, restart WITHOUT a checkpoint, so the entire committed set must be reconstructed
// from raft log replay alone (the btree starts empty on every replica). Everything must come back.
TEST_F(ReplicaSetTest, WriteRestartValidate) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    g_helper->restart(); // no trigger_cp() first: nothing is in the btree, all n must replay from the log

    wait_for_all_commits();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Checkpoint-then-recovery: write, checkpoint (flush the btree durably), restart. On recovery on_recover loads the
// full committed set from the btree; log replay re-fires on already-applied entries and must be idempotent (UPSERT).
TEST_F(ReplicaSetTest, CheckpointRestartValidate) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);
    trigger_cp(); // whole set is now durable in the btree

    g_helper->restart();

    wait_for_all_commits();
    validate_all_data();
    g_helper->sync_for_cleanup_start();
}

// Checkpoint boundary: write, checkpoint, write more (post-CP → only in the log), restart. Recovery must load the
// CP'd half from the btree AND replay the post-CP tail from the log — the state-machine-durability vs commit-lsn seam.
TEST_F(ReplicaSetTest, WriteCheckpointWriteRestart) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n); // first half
    wait_for_commits(n);
    trigger_cp();       // flush the btree durably

    write_on_leader(n); // second half — post-CP, lives only in the raft log until replay
    wait_for_commits(2 * n);

    g_helper->restart(); // full-cluster restart: recover btree + replay the tail

    wait_for_all_commits();
    validate_all_data(); // every key [0, 2n) must be present
    g_helper->sync_for_cleanup_start();
}

// Recover, then keep writing: the reformed cluster must re-elect a leader and accept new proposals whose LSNs
// continue past the recovered tail (write_on_leader waits for that leader). Catches recovery that leaves the
// log/commit index in a state that rejects fresh writes.
TEST_F(ReplicaSetTest, WriteRestartWrite) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    g_helper->restart();
    wait_for_all_commits(); // the recovered n

    write_on_leader(n); // n more, on the reformed cluster
    wait_for_all_commits();

    g_helper->sync_for_verify_start();
    validate_all_data(); // all 2n
    g_helper->sync_for_cleanup_start();
}

// Back-to-back restarts: replay must be idempotent across more than one reboot (each restart re-fires on_commit for
// the un-checkpointed tail). No key may be lost or double-counted after two reboots.
TEST_F(ReplicaSetTest, MultipleRestarts) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    g_helper->restart();
    wait_for_all_commits();
    g_helper->restart();
    wait_for_all_commits();

    validate_all_data();
    g_helper->sync_for_cleanup_start();
}
