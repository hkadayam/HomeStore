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
#pragma once

#include <chrono>
#include <utility>

#include "common/async.h"
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include "sisl/logging/logging.h" // RELEASE_ASSERT

#include "iomanager.h" // ReactorTarget, spawn_detached, iomgr()

namespace iomanager {

// ─────────────────────────────────────────────────────────────────────────────
// CoroTimer
//
// Reusable timer built on folly::AsyncTimeout (per-EventBase libevent timer) and a coro Baton.
// Avoids folly::coro::sleep + cancellation tokens, which routes through the global
// HeapTimekeeper + DistributedMutex and produces tcache corruption on reactor shutdown when
// cancelled mid-flight.
//
//   • start(target, interval, kind, tick) — pins a detached coroutine to the chosen reactor's
//     EventBase, schedules an AsyncTimeout for `interval` microseconds, awaits a wakeup baton,
//     then co_awaits tick().  Recurring loops; OneShot exits after a single tick.
//   • request_stop() — non-blocking; hops to the EB and posts the wakeup baton with a stop flag.
//   • stop()         — coroutine; request_stop + co_await timer coroutine exit.  Idempotent.
//
// Re-use:  for a OneShot that completed on its own (no stop call), the next start() detects the
// done-baton state and resets internally — no public reset() method.
//
// Lifecycle: the spawned coroutine captures `this`.  The owner MUST call stop() (for an active
// timer) before destruction; the dtor does NOT auto-stop, since blocking on a Baton in a
// destructor across coroutine boundaries is fragile.
//
// Threading: scheduleTimeout/cancelTimeout require the EventBase thread.  The timer coroutine
// is pinned to that EB, so all timer ops happen there.  request_stop() dispatches its state
// change onto the EB via runInEventBaseThread (fire-and-forget); stop() instead hops onto the
// EB synchronously via scheduleOn so the caller can be sure no queued lambda outlives `this`.
// ─────────────────────────────────────────────────────────────────────────────
enum class TimerKind : uint8_t {
    OneShot,
    Recurring,
};

class CoroTimer {
public:
    CoroTimer() = default;
    ~CoroTimer() = default;
    CoroTimer(const CoroTimer&) = delete;
    CoroTimer& operator=(const CoroTimer&) = delete;
    CoroTimer(CoroTimer&&) = delete;
    CoroTimer& operator=(CoroTimer&&) = delete;

    template < typename TickFn >
    void start(ReactorTarget target, std::chrono::microseconds interval, TimerKind kind, TickFn tick) {
        if (started_ && done_baton_.ready()) {
            // A previous OneShot finished on its own; clear the started_ + baton state so this start() can re-use
            // the timer.
            done_baton_.reset();
            started_ = false;
        }
        RELEASE_ASSERT(!started_, "CoroTimer::start while already running");

        eb_ = iomgr().resolve_target(target);
        stop_requested_ = false;
        wakeup_baton_.reset();
        started_ = true;

        // Build the timer task and start it on `eb_` directly.  We MUST NOT re-resolve `target` (it may be
        // ReactorTarget::any(), which advances the round-robin and would land us on a different reactor than `eb_`).
        // AsyncTimeout::scheduleTimeout/cancelTimeout require being called on the EB they're bound to.
        auto eb = eb_;
        auto factory = [this, eb, interval, kind, tick = std::move(tick)]() mutable -> Async< void > {
            auto to = folly::AsyncTimeout::make(*eb, [baton = &wakeup_baton_]() noexcept { baton->post(); });
            do {
                to->scheduleTimeoutHighRes(interval);
                co_await wakeup_baton_;
                wakeup_baton_.reset();
                if (stop_requested_) {
                    to->cancelTimeout();
                    break;
                }
                if (kind == TimerKind::OneShot) {
                    // Post done BEFORE running tick so the user may safely call start() again on this same
                    // CoroTimer from inside (or right after) tick — the auto-reset path in start() needs
                    // done_baton_.ready() to be true, otherwise its RELEASE_ASSERT(!started_) fires.
                    done_baton_.post();
                    co_await tick();
                    co_return;
                }
                co_await tick();
            } while (kind == TimerKind::Recurring);

            // Recurring + stop_requested path.
            done_baton_.post();
        };
        auto task = folly::coro::co_invoke(std::move(factory));
        eb->runInEventBaseThread(
            [eb, task = std::move(task)]() mutable { std::move(task).scheduleOn(eb).startInlineUnsafe([](auto) {}); });
    }

    /// Non-blocking: hops to the timer's EB to set stop_requested_ and post the wakeup baton.  Caller MUST still
    /// call stop() afterward to drain the done baton, reset state, and (critically) ensure the queued EB lambda has
    /// completed before the CoroTimer is destroyed — otherwise the lambda runs against a freed `this`.  Idempotent.
    void request_stop() {
        if (!started_ || !eb_) {
            return;
        }
        eb_->runInEventBaseThread([this] {
            if (started_ && !stop_requested_) {
                stop_requested_ = true;
                wakeup_baton_.post();
            }
        });
    }

    /// Coroutine: drains the timer to completion and resets state.  Idempotent.  Hops onto the timer's EB to issue
    /// the cancel synchronously, which both serialises against the timer coroutine's own EB-thread state and
    /// guarantees no fire-and-forget lambda outlives `this` after stop() returns.
    Async< void > stop() {
        if (!started_) {
            co_return;
        }
        if (done_baton_.ready()) {
            // Timer already finished on its own (natural OneShot completion) — nothing to cancel.
            done_baton_.reset();
            started_ = false;
            eb_ = nullptr;
            co_return;
        }
        auto* eb = eb_;
        // Hop to eb_'s thread, set stop flag + post wakeup baton synchronously.  When this co_await resolves, the
        // EB lambda has completed, so there is no queued reference to `this` left in the EB queue.
        co_await folly::coro::co_invoke([this]() -> Async< void > {
            if (started_ && !stop_requested_) {
                stop_requested_ = true;
                wakeup_baton_.post();
            }
            co_return;
        }).scheduleOn(eb);
        co_await done_baton_;
        done_baton_.reset();
        started_ = false;
        eb_ = nullptr;
    }

    bool is_started() const { return started_; }

private:
    folly::EventBase* eb_{nullptr};
    folly::coro::Baton wakeup_baton_;
    folly::coro::Baton done_baton_;
    bool stop_requested_{false};
    bool started_{false};
};

} // namespace iomanager
