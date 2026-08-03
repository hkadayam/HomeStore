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
// Replication test binary entry point + the canonical bring-up/convergence smoke test. The full suite is split by
// category across sibling .cpp files (test_repl_writes/recovery/... — Category A/B/...) that all compile into this
// one binary and share the ReplicaSetTest fixture from repl_test_base.h; main() and the smoke test live here.
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
    HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
        s.consensus.leadership_expiry_ms = -1;
        // Election timeout must tolerate the multi-second btree store recovery: on a full-cluster restart the replicas
        // boot at different speeds, and with the default ~800-1700ms window a fast replica election-times-out and steals
        // leadership before a slow-booting peer's raft is even up, churning leadership so teardown/destroy never settles.
        // Widen it well past the worst-case recovery boot so the pre-restart leader reasserts before anyone re-elects.
        s.consensus.elect_to_low_ms = 5000;
        s.consensus.elect_to_high_ms = 10000;
        // Fast, grace-free group-destroy reaping so each test's teardown completes promptly between tests.
        s.consensus.replica_set_reaper_scan_interval_ms = 1000;
        s.consensus.replica_set_reaper_grace_sec = 0;
    });
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
