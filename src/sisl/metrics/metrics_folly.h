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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <folly/ThreadLocal.h>
#include <folly/stats/TDigest.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include <sisl/fds/rcu.h>
#include "metrics_group_impl.h"

namespace sisl {

static constexpr uint32_t histogram_flush_threshold{256};

// Per-thread metric storage for one MetricsGroup instance. Only the owning thread writes (under RCU read-side guard).
// The collector calls Rcu::synchronize() first, which guarantees no writer is active, then freely reads+resets.
struct PerThreadMetrics {
    std::vector< int64_t > counters;
    std::vector< folly::TDigest > digests;
    std::vector< std::vector< double > > pending; // bounded batch buffer flushed into digest at threshold

    PerThreadMetrics(uint32_t ncntrs, uint32_t nhists) :
            counters(ncntrs, 0), digests(nhists, folly::TDigest{128}), pending(nhists) {}

    PerThreadMetrics(const PerThreadMetrics&) = delete;
    PerThreadMetrics& operator=(const PerThreadMetrics&) = delete;

    // Flush pending samples for histogram `idx` into its running TDigest.
    void flush_histogram(uint32_t idx) {
        if (pending[idx].empty()) { return; }
        std::sort(pending[idx].begin(), pending[idx].end());
        digests[idx] = digests[idx].merge(folly::sorted_equivalent, folly::range(pending[idx]));
        pending[idx].clear();
    }
};

/*
 * FollyRcuMetricsGroup — lock-free per-thread metrics using liburcu + folly::ThreadLocalPtr.
 *
 * Record path (counter/histogram): enter RCU read-side section via Rcu::read_guard, write to per-thread data, exit.
 *   Zero contention — only the owning thread touches its data, plain int64_t and vector operations, no atomics,
 *   no locks.
 *
 * Collect path: Rcu::synchronize() guarantees all writers have exited their read-side sections, so the collector has
 *   exclusive access to every thread's data. It reads+resets each thread's counters and histograms, merges into
 *   persistent accumulators, then drains the zombie list (data from exited threads).
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

    // Called by ThreadLocalPtr destructor when a thread exits — instead of deleting, we move the data to the zombie
    // list so the collector can still aggregate it on the next gather.
    void push_zombie(PerThreadMetrics* p);

    // Collect counters and histograms from a PerThreadMetrics into the accumulators, then reset it.
    void collect_and_reset(PerThreadMetrics& ptm);

private:
    uint32_t ncntrs_{0};
    uint32_t nhists_{0};

    // Accumulated results across all collection cycles. Only touched by the collector under gather's lock.
    std::vector< int64_t > acc_counters_;
    std::vector< folly::TDigest > acc_digests_;

    // Zombie list: PerThreadMetrics from exited threads. Guarded by zombie_mutex_.
    // IMPORTANT: zombie_mutex_ and zombie_list_ must be declared BEFORE tl_metrics_ so that tl_metrics_ is destroyed
    // first. Its destructor fires push_zombie() which locks zombie_mutex_ — that mutex must still be alive.
    std::mutex zombie_mutex_;
    std::vector< unique< PerThreadMetrics > > zombie_list_;

    // Unique tag so folly::ThreadLocalPtr allows accessAllThreads().
    struct FollyRcuMetricsTag {};
    folly::ThreadLocalPtr< PerThreadMetrics, FollyRcuMetricsTag > tl_metrics_;
};

} // namespace sisl
