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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <folly/stats/TDigest.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include "metrics_group_impl.h"

namespace sisl {

class AtomicCounterValue {
public:
    AtomicCounterValue() = default;
    AtomicCounterValue(const AtomicCounterValue&) = delete;
    AtomicCounterValue(AtomicCounterValue&&) noexcept = delete;
    AtomicCounterValue& operator=(const AtomicCounterValue&) = delete;
    AtomicCounterValue& operator=(AtomicCounterValue&&) noexcept = delete;

    void increment(int64_t value = 1) { value_.fetch_add(value, std::memory_order_relaxed); }
    void decrement(int64_t value = 1) { value_.fetch_sub(value, std::memory_order_relaxed); }
    [[nodiscard]] int64_t get() const { return value_.load(std::memory_order_relaxed); }

    [[nodiscard]] CounterValue to_counter_value() const {
        CounterValue v{};
        v.increment(get());
        return v;
    }

private:
    std::atomic< int64_t > value_{0};
};

// Histogram accumulator for AtomicMetricsGroup.  Uses a mutex-protected
// sample buffer that is flushed into a TDigest on collection.  Slightly
// heavier than PerHistogramData (uses std::mutex instead of MicroSpinLock)
// but AtomicMetricsGroup is already the "heavyweight" impl type.
class AtomicHistogramValue {
public:
    AtomicHistogramValue() : digest_{128} {}

    AtomicHistogramValue(const AtomicHistogramValue&) = delete;
    AtomicHistogramValue(AtomicHistogramValue&&) noexcept = delete;
    AtomicHistogramValue& operator=(const AtomicHistogramValue&) = delete;
    AtomicHistogramValue& operator=(AtomicHistogramValue&&) noexcept = delete;

    void observe(int64_t value, uint64_t count = 1) {
        std::unique_lock lock{mutex_};
        for (uint64_t i = 0; i < count; ++i) {
            pending_.push_back(static_cast< double >(value));
        }
    }

    [[nodiscard]] folly::TDigest to_tdigest() {
        std::unique_lock lock{mutex_};
        if (!pending_.empty()) {
            std::sort(pending_.begin(), pending_.end());
            digest_ = digest_.merge(folly::range(pending_));
            pending_.clear();
        }
        return digest_;
    }

private:
    std::mutex mutex_;
    std::vector< double > pending_;
    folly::TDigest digest_;
};

class AtomicMetricsGroup : public MetricsGroupImpl {
public:
    AtomicMetricsGroup(const std::string& grp_name, const std::string& inst_name) :
            MetricsGroupImpl{grp_name, inst_name} {}
    ~AtomicMetricsGroup() override = default;

    AtomicMetricsGroup(const AtomicMetricsGroup&) = delete;
    AtomicMetricsGroup(AtomicMetricsGroup&&) noexcept = delete;
    AtomicMetricsGroup& operator=(const AtomicMetricsGroup&) = delete;
    AtomicMetricsGroup& operator=(AtomicMetricsGroup&&) noexcept = delete;

    void counter_increment(uint64_t index, int64_t val = 1) override;
    void counter_decrement(uint64_t index, int64_t val = 1) override;
    void histogram_observe(uint64_t index, int64_t val) override;
    void histogram_observe(uint64_t index, int64_t val, uint64_t count) override;

    [[nodiscard]] GroupImplType impl_type() const override { return GroupImplType::Atomic; }

private:
    void on_register() override;
    void gather_result(bool need_latest, const CounterGatherCb& counter_cb, const GaugeGatherCb& gauge_cb,
                       const HistogramGatherCb& histogram_cb) override;

private:
    unique< AtomicCounterValue[] > counter_values_;
    unique< AtomicHistogramValue[] > histogram_values_;
};

} // namespace sisl
