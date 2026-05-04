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
 ***************************************************************************/
#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// EventManager
//
// Generic, type-safe pub/sub for cross-module signalling.  Each event type (a plain struct) gets its own static
// handler list — there is no central event registry, no string keys, no broker thread, and no queue.
// publish() runs subscribed handlers synchronously inline; handlers are expected to be cheap and to spawn_detached
// any heavy follow-up work themselves.
//
// Designed for low-frequency, asymmetric signals (e.g. ResourceEvent: disk full, alloc failed).  Hot-path metrics
// like dirty-node counts must NOT go through here — those are pulled by ResourceManager's poll loop.
//
// Adding a new event type costs nothing here: define a struct with the payload fields, then call
// EventManager::publish<MyEvent>(...) / EventManager::subscribe<MyEvent>(...).  No edits to this file are needed.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class EventManager {
public:
    template < typename EventT >
    using Handler = std::function< void(EventT const&) >;

    /// Publish synchronously.  All handlers subscribed for EventT run inline before publish() returns.
    template < typename EventT >
    static void publish(EventT const& ev) {
        std::lock_guard lk(mtx());
        for (auto const& h : handlers< EventT >()) {
            h(ev);
        }
    }

    /// Subscribe a handler for EventT.  Subscribers are not deduped; the caller owns idempotency.
    template < typename EventT >
    static void subscribe(Handler< EventT > h) {
        std::lock_guard lk(mtx());
        handlers< EventT >().push_back(std::move(h));
    }

    /// Drop all handlers across all event types.  Called from teardown so a fresh process iteration starts clean.
    static void reset();

private:
    template < typename EventT >
    static std::vector< Handler< EventT > >& handlers() {
        static std::vector< Handler< EventT > > s_handlers;
        s_reset_fns().push_back(+[]() { handlers< EventT >().clear(); });
        return s_handlers;
    }

    static std::mutex& mtx() {
        static std::mutex m;
        return m;
    }

    /// Per-instantiation reset shims, registered the first time handlers<T>() is called.  reset() walks them all so
    /// the static handler vectors of every EventT seen this run get cleared together.
    static std::vector< void (*)() >& s_reset_fns();
};

inline std::vector< void (*)() >& EventManager::s_reset_fns() {
    static std::vector< void (*)() > v;
    return v;
}

inline void EventManager::reset() {
    std::lock_guard lk(mtx());
    for (auto fn : s_reset_fns()) {
        fn();
    }
}

} // namespace homestore
