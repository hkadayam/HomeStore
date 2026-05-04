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
// Port of the old sisl flip self-test (workspace_sds/sisl/src/flip/lib/test_flip.cpp).  Rewritten to use the new
// flatbuffer-based FlipSpecT object-API; rpc-server bring-up is dropped (the rpc surface is not built — see
// flip_rpc_server.h).  Tests build a FlipSpecT directly, hand it to flip::Flip::add(), then exercise test_flip /
// get_test_flip / delay_flip / get_delay_flip and validate hit / no-hit / frequency-cap behavior.
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

namespace {

flip::FlipSpecT make_ret_fspec() {
    flip::FlipSpecT fspec;
    fspec.flip_name = "ret_fspec";

    auto cond = std::make_unique< flip::FlipConditionT >();
    cond->name = "coll_name";
    cond->oper = flip::Operator::EQUAL;
    cond->value = std::make_unique< flip::ParamValueT >();
    flip::to_param_converter< std::string >()("item_shipping", *cond->value);
    fspec.conditions.emplace_back(std::move(cond));

    fspec.flip_action = std::make_unique< flip::FlipActionT >();
    flip::ActionReturnsT ar;
    ar.retval = std::make_unique< flip::ParamValueT >();
    flip::to_param_converter< std::string >()(std::string{"Error simulated value"}, *ar.retval);
    fspec.flip_action->action.Set(std::move(ar));

    fspec.flip_frequency = std::make_unique< flip::FlipFrequencyT >();
    fspec.flip_frequency->count = 2;
    flip::PercentFrequencyT pf;
    pf.v = 100;
    fspec.flip_frequency->kind.Set(pf);
    return fspec;
}

flip::FlipSpecT make_check_fspec() {
    flip::FlipSpecT fspec;
    fspec.flip_name = "check_fspec";

    auto cond = std::make_unique< flip::FlipConditionT >();
    cond->name = "cmd_type";
    cond->oper = flip::Operator::EQUAL;
    cond->value = std::make_unique< flip::ParamValueT >();
    flip::to_param_converter< int >()(1, *cond->value);
    fspec.conditions.emplace_back(std::move(cond));

    fspec.flip_action = std::make_unique< flip::FlipActionT >();
    fspec.flip_action->action.Set(flip::NoActionT{});

    fspec.flip_frequency = std::make_unique< flip::FlipFrequencyT >();
    fspec.flip_frequency->count = 2;
    flip::PercentFrequencyT pf;
    pf.v = 100;
    fspec.flip_frequency->kind.Set(pf);
    return fspec;
}

flip::FlipSpecT make_delay_fspec() {
    flip::FlipSpecT fspec;
    fspec.flip_name = "delay_fspec";

    auto cond = std::make_unique< flip::FlipConditionT >();
    cond->name = "cmd_type";
    cond->oper = flip::Operator::EQUAL;
    cond->value = std::make_unique< flip::ParamValueT >();
    flip::to_param_converter< int >()(2, *cond->value);
    fspec.conditions.emplace_back(std::move(cond));

    fspec.flip_action = std::make_unique< flip::FlipActionT >();
    flip::ActionDelaysT ad;
    ad.delay_in_usec = 100000;
    fspec.flip_action->action.Set(std::move(ad));

    fspec.flip_frequency = std::make_unique< flip::FlipFrequencyT >();
    fspec.flip_frequency->count = 2;
    flip::PercentFrequencyT pf;
    pf.v = 100;
    fspec.flip_frequency->kind.Set(pf);
    return fspec;
}

flip::FlipSpecT make_delay_ret_fspec() {
    flip::FlipSpecT fspec;
    fspec.flip_name = "delay_ret_fspec";

    auto cond = std::make_unique< flip::FlipConditionT >();
    cond->name = "cmd_type";
    cond->oper = flip::Operator::EQUAL;
    cond->value = std::make_unique< flip::ParamValueT >();
    flip::to_param_converter< int >()(2, *cond->value);
    fspec.conditions.emplace_back(std::move(cond));

    fspec.flip_action = std::make_unique< flip::FlipActionT >();
    flip::ActionDelayedReturnsT adr;
    adr.delay_in_usec = 100000;
    adr.retval = std::make_unique< flip::ParamValueT >();
    flip::to_param_converter< std::string >()(std::string{"Delayed error simulated value"}, *adr.retval);
    fspec.flip_action->action.Set(std::move(adr));

    fspec.flip_frequency = std::make_unique< flip::FlipFrequencyT >();
    fspec.flip_frequency->count = 2;
    flip::PercentFrequencyT pf;
    pf.v = 100;
    fspec.flip_frequency->kind.Set(pf);
    return fspec;
}

} // namespace

