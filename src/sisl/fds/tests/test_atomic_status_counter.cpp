/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Author/Developer(s): Bryan Zimmerman
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
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include <gtest/gtest.h>

#include <sisl/fds/atomic_status_counter.h>

using namespace sisl;

namespace {
class AtomicStatusCounterTest : public testing::Test {
public:
    AtomicStatusCounterTest() : testing::Test(){};
    AtomicStatusCounterTest(const AtomicStatusCounterTest&) = delete;
    AtomicStatusCounterTest(AtomicStatusCounterTest&&) noexcept = delete;
    AtomicStatusCounterTest& operator=(const AtomicStatusCounterTest&) = delete;
    AtomicStatusCounterTest& operator=(AtomicStatusCounterTest&&) noexcept = delete;
    virtual ~AtomicStatusCounterTest() override = default;

protected:
    void SetUp() override {}
    void TearDown() override {}
};
} // namespace

TEST_F(AtomicStatusCounterTest, BasicStatusCounter) {
    StatusCounter< uint8_t, 0 > sc1{1};
    EXPECT_EQ(sc1.to_integer(), static_cast< uint64_t >(1));
    StatusCounter< uint8_t, 0 > sc2{1, 2};
    const uint8_t counter_size_bits{sizeof(typename decltype(sc2)::CounterType) * 8};
    EXPECT_EQ(sc2.to_integer(), static_cast< uint64_t >(1) | (static_cast< uint64_t >(2) << counter_size_bits));
}

TEST_F(AtomicStatusCounterTest, BasicTest) {
    AtomicStatusCounter< uint8_t, 0 > asc{1, 2};
    EXPECT_EQ(asc.count(), static_cast< typename decltype(asc)::CounterType >(1));
    EXPECT_EQ(asc.get_status(), static_cast< uint8_t >(2));
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    sisl::logging::SetLogger("test_atomic_status_counter");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%n] [%t] %v");

    const auto result{RUN_ALL_TESTS()};
    return result;
}