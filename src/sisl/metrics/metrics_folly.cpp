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
#include "sisl/metrics/metrics_folly.h"

namespace sisl {

FollyRcuMetricsGroup::~FollyRcuMetricsGroup() = default;

void FollyRcuMetricsGroup::on_register() {
    ncntrs_ = static_cast< uint32_t >(num_counters());
    nhists_ = static_cast< uint32_t >(num_histograms());
    // Thread-local PerThreadMetrics are created lazily on first access per thread.
}

PerThreadMetrics* FollyRcuMetricsGroup::get_or_create() {
    PerThreadMetrics* m = tl_metrics_.get();
    if (FOLLY_UNLIKELY(!m)) {
        auto* p = new PerThreadMetrics{ncntrs_, nhists_};
        // The destructor lambda fires when the thread exits.  Rather than
        // deleting p we push it onto the zombie list so that the next
        // collect() still aggregates its data.
        tl_metrics_.reset(p, [this](PerThreadMetrics* ptr, folly::TLPDestructionMode) { push_zombie(ptr); });
        m = p;
    }
    return m;
}

void FollyRcuMetricsGroup::push_zombie(PerThreadMetrics* p) {
    std::unique_lock lock{zombie_mutex_};
    zombie_list_.emplace_back(p);
}

// ─── Record path ─────────────────────────────────────────────────────────────

void FollyRcuMetricsGroup::counter_increment(uint64_t index, int64_t val) {
    PerThreadMetrics* m = get_or_create();
    folly::rcu_reader guard;
    m->counters_[index].fetch_add(val, std::memory_order_relaxed);
}

void FollyRcuMetricsGroup::counter_decrement(uint64_t index, int64_t val) {
    PerThreadMetrics* m = get_or_create();
    folly::rcu_reader guard;
    m->counters_[index].fetch_sub(val, std::memory_order_relaxed);
}

void FollyRcuMetricsGroup::histogram_observe(uint64_t index, int64_t val) {
    get_or_create()->histograms_[index].observe(static_cast< double >(val));
}

void FollyRcuMetricsGroup::histogram_observe(uint64_t index, int64_t val, uint64_t count) {
    get_or_create()->histograms_[index].observe(static_cast< double >(val), count);
}

// ─── Collect path ─────────────────────────────────────────────────────────────

void FollyRcuMetricsGroup::gather_result([[maybe_unused]] bool need_latest, const CounterGatherCb& counter_cb,
                                         const GaugeGatherCb& gauge_cb, const HistogramGatherCb& histogram_cb) {
    // Wait for all in-flight counter increments (which hold an rcu_reader) to
    // finish.  After this point every fetch_add that started before this call
    // has completed and is visible via the acquire semantics of load() below.
    folly::rcu_synchronize();

    // Accumulate across all live threads.
    std::vector< CounterValue > counters(ncntrs_);
    std::vector< folly::TDigest > digests(nhists_, folly::TDigest{128});

    auto aggregate = [&](PerThreadMetrics& tl) {
        for (uint32_t i = 0; i < ncntrs_; ++i) {
            counters[i].increment(tl.counters_[i].load(std::memory_order_relaxed));
        }
        for (uint32_t i = 0; i < nhists_; ++i) {
            auto d = tl.histograms_[i].snapshot();
            if (d.count() > 0) {
                digests[i] = folly::TDigest::merge(folly::range(std::initializer_list< folly::TDigest >{digests[i], d}));
            }
        }
    };

    for (auto& tl : tl_metrics_.accessAllThreads()) {
        aggregate(tl);
    }

    // Drain zombie list (exited threads).
    {
        std::unique_lock lock{zombie_mutex_};
        for (auto& zm : zombie_list_) {
            aggregate(*zm);
        }
        zombie_list_.clear();
    }

    for (uint32_t i = 0; i < ncntrs_; ++i) {
        counter_cb(i, counters[i]);
    }
    for (uint32_t i = 0; i < num_gauges(); ++i) {
        gauge_cb(i, gauge_values_[i]);
    }
    for (uint32_t i = 0; i < nhists_; ++i) {
        histogram_cb(i, digests[i]);
    }
}

} // namespace sisl