TEST(FlipTest, RetFspec) {
    auto& flip = flip::Flip::instance();
    flip.add(make_ret_fspec());

    std::string my_coll = "item_shipping";
    std::string unknown_coll = "unknown_collection";

    auto result = flip.get_test_flip< std::string >("ret_fspec", my_coll);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Error simulated value");

    result = flip.get_test_flip< std::string >("ret_fspec", unknown_coll);
    EXPECT_FALSE(result.has_value());

    result = flip.get_test_flip< std::string >("ret_fspec", my_coll);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Error simulated value");

    // Frequency count was 2 — third hit must miss.
    result = flip.get_test_flip< std::string >("ret_fspec", my_coll);
    EXPECT_FALSE(result.has_value());
}

TEST(FlipTest, CheckFspec) {
    auto& flip = flip::Flip::instance();
    flip.add(make_check_fspec());

    int valid_cmd = 1;
    int invalid_cmd = -1;

    EXPECT_FALSE(flip.test_flip("check_fspec", invalid_cmd));
    EXPECT_TRUE(flip.test_flip("check_fspec", valid_cmd));
    EXPECT_FALSE(flip.test_flip("check_fspec", invalid_cmd));
    EXPECT_TRUE(flip.test_flip("check_fspec", valid_cmd));
    // Frequency count was 2 — third matching hit must miss.
    EXPECT_FALSE(flip.test_flip("check_fspec", valid_cmd));
}

TEST(FlipTest, DelayFspec) {
    auto& flip = flip::Flip::instance();
    flip.add(make_delay_fspec());

    int valid_cmd = 2;
    int invalid_cmd = -1;
    auto closure_calls = std::make_shared< std::atomic< int > >(0);

    EXPECT_TRUE(flip.delay_flip(
        "delay_fspec", [closure_calls]() { (*closure_calls)++; }, valid_cmd));
    EXPECT_FALSE(flip.delay_flip(
        "delay_fspec", [closure_calls]() { (*closure_calls)++; }, invalid_cmd));
    EXPECT_TRUE(flip.delay_flip(
        "delay_fspec", [closure_calls]() { (*closure_calls)++; }, valid_cmd));
    EXPECT_FALSE(flip.delay_flip(
        "delay_fspec", [closure_calls]() { (*closure_calls)++; }, invalid_cmd));
    EXPECT_FALSE(flip.delay_flip(
        "delay_fspec", [closure_calls]() { (*closure_calls)++; }, valid_cmd));

    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(closure_calls->load(), 2);
}

TEST(FlipTest, DelayReturnFspec) {
    auto& flip = flip::Flip::instance();
    flip.add(make_delay_ret_fspec());

    int valid_cmd = 2;
    int invalid_cmd = -1;
    auto closure_calls = std::make_shared< std::atomic< int > >(0);

    EXPECT_TRUE(flip.get_delay_flip< std::string >(
        "delay_ret_fspec",
        [closure_calls](std::string error) {
            ++(*closure_calls);
            EXPECT_EQ(error, "Delayed error simulated value");
        },
        valid_cmd));

    EXPECT_FALSE(flip.get_delay_flip< std::string >(
        "delay_ret_fspec", [closure_calls](std::string) { ++(*closure_calls); }, invalid_cmd));

    EXPECT_TRUE(flip.get_delay_flip< std::string >(
        "delay_ret_fspec",
        [closure_calls](std::string error) {
            ++(*closure_calls);
            EXPECT_EQ(error, "Delayed error simulated value");
        },
        valid_cmd));

    EXPECT_FALSE(flip.get_delay_flip< std::string >(
        "delay_ret_fspec", [closure_calls](std::string) { ++(*closure_calls); }, invalid_cmd));

    EXPECT_FALSE(flip.get_delay_flip< std::string >(
        "delay_ret_fspec", [closure_calls](std::string) { ++(*closure_calls); }, valid_cmd));

    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(closure_calls->load(), 2);
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_flip");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%n] [%t] %v");
    return RUN_ALL_TESTS();
}