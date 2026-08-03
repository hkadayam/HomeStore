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
// Category A — functional writes: the happy-path replication contract with no faults injected. Every case proposes
// on the leader and asserts every replica converged to the identical committed contents. These share the one binary
// with the other categories (see repl_test_base.h); each category is its own file only for readability.
//
#include <gtest/gtest.h>

#include "sisl/options/options.h"
#include "repl_test_base.h"

using namespace test_common;

// A single proposed entry must replicate and commit on every replica — the minimal end-to-end path.
TEST_F(ReplicaSetTest, SingleWrite) {
    g_helper->sync_for_test_start();

    write_on_leader(1);
    wait_for_commits(1);

    g_helper->sync_for_verify_start();
    validate_data(1);
    g_helper->sync_for_cleanup_start();
}

// Many entries issued at a high queue depth (many concurrent in-flight proposals) must all commit and converge —
// exercises the leader's concurrent append/commit path rather than the one-at-a-time case.
TEST_F(ReplicaSetTest, HighQdepthWrites) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    write_qdepth_ = 64; // deep pipeline: up to 64 proposals outstanding at once
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    g_helper->sync_for_verify_start();
    validate_data(n);
    g_helper->sync_for_cleanup_start();
}

// Several sequential write batches on the same leader: LSNs must stay contiguous across batches and the cumulative
// key set must converge. Catches per-batch commit-count / lsn-continuity bugs that a single batch would miss.
TEST_F(ReplicaSetTest, SequentialBatches) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    for (int batch = 0; batch < 3; ++batch) {
        write_on_leader(n);
        wait_for_all_commits();
    }

    g_helper->sync_for_verify_start();
    validate_all_data(); // all 3n keys
    g_helper->sync_for_cleanup_start();
}
