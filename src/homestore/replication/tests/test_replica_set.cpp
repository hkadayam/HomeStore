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
// Multi-process replication bring-up + convergence test: stand up an N-replica raft group, propose a batch of
// writes on the leader, and verify every replica converged to the identical committed key→value contents.
//
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "homestore/base/hs_runtime_config.h" // HS_SETTINGS_FACTORY
#include "repl_test_base.h"

using namespace test_common;

// The one multi-process helper for this run (declared extern in repl_test_base.h).
std::unique_ptr< HSReplTestHelper > test_common::g_helper;

class ReplicaSetTest : public ReplicaSetTestBase {};

TEST_F(ReplicaSetTest, ReplicatedWrites) {
    auto const n = SISL_OPTIONS["num_io"].as< uint64_t >();
    g_helper->sync_for_test_start();

    write_on_leader(n);
    wait_for_commits(n);

    g_helper->sync_for_verify_start();
    validate_data(n);
    g_helper->sync_for_cleanup_start();
}

int main(int argc, char* argv[]) {
    // Capture the full argv before gtest strips its own flags — peer processes are re-spawned with these args.
    std::vector< std::string > args(argv, argv + argc);

    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_replica_set");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    // Pin leadership so the replica that creates the group stays leader for the whole test — makes the write path
    // deterministic (the leader never yields mid-run).
    HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) { s.consensus.leadership_expiry_ms = -1; });
    HS_SETTINGS_FACTORY().save();

    g_helper = std::make_unique< HSReplTestHelper >("test_replica_set", args, argv);
    g_helper->setup(SISL_OPTIONS["replicas"].as< uint32_t >());

    auto const ret = RUN_ALL_TESTS();
    g_helper->teardown();
    // Release the helper (and the per-replica listeners it holds → their MemBtree state machines) before main
    // returns, while MemBtreeDrainer's function-local-static singleton is still alive. Left to static destruction,
    // g_helper outlives the drainer and ~MemBtree's deregister() would touch a freed drainer.
    g_helper.reset();
    return ret;
}
