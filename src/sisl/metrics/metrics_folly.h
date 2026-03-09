/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <folly/MicroSpinLock.h>
#include <folly/ThreadLocal.h>
#include <folly/synchronization/Rcu.h>
#include <folly/stats/TDigest.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include "metrics_group_impl.h"

namespace sisl {

// Per-histogram accumulator for one thread.  Hot path is a single push_back
// under an uncontended MicroSpinLock.  The collector drains and merges the
// pending buffer into a running TDigest snapshot under the same lock.
struct PerHistogramData {
    folly::MicroSpinLock spin_{};
    std::vector< double > pending_;
    folly::TDigest digest_;

    PerHistogramData() : digest_{128 /* max_centroids */} { spin_.init(); }

    void observe(double value, uint64_t count = 1) {
        folly::MSLGuard g{spin_};
        for (uint64_t i = 0; i < count; ++i) {
            pending_.push_back(value);
        }
        if (pending_.size() >= 128) { flush_locked(); }
    }

    // Called by collector: flush pending samples and return a copy of the digest.
    folly::TDigest snapshot() {
        folly::MSLGuard g{spin_};
        flush_locked();
        return digest_;
    }

private:
    void flush_locked() {
        if (pending_.empty()) { return; }
        std::sort(pending_.begin(), pending_.end());
        digest_ = digest_.merge(folly::range(pending_));
        pending_.clear();
    }
};

// Per-thread metric storage for one MetricsGroup instance.
// Counters use std::atomic<int64_t> with relaxed ordering — safe because only
// one thread writes, and the RCU barrier in the collect path provides the
// necessary happens-before edge before the collector reads them.
class PerThreadMetrics {
public:
    PerThreadMetrics(uint32_t ncntrs, uint32_t nhists) : counters_(ncntrs), histograms_(nhists) {}

    PerThreadMetrics(const PerThreadMetrics&) = delete;
    PerThreadMetrics& operator=(const PerThreadMetrics&) = delete;

    std::vector< std::atomic< int64_t > > counters_;
    std::vector< PerHistogramData > histograms_;
};

/*
 * FollyRcuMetricsGroup
 *
 * Counter record path  : enter RCU read section → plain relaxed atomic add → exit.
 *                        Zero contention; no cache-line bouncing.
 * Histogram record path: push_back into per-thread buffer under MicroSpinLock
 *                        (uncontended 99.99% of the time).
 * Collect path         : rcu_synchronize() → iterate all thread-locals →
 *                        drain zombie list (data from exited threads).
 *
 * Replaces WisrBufferMetricsGroup (urcu) + ThreadBufferMetricsGroup (signal).
 */
class FollyRcuMetricsGroup : public MetricsGroupImpl {
public:
    FollyRcuMetricsGroup(const std::string& grp_name, const std::string& inst_name) :
            MetricsGroupImpl{grp_name, inst_name} {}

    ~FollyRcuMetricsGroup() override;

    FollyRcuMetricsGroup(const FollyRcuMetricsGroup&) = delete;
    FollyRcuMetricsGroup& operator=(const FollyRcuMetricsGroup&) = delete;

    void counter_increment(uint64_t index, int64_t val = 1) override;
    void counter_decrement(uint64_t index, int64_t val = 1) override;
    void histogram_observe(uint64_t index, int64_t val) override;
    void histogram_observe(uint64_t index, int64_t val, uint64_t count) override;

    [[nodiscard]] GroupImplType impl_type() const override { return GroupImplType::Rcu; }

private:
    void on_register() override;
    void gather_result(bool need_latest, const CounterGatherCb& counter_cb, const GaugeGatherCb& gauge_cb,
                       const HistogramGatherCb& histogram_cb) override;

    // Get or lazily create the thread-local PerThreadMetrics for this group.
    PerThreadMetrics* get_or_create();

    // Called by ThreadLocalPtr destructor when a thread exits — instead of
    // deleting, we move the data to the zombie list so the collector can still
    // aggregate it on the next gather.
    void push_zombie(PerThreadMetrics* p);

private:
    uint32_t ncntrs_{0};
    uint32_t nhists_{0};

    folly::ThreadLocalPtr< PerThreadMetrics > tl_metrics_;

    // Zombie list: PerThreadMetrics instances from threads that have already
    // exited.  Guarded by zombie_mutex_ (zombie push is rare — only on thread
    // exit; zombie drain is periodic — only during collect).
    std::mutex zombie_mutex_;
    std::vector< unique< PerThreadMetrics > > zombie_list_;
};

} // namespace sisl
