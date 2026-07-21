/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include "homestore/base/blk.h"
#include "common/defs.h"
#include "segment_manager.h"

namespace homestore {
namespace blkalloc {

///
/// SweepService — module-scoped background refill pool shared by all SlabBlkAllocator instances.
///
/// Why it exists: HomeStore can have up to 64K chunks, each with its own allocator. A per-allocator thread
/// is catastrophic at scale. Instead, one small pool (sized by blkallocator.num_slab_sweeper_threads) services
/// every allocator via portion-granular work items.
///
/// Priority tiers (folly::CPUThreadPoolExecutor numPriorities=2):
///   HIGH — synchronous request_refill_blocking() from an alloc-path miss; a thread is blocked waiting for blocks.
///   LOW  — proactive request_refill() from threshold-cross and periodic safety-net scans.
///
/// Portion enqueue is gated by InmemPortion::enqueued_ (atomic CAS) so a portion is never double-queued while
/// a sweep for it is pending.
///
/// Allocator lifetime:
///   Constructor → sweep_service().register_allocator(seg_mgr, refill_fn, name) returns a shared AllocatorHandle.
///   Destructor  → handle_.reset() — clears alive_, waits for in-flight workers on this allocator to drain.
///
class SweepService {
public:
    using RefillFn = std::function< void(InmemPortion&) >;

    /// Opaque handle returned by register_allocator(). Allocator holds it; drop to deregister.
    struct AllocatorHandle {
        SegmentManager* seg_mgr{nullptr};
        RefillFn refill_fn;
        std::atomic< bool > alive_{true};
        std::atomic< int32_t > in_flight_{0};
        std::string name_;

        AllocatorHandle(SegmentManager& sm, RefillFn fn, std::string n) :
                seg_mgr{&sm}, refill_fn{std::move(fn)}, name_{std::move(n)} {}

        AllocatorHandle(AllocatorHandle const&) = delete;
        AllocatorHandle(AllocatorHandle&&) = delete;
        AllocatorHandle& operator=(AllocatorHandle const&) = delete;
        AllocatorHandle& operator=(AllocatorHandle&&) = delete;

        /// Sets alive_=false and spin-waits until in_flight_ reaches 0. Called from allocator destructor.
        ~AllocatorHandle();
    };

    SweepService(uint32_t num_workers, std::chrono::milliseconds tick);
    SweepService(SweepService const&) = delete;
    SweepService(SweepService&&) = delete;
    SweepService& operator=(SweepService const&) = delete;
    SweepService& operator=(SweepService&&) = delete;
    ~SweepService();

    shared< AllocatorHandle > register_allocator(SegmentManager& seg_mgr, RefillFn refill, std::string name);

    /// Async low-priority refill request — fire-and-forget. No-op if portion is already enqueued.
    void request_refill(AllocatorHandle& h, InmemPortion& portion);

    /// Sync high-priority refill request. Blocks until the portion's slab cache reports
    /// total_cached_blks() >= wait_for_blks (or the service is shutting down).
    void request_refill_blocking(AllocatorHandle& h, InmemPortion& portion, blk_count_t wait_for_blks);

private:
    /// Periodic safety-net: scans all registered allocators, enqueues any portion below refill threshold.
    void ticker_loop();

    /// Enqueues a refill task at the given priority if portion's enqueued_ flag CAS-flips false→true.
    /// Returns true if the task was enqueued.
    bool try_enqueue(shared< AllocatorHandle > h, InmemPortion& portion, int8_t priority);

    /// Worker entry point run on sweep_pool_; calls handle.refill_fn(portion) under in_flight_ tracking
    /// and clears portion.enqueued_ on completion.
    void run_refill(shared< AllocatorHandle > h, InmemPortion* portion);

    std::chrono::milliseconds tick_;
    folly::CPUThreadPoolExecutor sweep_pool_;

    std::mutex registry_mtx_;
    std::vector< std::weak_ptr< AllocatorHandle > > registry_;

    std::thread ticker_;
    std::atomic< bool > stop_{false};
    std::mutex ticker_mtx_;
    std::condition_variable ticker_cv_;
};

// ---- Module-scoped service access ----

/// Initializes the sweep service using current HS_RUNTIME_CONFIG values:
///   blkallocator.num_slab_sweeper_threads
///   blkallocator.slab_refill_frequency_ms
/// Idempotent: calling twice is a no-op.
void init_sweep_service();

/// Tears down the service. Drains in-flight workers and joins all threads. After this point,
/// sweep_service() must not be called until init_sweep_service() is called again.
void shutdown_sweep_service();

/// Accessor. Asserts that init_sweep_service() was called.
SweepService& sweep_service();

} // namespace blkalloc
} // namespace homestore