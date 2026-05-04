/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Author/Developer(s): Harihara Kadayam
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
// Port of workspace_sds/sisl/src/flip/client/local/test_flip_local_client.cpp.  Exercises FlipClient's
// inject_*_flip builders against the new flatbuffer-based API: FlipConditionT / FlipFrequencyT replace the old
// protobuf types, and Flip::instance() is the singleton (the old test used a per-instance Flip).
//
#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex> // must precede folly/Lock.h (pulled in via gtest/folly headers below)
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <sisl/options/options.h>
#include <sisl/logging/logging.h>

#include "sisl/flip/flip.h"
#include "sisl/flip/flip_client.h"

namespace {

flip::FlipFrequencyT make_freq(uint32_t count, uint32_t percent) {
    flip::FlipFrequencyT f;
    f.count = count;
    flip::PercentFrequencyT pf;
    pf.v = percent;
    f.kind.Set(pf);
    return f;
}

void inject_all_flips() {
    auto& fclient = flip::FlipClient::instance();

    // No-return action — fires for cmd_type == 1, count=2.
    auto cond1 = fclient.create_condition< int >("cmd_type", flip::Operator::EQUAL, 1);
    fclient.inject_noreturn_flip("noret_flip", {cond1}, make_freq(2, 100));

    // Retval action — fires for vol_name == "vol1" AND dev_name matches "/dev/", returns "Simulated error value".
    auto cond2 = fclient.create_condition< std::string >("vol_name", flip::Operator::EQUAL, "vol1");
    auto cond6 = fclient.create_condition< std::string >("dev_name", flip::Operator::REG_EX, "\\/dev\\/");
    fclient.inject_retval_flip< std::string >("simval_flip", {cond2, cond6}, make_freq(2, 100), "Simulated error value");

    // Delay action — fires for cmd_type == 1 AND size_bytes <= 2048, delays 100ms.
    auto cond3 = fclient.create_condition< int >("cmd_type", flip::Operator::EQUAL, 1);
    auto cond4 = fclient.create_condition< long >("size_bytes", flip::Operator::LESS_THAN_OR_EQUAL, (long)2048);
    fclient.inject_delay_flip("delay_flip", {cond3, cond4}, make_freq(2, 100), 100000);

    // Delay-and-retval — fires for double_val != 1.85, delays 1s and returns "Simulated delayed errval".
    auto cond5 = fclient.create_condition< double >("double_val", flip::Operator::NOT_EQUAL, 1.85);
    fclient.inject_delay_and_retval_flip< std::string >("delay_simval_flip", {cond5}, make_freq(2, 100), 1000000,
                                                        "Simulated delayed errval");
}

} // namespace

class FlipLocalClientTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { inject_all_flips(); }
};

TEST_F(FlipLocalClientTest, NoretFlip) {
    auto& flip = flip::Flip::instance();
    int valid_cmd = 1;
    int invalid_cmd = -1;

    EXPECT_FALSE(flip.test_flip("noret_flip", invalid_cmd));
    EXPECT_TRUE(flip.test_flip("noret_flip", valid_cmd));
    EXPECT_FALSE(flip.test_flip("noret_flip", invalid_cmd));
    EXPECT_TRUE(flip.test_flip("noret_flip", valid_cmd));
    EXPECT_FALSE(flip.test_flip("noret_flip", valid_cmd)); // count was 2; third must miss
}

TEST_F(FlipLocalClientTest, RetFlip) {
    auto& flip = flip::Flip::instance();
    std::string my_vol = "vol1";
    std::string valid_dev = "/dev/sda";
    std::string unknown_vol = "unknown_vol";
    std::string invalid_dev = "/boot/sda";

    auto result = flip.get_test_flip< std::string >("simval_flip", my_vol, valid_dev);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Simulated error value");

    EXPECT_FALSE(flip.get_test_flip< std::string >("simval_flip", unknown_vol, valid_dev).has_value());
    EXPECT_FALSE(flip.get_test_flip< std::string >("simval_flip", my_vol, invalid_dev).has_value());

    result = flip.get_test_flip< std::string >("simval_flip", my_vol, valid_dev);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Simulated error value");

    EXPECT_FALSE(flip.get_test_flip< std::string >("simval_flip", my_vol, valid_dev).has_value()); // 3rd must miss
}

TEST_F(FlipLocalClientTest, DelayFlip) {
    auto& flip = flip::Flip::instance();
    int valid_cmd = 1;
    long size_2047 = 2047;
    long size_2048 = 2048;
    int invalid_cmd = -1;
    long size_4096 = 4096;
    auto closure_calls = std::make_shared< std::atomic< int > >(0);

    EXPECT_TRUE(flip.delay_flip(
        "delay_flip", [closure_calls]() { ++(*closure_calls); }, valid_cmd, size_2047));
    EXPECT_FALSE(flip.delay_flip(
        "delay_flip", [closure_calls]() { ++(*closure_calls); }, invalid_cmd, size_2047));
    EXPECT_TRUE(flip.delay_flip(
        "delay_flip", [closure_calls]() { ++(*closure_calls); }, valid_cmd, size_2048));
    EXPECT_FALSE(flip.delay_flip(
        "delay_flip", [closure_calls]() { ++(*closure_calls); }, valid_cmd, size_4096));
    EXPECT_FALSE(flip.delay_flip(
        "delay_flip", [closure_calls]() { ++(*closure_calls); }, valid_cmd, size_2047));

    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(closure_calls->load(), 2);
}

TEST_F(FlipLocalClientTest, DelayReturnFlip) {
    auto& flip = flip::Flip::instance();
    double valid = 2.0;
    double invalid = 1.85;
    auto closure_calls = std::make_shared< std::atomic< int > >(0);

    EXPECT_TRUE(flip.get_delay_flip< std::string >(
        "delay_simval_flip",
        [closure_calls](std::string e) {
            ++(*closure_calls);
            EXPECT_EQ(e, "Simulated delayed errval");
        },
        valid));

    EXPECT_FALSE(flip.get_delay_flip< std::string >(
        "delay_simval_flip", [closure_calls](std::string) { ++(*closure_calls); }, invalid));

    EXPECT_TRUE(flip.get_delay_flip< std::string >(
        "delay_simval_flip",
        [closure_calls](std::string e) {
            ++(*closure_calls);
            EXPECT_EQ(e, "Simulated delayed errval");
        },
        valid));

    EXPECT_FALSE(flip.get_delay_flip< std::string >(
        "delay_simval_flip", [closure_calls](std::string) { ++(*closure_calls); }, invalid));

    EXPECT_FALSE(flip.get_delay_flip< std::string >(
        "delay_simval_flip", [closure_calls](std::string) { ++(*closure_calls); }, valid));

    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(closure_calls->load(), 2);
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_flip_local_client");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%n] [%t] %v");
    return RUN_ALL_TESTS();
}