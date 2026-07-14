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
#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include "common/async.h"
#include <folly/synchronization/Baton.h>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <iomanager/iomanager.h>
#include <iomanager/coro_timer.h>

using namespace iomanager;
using namespace std::chrono_literals;

SISL_OPTION_GROUP(test_coro_timer,
                  (num_reactors, "", "num_reactors", "number of reactor threads",
                   ::cxxopts::value< uint32_t >()->default_value("4"), "number"))

static uint32_t g_num_reactors{4};

class CoroTimerTest : public ::testing::Test {};

// ── Recurring ─────────────────────────────────────────────────────────────────

TEST_F(CoroTimerTest, RecurringFiresMultipleTimes) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    timer.start(ReactorTarget::any(), 10ms, TimerKind::Recurring, [&tick_count]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    std::this_thread::sleep_for(55ms);
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());

    // Expect ~5 ticks at 10 ms intervals over 55 ms; tolerate scheduler jitter.
    auto count = tick_count.load();
    EXPECT_GE(count, 3u) << "too few ticks fired";
    EXPECT_LE(count, 8u) << "too many ticks fired";
}

TEST_F(CoroTimerTest, RecurringStopsPromptly) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    timer.start(ReactorTarget::any(), 5ms, TimerKind::Recurring, [&tick_count]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    std::this_thread::sleep_for(20ms);
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    const auto count_at_stop = tick_count.load();
    std::this_thread::sleep_for(30ms);
    const auto after_stop = tick_count.load();
    EXPECT_EQ(count_at_stop, after_stop) << "ticks fired after stop() returned";
}

// ── One-shot ──────────────────────────────────────────────────────────────────

TEST_F(CoroTimerTest, OneShotFiresExactlyOnce) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};
    folly::Baton<> tick_done;

    timer.start(ReactorTarget::any(), 10ms, TimerKind::OneShot, [&tick_count, &tick_done]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        tick_done.post();
        co_return;
    });

    tick_done.wait();
    std::this_thread::sleep_for(50ms); // Plenty of time for any spurious additional ticks.
    EXPECT_EQ(tick_count.load(), 1u) << "OneShot fired more than once";

    // stop() after natural completion is a no-op.
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    EXPECT_FALSE(timer.is_started());
}

// Hot-path pattern: OneShot tick re-arms the timer from inside its own tick body.
TEST_F(CoroTimerTest, OneShotRearmsFromInsideTick) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};
    folly::Baton<> all_done;
    constexpr uint32_t target = 50;

    std::function< void() > arm = [&]() {
        timer.start(ReactorTarget::any(), 1ms, TimerKind::OneShot,
                    [&tick_count, &arm, &all_done, target]() -> Async< void > {
                        auto n = tick_count.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (n < target) {
                            arm(); // re-arm from INSIDE tick — exercises the auto-reset hot path
                        } else {
                            all_done.post();
                        }
                        co_return;
                    });
    };
    arm();
    all_done.wait();
    EXPECT_EQ(tick_count.load(), target);

    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    EXPECT_FALSE(timer.is_started());
}

TEST_F(CoroTimerTest, OneShotCancelledBeforeTick) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    timer.start(ReactorTarget::any(), 200ms, TimerKind::OneShot, [&tick_count]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    // Cancel well before the 200ms interval elapses.
    std::this_thread::sleep_for(20ms);
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    std::this_thread::sleep_for(250ms);
    EXPECT_EQ(tick_count.load(), 0u) << "tick fired despite early stop";
}

// ── Restart / re-use ──────────────────────────────────────────────────────────

TEST_F(CoroTimerTest, RestartAfterStop) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    auto run_burst = [&]() {
        timer.start(ReactorTarget::any(), 10ms, TimerKind::Recurring, [&tick_count]() -> Async< void > {
            tick_count.fetch_add(1, std::memory_order_relaxed);
            co_return;
        });
        std::this_thread::sleep_for(35ms);
        iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    };

    run_burst();
    const auto first = tick_count.load();
    EXPECT_GE(first, 2u);

    run_burst();
    const auto second = tick_count.load();
    EXPECT_GT(second, first) << "second start did not fire any ticks";
}

