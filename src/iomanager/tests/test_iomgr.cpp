#include <atomic>
#include "common/async.h"
#include <chrono>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>
#include <folly/synchronization/Baton.h>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <iomanager/iomanager.h>

using namespace iomanager;
using namespace std::chrono_literals;

SISL_OPTION_GROUP(test_iomgr,
                  (num_reactors, "", "num_reactors", "number of reactor threads",
                   ::cxxopts::value< uint32_t >()->default_value("4"), "number"),
                  (num_iters, "", "num_iters", "iterations for timer/msg tests",
                   ::cxxopts::value< uint32_t >()->default_value("5"), "number"))

static uint32_t g_num_reactors{4};
static uint32_t g_num_iters{5};

// All tests share one iomgr instance (created in main).
class IOMgrTest : public ::testing::Test {};

// ── Basic dispatch ─────────────────────────────────────────────────────────────

TEST_F(IOMgrTest, NumReactors) {
    EXPECT_EQ(iomgr().num_reactors(), g_num_reactors);
}

TEST_F(IOMgrTest, SpawnAndBlockRunsOnCorrectReactor) {
    for (size_t i = 0; i < iomgr().num_reactors(); ++i) {
        size_t observed = iomgr().spawn_and_block(
            ReactorTarget::reactor(i), [i]() -> Async< size_t > { co_return iomgr().current_reactor_id(); }());
        EXPECT_EQ(observed, i) << "reactor " << i << " reported wrong id";
    }
}

TEST_F(IOMgrTest, CurrentReactorIdIsMaxOnNonReactorThread) {
    EXPECT_EQ(iomgr().current_reactor_id(), std::numeric_limits< size_t >::max());
}

TEST_F(IOMgrTest, SpawnDetachedFiresAndForgets) {
    folly::Baton<> done;
    iomgr().spawn_detached(ReactorTarget::any(), [&done]() -> Async< void > {
        done.post();
        co_return;
    });
    done.wait();
}

// ── Timer / sleep ──────────────────────────────────────────────────────────────
//
// Old: iomanager.schedule_thread_timer(ns, false, arg, cb)
// New: co_await iomgr().sleep(ms)  — same semantics, coroutine-native

TEST_F(IOMgrTest, SingleShotSleepAccuracy) {
    // Sleep 50 ms on each reactor and verify elapsed time is in range.
    for (size_t i = 0; i < iomgr().num_reactors(); ++i) {
        auto start = std::chrono::steady_clock::now();
        iomgr().spawn_and_block(ReactorTarget::reactor(i), iomgr().sleep(50ms));
        auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_GE(elapsed, 40ms) << "sleep fired too early on reactor " << i;
        EXPECT_LT(elapsed, 500ms) << "sleep took too long on reactor " << i;
    }
}

// Old: schedule_global_timer(ns, recurring=true, ..., all_worker, cb, wait)
// New: spawn_waitable_all with a sleep loop inside each reactor's coroutine

TEST_F(IOMgrTest, RecurringTimerRunsNTimesOnEachReactor) {
    // Each reactor runs a sleep loop g_num_iters times.
    // We collect the per-reactor counts and verify they all hit g_num_iters.
    auto counts = iomgr().spawn_and_block(ReactorTarget::reactor(0),
                                          iomgr().spawn_waitable_all_seq([](size_t /*reactor_id*/) -> Async< uint32_t > {
                                              uint32_t count = 0;
                                              for (uint32_t i = 0; i < g_num_iters; ++i) {
                                                  co_await iomgr().sleep(5ms);
                                                  ++count;
                                              }
                                              co_return count;
                                          }));

    ASSERT_EQ(counts.size(), iomgr().num_reactors());
    for (size_t i = 0; i < counts.size(); ++i) {
        EXPECT_EQ(counts[i], g_num_iters) << "reactor " << i << " missed iterations";
    }
}

// Old: cancel_timer(hdl, wait)
// New: atomic stop flag checked inside the coroutine loop — no handle needed.

TEST_F(IOMgrTest, CancellableRecurringTimer) {
    std::atomic< bool > stop{false};
    std::atomic< uint32_t > fire_count{0};
    folly::Baton<> done;

    iomgr().spawn_detached(ReactorTarget::reactor(0), [&stop, &fire_count, &done]() -> Async< void > {
        while (!stop.load(std::memory_order_acquire)) {
            co_await iomgr().sleep(10ms);
            ++fire_count;
        }
        done.post();
    });

    // Let a few ticks fire, then cancel.
    std::this_thread::sleep_for(55ms);
    stop.store(true, std::memory_order_release);
    done.wait();

    // Expect roughly 4–6 ticks in ~55 ms with 10 ms intervals.
    EXPECT_GE(fire_count.load(), 3u);
    EXPECT_LE(fire_count.load(), 8u);
}

// ── Yield ─────────────────────────────────────────────────────────────────────
//
// Old: implicit fiber yield inside a tight loop
// New: co_await iomgr().yield_now()  (= co_reschedule_on_current_executor)
//
// Test: two tasks on the same reactor. Task A yields in a loop; task B runs
// between A's iterations.  If A never yielded, B would only run after A
// completes, so b_ran would still be 0 when A checks it.

