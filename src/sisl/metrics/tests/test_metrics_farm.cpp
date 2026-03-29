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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#include <gtest/gtest.h>
#include <sisl/logging/logging.h>

#include <sisl/metrics/metrics.h>

constexpr size_t ITERATIONS{3};

using namespace sisl;

class Group1Metrics : public MetricsGroup {
public:
    Group1Metrics() : MetricsGroup("Group1", "Instance1") {
        REGISTER_COUNTER(counter1, "Counter1");
        REGISTER_COUNTER(counter2, "Counter2");
        REGISTER_COUNTER(counter3, "Counter3");
        register_me_to_farm();
    }
};

class Group2Metrics : public MetricsGroup {
public:
    Group2Metrics() : MetricsGroup("Group2", "Instance1") {
        REGISTER_GAUGE(gauge1, "Gauge1");
        REGISTER_GAUGE(gauge2, "Gauge2");
        register_me_to_farm();
    }
};

// Worker threads own their MetricsGroup — when the thread exits, the group is destroyed and deregistered from the farm.
// This means iteration 2 (after both threads exit) sees an empty farm → empty JSON.
void userA() {
    Group1Metrics mgroup;
    COUNTER_INCREMENT(mgroup, counter1, 1);
    COUNTER_INCREMENT(mgroup, counter3, 4);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    COUNTER_INCREMENT(mgroup, counter2, 1);
    std::this_thread::sleep_for(std::chrono::seconds(4));
}

void userB() {
    Group2Metrics mgroup;
    GAUGE_UPDATE(mgroup, gauge1, 5);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    GAUGE_UPDATE(mgroup, gauge2, 2);
    GAUGE_UPDATE(mgroup, gauge1, 3);
    std::this_thread::sleep_for(std::chrono::seconds(4));
}

// clang-format off
nlohmann::json expected[ITERATIONS] = {
        {
            {"Group1", {
                {"Instance1", {
                    {"Counters", {
                        {"Counter1", 1},
                        {"Counter2", 0},
                        {"Counter3", 4}
                    }},
                    {"Gauges", {}},
                    {"Histograms percentiles avg/p50/p95/p99", {}}
                }},
            }},
            {"Group2", {
               {"Instance1", {
                    {"Counters", {}},
                    {"Gauges", {
                        {"Gauge1", 5},
                        {"Gauge2", 0}
                    }},
                    {"Histograms percentiles avg/p50/p95/p99", {}}
                }}
            }}
        },
        {
            {"Group1", {
                {"Instance1", {
                    {"Counters", {
                        {"Counter1", 1},
                        {"Counter2", 1},
                        {"Counter3", 4}
                    }},
                    {"Gauges", {}},
                    {"Histograms percentiles avg/p50/p95/p99", {}}
                }}
            }},
            {"Group2", {
                {"Instance1", {
                    {"Counters", {}},
                    {"Gauges", {
                       {"Gauge1", 3},
                       {"Gauge2", 2}
                    }},
                    {"Histograms percentiles avg/p50/p95/p99", {}}
                }}
            }},
        },
        {
        },
};
// clang-format on

std::array< uint64_t, ITERATIONS > delay{2, 3, 4};

void gather() {
    for (size_t i{0}; i < ITERATIONS; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(delay[i]));
        auto output = MetricsFarm::getInstance().get_result_in_json();

        nlohmann::json patch = nlohmann::json::diff(output, expected[i]);
        EXPECT_EQ(patch.empty(), true);
        if (!patch.empty()) {
            std::cerr << "On Iteration " << i << "\n";
            std::cerr << "Actual     " << std::setw(4) << output << "\n";
            std::cerr << "Expected   " << std::setw(4) << expected[i] << "\n";
            std::cerr << "Diff patch " << std::setw(4) << patch << "\n";
        }
    }
}

TEST(FarmTest, Gather) {
    std::thread th1(userA);
    std::thread th2(userB);
    std::thread th3(gather);

    th1.join();
    th2.join();
    th3.join();
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}