TEST_F(CoroTimerTest, RestartAfterOneShotNaturalCompletion) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    folly::Baton<> tick_a;
    timer.start(ReactorTarget::any(), 5ms, TimerKind::OneShot, [&tick_count, &tick_a]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        tick_a.post();
        co_return;
    });
    tick_a.wait();
    EXPECT_EQ(tick_count.load(), 1u);

    // Re-start without explicit stop — auto-reset path.
    folly::Baton<> tick_b;
    timer.start(ReactorTarget::any(), 5ms, TimerKind::OneShot, [&tick_count, &tick_b]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        tick_b.post();
        co_return;
    });
    tick_b.wait();
    EXPECT_EQ(tick_count.load(), 2u);

    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
}

// ── request_stop + stop split ─────────────────────────────────────────────────

TEST_F(CoroTimerTest, RequestStopThenStopPattern) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    timer.start(ReactorTarget::any(), 10ms, TimerKind::Recurring, [&tick_count]() -> Async< void > {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    std::this_thread::sleep_for(25ms);
    timer.request_stop(); // non-blocking; cancellation requested
    // Caller can do other work here while the timer drains.
    std::this_thread::sleep_for(20ms);
    const auto count_at_drain = tick_count.load();
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(tick_count.load(), count_at_drain) << "ticks fired after request_stop";
    EXPECT_FALSE(timer.is_started());
}

TEST_F(CoroTimerTest, RequestStopOnNotStartedIsNoOp) {
    CoroTimer timer;
    timer.request_stop(); // must not crash
    EXPECT_FALSE(timer.is_started());
}

TEST_F(CoroTimerTest, StopOnNotStartedIsNoOp) {
    CoroTimer timer;
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    EXPECT_FALSE(timer.is_started());
}

// ── State queries ─────────────────────────────────────────────────────────────

TEST_F(CoroTimerTest, IsStartedReflectsLifecycle) {
    CoroTimer timer;
    EXPECT_FALSE(timer.is_started());

    timer.start(ReactorTarget::any(), 100ms, TimerKind::Recurring, []() -> Async< void > { co_return; });
    EXPECT_TRUE(timer.is_started());

    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());
    EXPECT_FALSE(timer.is_started());
}

// ── Tick callback with awaits ─────────────────────────────────────────────────

TEST_F(CoroTimerTest, TickCanAwait) {
    CoroTimer timer;
    std::atomic< uint32_t > tick_count{0};

    timer.start(ReactorTarget::any(), 10ms, TimerKind::Recurring, [&tick_count]() -> Async< void > {
        co_await iomgr().sleep(2ms);
        tick_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(60ms);
    iomgr().spawn_and_block(ReactorTarget::reactor(0), timer.stop());

    auto count = tick_count.load();
    EXPECT_GE(count, 2u);
}

// ── Concurrent timers ─────────────────────────────────────────────────────────

TEST_F(CoroTimerTest, MultipleTimersConcurrent) {
    constexpr int N = 4;
    std::array< CoroTimer, N > timers;
    std::array< std::atomic< uint32_t >, N > counts{};

    for (int i = 0; i < N; ++i) {
        timers[i].start(ReactorTarget::any(), 10ms, TimerKind::Recurring, [&counts, i]() -> Async< void > {
            counts[i].fetch_add(1, std::memory_order_relaxed);
            co_return;
        });
    }

    std::this_thread::sleep_for(50ms);
    for (int i = 0; i < N; ++i) {
        iomgr().spawn_and_block(ReactorTarget::reactor(0), timers[i].stop());
    }

    for (int i = 0; i < N; ++i) {
        EXPECT_GE(counts[i].load(), 2u) << "timer " << i << " under-fired";
    }
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    g_num_reactors = SISL_OPTIONS["num_reactors"].as< uint32_t >();
    sisl::logging::SetLogger("test_coro_timer");
    ::testing::InitGoogleTest(&argc, argv);
    init_iomgr(g_num_reactors);
    int rc = RUN_ALL_TESTS();
    stop_iomgr();
    return rc;
}