TEST_F(IOMgrTest, YieldAllowsOtherTaskToRun) {
    std::atomic< int > b_ran{0};
    folly::Baton<> done;

    // Task A: yield 10 times, then check that B already ran.
    iomgr().spawn_detached(ReactorTarget::reactor(0), [&b_ran, &done]() -> Async< void > {
        for (int i = 0; i < 10; ++i) {
            co_await iomgr().yield_now();
        }
        EXPECT_GT(b_ran.load(), 0) << "B never ran during A's yields";
        done.post();
    });

    // Task B: increment counter and return immediately.
    iomgr().spawn_detached(ReactorTarget::reactor(0), [&b_ran]() -> Async< void > {
        b_ran.fetch_add(1);
        co_return;
    });

    done.wait();
}

// ── Messaging (dispatch between reactors) ─────────────────────────────────────
//
// Old: iomanager.run_on_wait(all_worker, fn)   — sync broadcast
// New: spawn_and_block(reactor(0), spawn_waitable_all_seq(fn))

TEST_F(IOMgrTest, SyncBroadcastToAllReactors) {
    std::atomic< uint32_t > rcvd{0};

    // Sequentially visits every reactor and increments the counter.
    iomgr().spawn_and_block(ReactorTarget::reactor(0),
                            iomgr().spawn_waitable_all_seq([&rcvd](size_t /*reactor_id*/) -> Async< void > {
                                rcvd.fetch_add(1, std::memory_order_relaxed);
                                co_return;
                            }));

    EXPECT_EQ(rcvd.load(), iomgr().num_reactors());
}

// Old: iomanager.run_on_forget(all_io, fn)   — async broadcast
// New: loop spawn_detached to every reactor, then wait with a latch.

TEST_F(IOMgrTest, AsyncBroadcastToAllReactors) {
    const size_t N = iomgr().num_reactors();
    std::atomic< size_t > rcvd{0};
    folly::Baton<> all_done;

    for (size_t i = 0; i < N; ++i) {
        iomgr().spawn_detached(ReactorTarget::reactor(i), [&rcvd, &all_done, N]() -> Async< void > {
            if (rcvd.fetch_add(1, std::memory_order_acq_rel) + 1 == N) {
                all_done.post();
            }
            co_return;
        });
    }

    all_done.wait();
    EXPECT_EQ(rcvd.load(), N);
}

// Old: iomanager.run_on_wait(all_io, fn) with messages from client threads
// New: each "client" calls spawn_and_block; the reactor-side task increments rcvd.

TEST_F(IOMgrTest, SyncUnicastFromMultipleCallers) {
    std::atomic< uint64_t > sent{0};
    std::atomic< uint64_t > rcvd{0};

    auto sink_task = [&rcvd]() -> Async< void > {
        rcvd.fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    const uint32_t nclients = 4;
    const uint32_t iters = g_num_iters;
    std::vector< std::thread > clients;
    clients.reserve(nclients);

    for (uint32_t c = 0; c < nclients; ++c) {
        clients.emplace_back([&sent, &rcvd, &sink_task, iters]() {
            for (uint32_t i = 0; i < iters; ++i) {
                // Pick a reactor round-robin across the client threads.
                size_t rid = iomgr().next_reactor();
                iomgr().spawn_and_block(ReactorTarget::reactor(rid), sink_task());
                sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : clients)
        t.join();
    EXPECT_EQ(sent.load(), rcvd.load());
}

// Old: async relay — send to one reactor which re-broadcasts to all_io
// New: spawn_waitable on any reactor; inside that coroutine, spawn_detached to all.

TEST_F(IOMgrTest, AsyncRelayBroadcast) {
    const size_t N = iomgr().num_reactors();
    std::atomic< size_t > rcvd{0};
    folly::Baton<> all_done;

    iomgr().spawn_and_block(ReactorTarget::any(), [N, &rcvd, &all_done]() -> Async< void > {
        // From inside a reactor coroutine, fan-out to all reactors.
        for (size_t i = 0; i < N; ++i) {
            iomgr().spawn_detached(ReactorTarget::reactor(i), [&rcvd, &all_done, N]() -> Async< void > {
                if (rcvd.fetch_add(1, std::memory_order_acq_rel) + 1 == N) {
                    all_done.post();
                }
                co_return;
            });
        }
        co_return;
    }());

    all_done.wait();
    EXPECT_EQ(rcvd.load(), N);
}

// ── spawn_waitable_all ─────────────────────────────────────────────────────────

TEST_F(IOMgrTest, SpawnWaitableAllVisitsEveryReactor) {
    auto results = iomgr().spawn_and_block(
        ReactorTarget::reactor(0),
        iomgr().spawn_waitable_all_seq([](size_t /*i*/) -> Async< size_t > { co_return iomgr().current_reactor_id(); }));

    ASSERT_EQ(results.size(), iomgr().num_reactors());
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i], i);
    }
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    g_num_reactors = SISL_OPTIONS["num_reactors"].as< uint32_t >();
    g_num_iters = SISL_OPTIONS["num_iters"].as< uint32_t >();
    sisl::logging::SetLogger("test_iomgr");
    ::testing::InitGoogleTest(&argc, argv);
    init_iomgr(g_num_reactors);
    int rc = RUN_ALL_TESTS();
    stop_iomgr();
    return rc;
}
