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
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "sisl/fds/rcu.h"

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
    ///
    /// Lock-free read path (RCU): grab a shared_ptr snapshot of the current immutable handler list under a brief RCU
    /// read guard — the guard is ~free and the snapshot is a single refcount bump, no per-handler copy — then drop
    /// the guard and run handlers holding nothing.  Running handlers outside the RCU section is mandatory: a handler
    /// may re-enter subscribe(), whose grace-period wait (Rcu::synchronize) would deadlock if called while this
    /// thread still held a read guard.  publish is the frequent side; writers replace the list copy-on-write, so a
    /// concurrent subscribe()/reset() never mutates the vector this snapshot references.
    template < typename EventT >
    static void publish(EventT const& ev) {
        std::shared_ptr< const HandlerList< EventT > > handlers;
        {
            sisl::Rcu::read_guard guard;
            handlers = storage< EventT >().get_node()->get();
        }
        for (auto const& h : *handlers) {
            h(ev);
        }
    }

    /// Subscribe a handler for EventT.  Subscribers are not deduped — caller owns idempotency.  Copy-on-write under
    /// the writer mutex (serialises the read-copy-modify-swap against other subscribers): build a new list from the
    /// current one plus the new handler, then atomically publish it via make_and_exchange.
    template < typename EventT >
    static void subscribe(Handler< EventT > h) {
        std::lock_guard< std::mutex > lk(mtx());
        register_clearer< EventT >();
        HandlerList< EventT > next;
        {
            auto acc = storage< EventT >().get();
            next = *acc;
        }
        next.push_back(std::move(h));
        storage< EventT >().make_and_exchange(std::move(next));
    }

    /// Clear all subscribed handlers across every EventT seen this process.  Used at teardown so a re-init starts
    /// with no stale handlers pointing at destroyed objects.
    static void reset();

private:
    template < typename EventT >
    using HandlerList = std::vector< Handler< EventT > >;

    /// Per-EventT handler list, held in an RCU cell: readers (publish) take a near-free read guard; writers swap in
    /// a new list copy-on-write.  Default-constructs to a non-null empty list so publish never has to null-check.
    template < typename EventT >
    static sisl::Rcu::data< HandlerList< EventT > >& storage() {
        static sisl::Rcu::data< HandlerList< EventT > > s;
        return s;
    }

    /// Once per EventT (per process), register a function that clears storage<EventT>() (swaps in a fresh empty
    /// list).  reset() walks all registered clearers.  The static-local `once` flag ensures the registration runs
    /// exactly once per type regardless of how many times subscribe<T>() is called.  The sole caller (subscribe)
    /// already holds mtx(), so the one-time clearers() push is serialised across event types — take no lock here.
    template < typename EventT >
    static void register_clearer() {
        static bool once = [] {
            clearers().push_back(+[]() { storage< EventT >().make_and_exchange(); });
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
