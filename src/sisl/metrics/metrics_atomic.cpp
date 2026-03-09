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
#include "sisl/metrics/metrics_atomic.h"

namespace sisl {

void AtomicMetricsGroup::on_register() {
    counter_values_ = std::make_unique< AtomicCounterValue[] >(num_counters());
    histogram_values_ = std::make_unique< AtomicHistogramValue[] >(num_histograms());
}

void AtomicMetricsGroup::gather_result([[maybe_unused]] bool need_latest, const CounterGatherCb& counter_cb,
                                       const GaugeGatherCb& gauge_cb, const HistogramGatherCb& histogram_cb) {
    for (size_t i{0}; i < num_counters(); ++i) {
        counter_cb(i, counter_values_[i].to_counter_value());
    }
    for (size_t i{0}; i < num_gauges(); ++i) {
        gauge_cb(i, gauge_values_[i]);
    }
    for (size_t i{0}; i < num_histograms(); ++i) {
        histogram_cb(i, histogram_values_[i].to_tdigest());
    }
}

void AtomicMetricsGroup::counter_increment(uint64_t index, int64_t val) { counter_values_[index].increment(val); }

void AtomicMetricsGroup::counter_decrement(uint64_t index, int64_t val) { counter_values_[index].decrement(val); }

void AtomicMetricsGroup::histogram_observe(uint64_t index, int64_t val) { histogram_values_[index].observe(val); }

void AtomicMetricsGroup::histogram_observe(uint64_t index, int64_t val, uint64_t count) {
    histogram_values_[index].observe(val, count);
}

} // namespace sisl
