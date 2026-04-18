#pragma once

#ifdef BTREE_ASYNC_MODE

#include <folly/coro/Mutex.h>
#include <folly/coro/SharedMutex.h>
#include <folly/coro/Task.h>

namespace homestore {
template < typename T >
using BtreeTask = folly::coro::Task< T >;
#define CO_AWAIT co_await
#define CO_RETURN co_return
using BtreeMutex = folly::coro::Mutex;
using BtreeSharedMutex = folly::coro::SharedMutex;

// Wrappers used by btree code that must compile in either sync or async mode.  In async mode they return awaitables
// (co_await locks); in sync mode they acquire immediately and return void.  Call sites uniformly do
// `CO_AWAIT lock_shared_async(m)` — expanding to `co_await m.co_lock_shared()` in async mode, and to a plain
// `m.lock_shared()` in sync mode.
inline auto lock_async(BtreeSharedMutex& m) { return m.co_lock(); }
inline auto lock_shared_async(BtreeSharedMutex& m) { return m.co_lock_shared(); }
inline auto lock_async(BtreeMutex& m) { return m.co_lock(); }
} // namespace homestore

#else

#include <mutex>
#include <shared_mutex>

namespace homestore {
template < typename T >
using BtreeTask = T;
#define CO_AWAIT
#define CO_RETURN return
using BtreeMutex = std::mutex;
using BtreeSharedMutex = folly::SharedMutex;

// Sync-mode equivalents of the async wrappers.  `CO_AWAIT` is empty in sync mode, so `CO_AWAIT lock_shared_async(m)`
// collapses to just `lock_shared_async(m);` — which acquires the lock synchronously.
inline void lock_async(BtreeSharedMutex& m) { m.lock(); }
inline void lock_shared_async(BtreeSharedMutex& m) { m.lock_shared(); }
inline void lock_async(BtreeMutex& m) { m.lock(); }
} // namespace homestore

#endif
