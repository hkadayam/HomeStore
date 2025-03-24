/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
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
#include <gtest/gtest.h>
#include <boost/uuid/random_generator.hpp>

#include <sisl/utility/enum.hpp>
#include "common/homestore_config.hpp"
#include "common/resource_mgr.hpp"
#include "test_common/homestore_test_common.hpp"
#include "test_common/range_scheduler.hpp"
#include "btree_helpers/btree_test_helper.hpp"
#include "btree_helpers/btree_test_kvs.hpp"
#include "btree_helpers/btree_decls.h"

using namespace homestore;

SISL_OPTIONS_ENABLE(logging, test_cow_btree_recovery, iomgr, test_common_setup)

// TODO Add tests to do write,remove after recovery.
// TODO Test with var len key with io mgr page size is 512.

SISL_OPTION_GROUP(test_cow_btree_recovery,
                  (num_iters, "", "num_iters", "number of iterations for rand ops",
                   ::cxxopts::value< uint32_t >()->default_value("500"), "number"),
                  (num_btrees, "", "num_btrees", "number of btrees to test",
                   ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
                  (num_entries, "", "num_entries", "number of entries per btree to test with",
                   ::cxxopts::value< uint32_t >()->default_value("50000"), "number"),
                  (run_time, "", "run_time", "run time for io", ::cxxopts::value< uint32_t >()->default_value("360000"),
                   "seconds"),
                  (disable_merge, "", "disable_merge", "disable_merge", ::cxxopts::value< bool >()->default_value("0"),
                   ""),
                  (preload_size, "", "preload_size", "number of entries to preload tree with",
                   ::cxxopts::value< uint32_t >()->default_value("1000"), "number"),
                  (seed, "", "seed", "random engine seed, use random if not defined",
                   ::cxxopts::value< uint64_t >()->default_value("0"), "number"))

void log_obj_life_counter() {
    std::string str;
    sisl::ObjCounterRegistry::foreach ([&str](const std::string& name, int64_t created, int64_t alive) {
        fmt::format_to(std::back_inserter(str), "{}: created={} alive={}\n", name, created, alive);
    });
    LOGINFO("Object Life Counter\n:{}", str);
}

struct BtreeTest : public ::testing::Test {
    using T = VarObjSizeBtree< IndexStore::Type::COPY_ON_WRITE_BTREE >;
    using K = typename T::KeyType;
    using V = typename T::ValueType;

    class TestIndexServiceCallbacks : public IndexServiceCallbacks {
    public:
        TestIndexServiceCallbacks(BtreeTest* test) : m_test(test) {}
        std::shared_ptr< Index > on_index_table_found(superblk< IndexSuperBlock >&& sb) override {
            auto bt_helper = m_test->m_bt_helpers[m_test->m_recovered++].get();
            bt_helper->m_bt = std::make_shared< Btree< K, V > >(bt_helper->m_cfg, std::move(sb));
            return bt_helper->m_bt;
        }

    private:
        BtreeTest* m_test;
    };
    friend class TestIndexServiceCallbacks;

    BtreeTest() : testing::Test() {}

    void SetUp() override {
        m_helper.start_homestore(
            "test_btree",
            {{ServiceType::META, {.size_pct = 10.0}},
             {ServiceType::INDEX, {.size_pct = 70.0, .index_svc_cbs = new TestIndexServiceCallbacks(this)}}},
            nullptr, {homestore::dev_info{"", homestore::HSDevType::Fast, 0}});
        // For persistent btree, we try to create a default size, but with only 1 device explictly, since this tests
        // start restart homestore several times and its better to use 1 disk always.

        // Test cp flush of write back.
        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.generic.cache_max_throttle_cnt = 10000;
            HS_SETTINGS_FACTORY().save();
        });
        homestore::hs()->resource_mgr().reset_dirty_buf_qd();

        // Create index table and attach to index service.
        auto const multi_threaded =
            (testing::UnitTest::GetInstance()->current_test_info()->name() == std::string("ConcurrentMultiOps"));

        for (uint32_t i{0}; i < SISL_OPTIONS["num_btrees"].as< uint32_t >(); ++i) {
            auto uuid = boost::uuids::random_generator()();
            auto parent_uuid = boost::uuids::random_generator()();

            auto bt_helper = std::make_unique< BtreeTestHelper< T > >();
            bt_helper->SetUp(true /* multi_threaded */);
            bt_helper->m_bt = std::make_shared< Btree< K, V > >(bt_helper->m_cfg, uuid, parent_uuid, 0);
            hs()->index_service().add_index_table(bt_helper->m_bt);
            m_bt_helpers.emplace_back(std::move(bt_helper));
        }
    }

    void fillup_btrees() {
        std::vector< std::string > input_ops = {"put:70", "remove:30"};
        for (auto& bt_helper : this->m_bt_helpers) {
            bt_helper->multi_op_execute(bt_helper->build_op_list(input_ops), false /* skip_preload */);
        }
    }

    void validate_btrees() {
        for (auto& bt_helper : this->m_bt_helpers) {
            bt_helper->query_all_paginate(500);
        }
    }

    void TearDown() override {
        for (auto& bt_helper : this->m_bt_helpers) {
            hs()->index_service().destroy_index_table(bt_helper->m_bt);
            bt_helper->m_bt.reset();
            bt_helper->TearDown();
        }
        m_helper.shutdown_homestore(false);
        log_obj_life_counter();
    }

    void restart_homestore() {
        m_recovered = 0;
        m_helper.params(HS_SERVICE::INDEX).index_svc_cbs = new TestIndexServiceCallbacks(this);
        for (auto& bt_helper : this->m_bt_helpers) {
            bt_helper->m_bt.reset();
        }

        m_helper.restart_homestore();
    }

    void restart_and_validate() {
        LOGINFO("Restart homestore and validate if before and after states of btrees are identical");
        for (auto& bt_helper : this->m_bt_helpers) {
            std::string fname = fmt::format("/tmp/btree_{}_before.txt", bt_helper->m_bt->ordinal());
            bt_helper->dump_to_file(fname);
        }
        restart_homestore();
        std::this_thread::sleep_for(std::chrono::seconds{1});
        LOGINFO(" Restarted homestore with {} indexes recovered", m_recovered);

        // TODO: Vadlidate if the expected recovered == total recovered

        for (auto& bt_helper : this->m_bt_helpers) {
            std::string before_fname = fmt::format("/tmp/btree_{}_before.txt", bt_helper->m_bt->ordinal());
            std::string after_fname = fmt::format("/tmp/btree_{}_after.txt", bt_helper->m_bt->ordinal());
            bt_helper->dump_to_file(after_fname);
            bt_helper->compare_files(before_fname, after_fname); // Validate with dumping
            bt_helper->query_all_paginate(500); // Validate with query as well.
        }
    }

    void incremental_map_cp_then_restart_validate() {
        LOGINFO("Setup to do incremental map cp flush and then trigger CP");
        // Modify the settings to take incremental map flushes only once
        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.cow_max_incremental_map_flushes = 100000;
            HS_SETTINGS_FACTORY().save();
        });
        test_common::HSTestHelper::trigger_cp(true /* wait */);
        restart_and_validate();
    }

    void fullmap_cp_then_restart_validate() {
        LOGINFO("Setup to do full map cp flush and then trigger CP");
        // Modify the settings to take incremental map flushes only once
        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.cow_max_incremental_map_flushes = 0;
            HS_SETTINGS_FACTORY().save();
        });
        test_common::HSTestHelper::trigger_cp(true /* wait */);
        restart_and_validate();
    }

protected:
    test_common::HSTestHelper m_helper;
    std::vector< std::unique_ptr< BtreeTestHelper< T > > > m_bt_helpers;
    uint32_t m_recovered{0};
};

TEST_F(BtreeTest, IOFullMapFlushThenRestart) {
    LOGINFO("Fill up the {} btrees", SISL_OPTIONS["num_btrees"].as< uint32_t >());
    this->fillup_btrees();
    this->fullmap_cp_then_restart_validate();
}

TEST_F(BtreeTest, IOIncrementalMapFlushThenRestart) {
    LOGINFO("Fill up the {} btrees", SISL_OPTIONS["num_btrees"].as< uint32_t >());
    this->fillup_btrees();
    this->incremental_map_cp_then_restart_validate();
}

int main(int argc, char* argv[]) {
    int parsed_argc{argc};
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging, test_cow_btree_recovery, iomgr, test_common_setup);
    sisl::logging::SetLogger("test_cow_btree_recovery");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    if (SISL_OPTIONS.count("seed")) {
        auto seed = SISL_OPTIONS["seed"].as< uint64_t >();
        LOGINFO("Using seed {} to sow the random generation", seed);
        g_re.seed(seed);
    }
    auto ret = RUN_ALL_TESTS();
    return ret;
}
