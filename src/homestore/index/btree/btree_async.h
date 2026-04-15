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
} // namespace homestore

#endif
