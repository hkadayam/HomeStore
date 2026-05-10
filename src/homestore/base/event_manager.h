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
// handler list — there is no central event registry, no string keys, no broker thread, and no queue.  publish() runs
// subscribed handlers synchronously inline; handlers are expected to be cheap and to spawn_detached any heavy
// follow-up work themselves.
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
        std::lock_guard< std::mutex > lk(mtx());
        for (auto const& h : storage< EventT >()) {
            h(ev);
        }
    }

    /// Subscribe a handler for EventT.  Subscribers are not deduped — caller owns idempotency.
    template < typename EventT >
    static void subscribe(Handler< EventT > h) {
        register_clearer< EventT >();
        std::lock_guard< std::mutex > lk(mtx());
        storage< EventT >().push_back(std::move(h));
    }

    /// Clear all subscribed handlers across every EventT seen this process.  Used at teardown so a re-init starts
    /// with no stale handlers pointing at destroyed objects.
    static void reset();

private:
    template < typename EventT >
    static std::vector< Handler< EventT > >& storage() {
        static std::vector< Handler< EventT > > s;
        return s;
    }

    /// Once per EventT (per process), register a function that clears storage<EventT>().  reset() walks all
    /// registered clearers.  The static-local `once` flag ensures the registration runs exactly once per type
    /// regardless of how many times subscribe<T>() is called.  We register before taking mtx() to avoid recursive
    /// locking.
    template < typename EventT >
    static void register_clearer() {
        static bool once = [] {
            clearers().push_back(+[]() { storage< EventT >().clear(); });
            return true;
        }();
        (void)once;
    }

    static std::mutex& mtx();
    static std::vector< void (*)() >& clearers();
};

inline std::mutex& EventManager::mtx() {
    static std::mutex m;
    return m;
}

inline std::vector< void (*)() >& EventManager::clearers() {
    static std::vector< void (*)() > v;
    return v;
}

inline void EventManager::reset() {
    std::lock_guard< std::mutex > lk(mtx());
    for (auto fn : clearers()) {
        fn();
    }
}

} // namespace homestore
