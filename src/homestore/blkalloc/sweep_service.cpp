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
#include "sweep_service.h"

#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include "sisl/logging/logging.h"

#include "homestore/base/homestore_assert.h"
#include "homestore/base/hs_runtime_config.h"

namespace homestore {
namespace blkalloc {

static constexpr int8_t SWEEP_PRIO_LOW = 0;
static constexpr int8_t SWEEP_PRIO_HIGH = 1;
static constexpr int8_t SWEEP_NUM_PRIORITIES = 2;

SweepService::AllocatorHandle::~AllocatorHandle() {
    // seq_cst on both sides (here and in run_refill) is required for the Dekker-style protocol against worker
    // entry: either the worker's fetch_add on in_flight_ is visible before we load it (we spin), or our store
    // to alive_=false is visible before the worker loads it (worker backs out).  Release/acquire alone is not
    // enough — it admits a total-order interleaving where worker sees alive_=true and we see in_flight_=0.
    alive_.store(false, std::memory_order_seq_cst);
    while (in_flight_.load(std::memory_order_seq_cst) > 0) {
        std::this_thread::yield();
    }
}

SweepService::SweepService(uint32_t num_workers, std::chrono::milliseconds tick) :
        tick_{tick},
        sweep_pool_{num_workers, SWEEP_NUM_PRIORITIES,
                    std::make_shared< folly::NamedThreadFactory >("blkalloc_sweep")} {
    LOGINFOMOD(blkalloc, "SweepService starting: num_workers={} tick_ms={}", num_workers, tick.count());
    ticker_ = std::thread{[this]() { ticker_loop(); }};
}

SweepService::~SweepService() {
    stop_.store(true, std::memory_order_release);
    {
        std::lock_guard lk{ticker_mtx_};
        ticker_cv_.notify_all();
    }
    if (ticker_.joinable())
        ticker_.join();

    // join() drains queued tasks; stop() prevents new task acceptance and joins worker threads.
    sweep_pool_.join();
    LOGINFOMOD(blkalloc, "SweepService stopped");
}

shared< SweepService::AllocatorHandle >
SweepService::register_allocator(SegmentManager& seg_mgr, RefillFn refill, std::string name) {
    auto h = std::make_shared< AllocatorHandle >(seg_mgr, std::move(refill), std::move(name));
    std::lock_guard lk{registry_mtx_};
    registry_.push_back(h);
    return h;
}

void SweepService::request_refill(AllocatorHandle& h, InmemPortion& portion) {
    if (!h.alive_.load(std::memory_order_acquire))
        return;
    // shared_from_this isn't available — we look up the shared_ptr by walking the registry. The hot path
    // here is the slow alloc path (cache miss), so the registry scan is fine.
    std::shared_ptr< AllocatorHandle > sp;
    {
        std::lock_guard lk{registry_mtx_};
        for (auto const& w : registry_) {
            if (auto s = w.lock(); s.get() == &h) {
                sp = std::move(s);
                break;
            }
        }
    }
    if (!sp)
        return;
    try_enqueue(std::move(sp), portion, SWEEP_PRIO_LOW);
}

void SweepService::request_refill_blocking(AllocatorHandle& h, InmemPortion& portion,
                                                   blk_count_t wait_for_blks) {
    if (!h.alive_.load(std::memory_order_acquire))
        return;

    std::shared_ptr< AllocatorHandle > sp;
    {
        std::lock_guard lk{registry_mtx_};
        for (auto const& w : registry_) {
            if (auto s = w.lock(); s.get() == &h) {
                sp = std::move(s);
                break;
            }
        }
    }
    if (!sp)
        return;

    try_enqueue(sp, portion, SWEEP_PRIO_HIGH);

    // Poll until the slab cache hits the requested level or the service is shutting down. The wait is
    // bounded by the refill itself (which runs on the pool); typical wait is sub-millisecond.
    while (!stop_.load(std::memory_order_acquire)) {
        if (portion.slab_cache_.total_cached_blks() >= wait_for_blks)
            return;
        std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
}

bool SweepService::try_enqueue(std::shared_ptr< AllocatorHandle > h, InmemPortion& portion, int8_t priority) {
    bool expected = false;
    if (!portion.enqueued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    auto* portion_ptr = &portion;
    sweep_pool_.addWithPriority(
        [this, h = std::move(h), portion_ptr]() mutable { run_refill(std::move(h), portion_ptr); }, priority);
    return true;
}

void SweepService::run_refill(std::shared_ptr< AllocatorHandle > h, InmemPortion* portion) {
    // Reserve our slot first, then check alive_.  Inverting the order (check then reserve) races with
    // ~AllocatorHandle: it could read alive_=true here, the dtor flips alive_=false and observes in_flight_=0,
    // the allocator is freed, and we then call refill_fn on dead memory.  By incrementing first and using
    // seq_cst on the read of alive_, the dtor either sees in_flight_>0 (and spins) or we see alive_=false
    // (and back out before touching the allocator).
    h->in_flight_.fetch_add(1, std::memory_order_seq_cst);
    if (!h->alive_.load(std::memory_order_seq_cst)) {
        h->in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        portion->enqueued_.store(false, std::memory_order_release);
        return;
    }
    try {
        h->refill_fn(*portion);
    } catch (...) {
        // Swallow — refill is best-effort. Logging deferred to caller.
    }
    portion->enqueued_.store(false, std::memory_order_release);
    h->in_flight_.fetch_sub(1, std::memory_order_acq_rel);
}

void SweepService::ticker_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lk{ticker_mtx_};
            ticker_cv_.wait_for(lk, tick_, [this]() { return stop_.load(std::memory_order_acquire); });
        }
        if (stop_.load(std::memory_order_acquire))
            break;

        // Snapshot live handles to release the registry mutex before scanning portions.
        std::vector< std::shared_ptr< AllocatorHandle > > live;
        {
            std::lock_guard lk{registry_mtx_};
            live.reserve(registry_.size());
            auto it = registry_.begin();
            while (it != registry_.end()) {
                if (auto s = it->lock(); s && s->alive_.load(std::memory_order_acquire)) {
                    live.push_back(std::move(s));
                    ++it;
                } else if (it->expired()) {
                    it = registry_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& h : live) {
            for (auto& seg : h->seg_mgr->segments()) {
                for (auto& p_ptr : seg.portions_) {
                    InmemPortion& portion = *p_ptr;
                    if (portion.slab_cache_.needs_refill()) {
                        try_enqueue(h, portion, SWEEP_PRIO_LOW);
                    }
                }
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//                         Module-scoped service access
// ──────────────────────────────────────────────────────────────────────────────

namespace {
std::unique_ptr< SweepService > g_service;
std::mutex g_service_mtx;
} // namespace

void init_sweep_service() {
    std::lock_guard lk{g_service_mtx};
    if (g_service)
        return;
    const auto num_workers = HS_RUNTIME_CONFIG(blkallocator.num_slab_sweeper_threads);
    const auto tick = std::chrono::milliseconds{HS_RUNTIME_CONFIG(blkallocator.slab_refill_frequency_ms)};
    g_service = std::make_unique< SweepService >(num_workers, tick);
}

void shutdown_sweep_service() {
    std::lock_guard lk{g_service_mtx};
    g_service.reset();
}

SweepService& sweep_service() {
    HS_REL_ASSERT(g_service != nullptr, "blkalloc::sweep_service() called before init_sweep_service()");
    return *g_service;
}

} // namespace blkalloc
} // namespace homestore