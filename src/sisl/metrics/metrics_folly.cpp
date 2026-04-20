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
    ncntrs_ = to_u32(num_counters());
    nhists_ = to_u32(num_histograms());
    acc_counters_.resize(ncntrs_, 0);
    acc_digests_.resize(nhists_, folly::TDigest{128});
}

FollyRcuMetricsGroup::ThreadMetricsPtr* FollyRcuMetricsGroup::get_or_create() {
    ThreadMetricsPtr* m = tl_metrics_.get();
    if (FOLLY_UNLIKELY(!m)) {
        auto* p = new ThreadMetricsPtr{ncntrs_, nhists_};
        // The destructor lambda fires when the thread exits.  Rather than deleting p we push it onto the zombie list
        // so that the next collect() still aggregates its data.
        tl_metrics_.reset(p, [this](ThreadMetricsPtr* ptr, folly::TLPDestructionMode) { push_zombie(ptr); });
        m = p;
    }
    return m;
}

void FollyRcuMetricsGroup::push_zombie(ThreadMetricsPtr* p) {
    std::unique_lock lock{zombie_mutex_};
    zombie_list_.emplace_back(p);
}

// ─── Record path ─────────────────────────────────────────────────────────────
// Writers hold the RCU read-side section for the duration of the write. The collector calls Rcu::synchronize() which
// blocks until every read-side section has exited — after that, no thread is touching any per-thread data.

void FollyRcuMetricsGroup::counter_increment(uint64_t index, int64_t val) {
    auto acc = get_or_create()->access();
    acc->counters[index] += val;
}

void FollyRcuMetricsGroup::counter_decrement(uint64_t index, int64_t val) {
    auto acc = get_or_create()->access();
    acc->counters[index] -= val;
}

void FollyRcuMetricsGroup::histogram_observe(uint64_t index, int64_t val) {
    auto acc = get_or_create()->access();
    acc->pending[index].push_back(to_double(val));
    if (acc->pending[index].size() >= histogram_flush_threshold) { acc->flush_histogram(to_u32(index)); }
}

void FollyRcuMetricsGroup::histogram_observe(uint64_t index, int64_t val, uint64_t count) {
    auto acc = get_or_create()->access();
    for (uint64_t i = 0; i < count; ++i) {
        acc->pending[index].push_back(to_double(val));
    }
    if (acc->pending[index].size() >= histogram_flush_threshold) { acc->flush_histogram(to_u32(index)); }
}

// ─── Collect path ─────────────────────────────────────────────────────────────

void FollyRcuMetricsGroup::collect_and_reset(PerThreadMetrics& ptm) {
    for (uint32_t i = 0; i < ncntrs_; ++i) {
        acc_counters_[i] += ptm.counters[i];
        ptm.counters[i] = 0;
    }
    for (uint32_t i = 0; i < nhists_; ++i) {
        ptm.flush_histogram(i);
        if (ptm.digests[i].count() > 0) {
            acc_digests_[i] = folly::TDigest::merge(
                folly::range(std::initializer_list< folly::TDigest >{acc_digests_[i], ptm.digests[i]}));
            ptm.digests[i] = folly::TDigest{128};
        }
    }
}

void FollyRcuMetricsGroup::gather_result(bool need_latest, const CounterGatherCb& counter_cb,
                                         const GaugeGatherCb& gauge_cb, const HistogramGatherCb& histogram_cb) {
    if (need_latest) {
        // Wisr-style pointer-swap rotation: atomically exchange every live thread's slot with a fresh PerThreadMetrics,
        // then synchronize_rcu() waits for any writer that had dereferenced an old pointer to finish.  After
        // synchronize returns, olds are exclusively owned by the collector and safe to drain.  rotate_mutex_
        // serialises concurrent gather_result() callers; drop it if gather is guaranteed to be called single-threaded.
        std::lock_guard rlock{rotate_mutex_};
        std::vector< PerThreadMetrics* > olds;
        for (auto& slot : tl_metrics_.accessAllThreads()) {
            olds.push_back(slot.make_and_exchange(false /* sync_rcu_now */));
        }
        Rcu::synchronize();
        for (auto* old : olds) {
            collect_and_reset(*old);
            delete old;
        }

        // Drain zombie list — exited threads whose data hasn't been collected yet.  No live writers on zombies, so
        // direct access() (read_guard-backed read) is safe; no swap needed.
        {
            std::unique_lock lock{zombie_mutex_};
            for (auto& zm : zombie_list_) {
                auto acc = zm->access();
                collect_and_reset(*acc);
            }
            zombie_list_.clear();
        }
    }

    for (uint32_t i = 0; i < ncntrs_; ++i) {
        CounterValue cv;
        cv.increment(acc_counters_[i]);
        counter_cb(i, cv);
    }
    for (uint32_t i = 0; i < num_gauges(); ++i) {
        gauge_cb(i, gauge_values_[i]);
    }
    for (uint32_t i = 0; i < nhists_; ++i) {
        histogram_cb(i, acc_digests_[i]);
    }
}

} // namespace sisl
