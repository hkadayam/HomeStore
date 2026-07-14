/*********************************************************************************
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
#pragma once

// ── Coroutine facade ─────────────────────────────────────────────────────────
//
// The single dependency point on folly's coroutine library.  Any file doing asynchronous work includes this
// header instead of naming folly directly — it re-exports the coroutine primitives the codebase uses (Task,
// blockingWait, Mutex, SharedMutex, Baton, sleep, co_invoke, collect, via helpers), so the runtime can be
// swapped in one place and call sites stay decoupled from the underlying library.

#include <folly/coro/Task.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/SharedMutex.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Collect.h>
#include <folly/coro/ViaIfAsync.h>

// Async<T> is the return type of any function that completes asynchronously — a lazily-started coroutine the
// caller must co_await.  It aliases folly's coroutine task so call sites read as an intent ("returns an async T").
template < typename T >
using Async = folly::coro::Task< T >;
