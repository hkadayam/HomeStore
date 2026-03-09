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
#include <algorithm>

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <folly/Synchronized.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include <fmt/format.h>
#include <sisl/logging/logging.h>

#include "sisl/metrics/metrics_group_impl.h"
#include "sisl/metrics/metrics.h"

namespace sisl {

// ─── MetricsGroup ─────────────────────────────────────────────────────────────

MetricsGroupImplPtr MetricsGroup::make_group(const std::string& grp_name, const std::string& inst_name,
                                             GroupImplType type) {
    if (type == GroupImplType::Rcu) {
        return std::make_shared< FollyRcuMetricsGroup >(grp_name, inst_name);
    } else if (type == GroupImplType::Atomic) {
        return std::make_shared< AtomicMetricsGroup >(grp_name, inst_name);
    }
    return nullptr;
}

MetricsGroup::MetricsGroup(const std::string& grp_name, const std::string& inst_name, GroupImplType type) {
    impl_ptr_ = make_group(grp_name, inst_name, type);
    farm_ptr_ = MetricsFarm::get_instance_ptr();
}

MetricsGroup::~MetricsGroup() {
    deregister_me_from_farm();
    impl_ptr_.reset();
    farm_ptr_.reset();
}

void MetricsGroup::register_me_to_farm() {
    MetricsFarm::getInstance().register_metrics_group(impl_ptr_);
    is_registered_.store(true);
}

void MetricsGroup::deregister_me_from_farm() {
    if (is_registered_.load() && MetricsFarm::is_initialized()) {
        MetricsFarm::getInstance().deregister_metrics_group(impl_ptr_);
        is_registered_.store(false);
    }
}

void MetricsGroup::register_me_to_parent(MetricsGroup* parent) {
    parent->impl_ptr_->add_child_group(impl_ptr_);
    MetricsFarm::getInstance().register_metrics_group(impl_ptr_, false /* add_to_farm_list */);
}

nlohmann::json MetricsGroup::get_result_in_json(bool need_latest) {
    return impl_ptr_->get_result_in_json(need_latest);
}

void MetricsGroup::gather() { impl_ptr_->gather(); }
void MetricsGroup::attach_gather_cb(const OnGatherCb& cb) { impl_ptr_->attach_gather_cb(cb); }
void MetricsGroup::detach_gather_cb() { impl_ptr_->detach_gather_cb(); }

// ─── MetricsGroupStaticInfo ────────────────────────────────────────────────────

MetricsGroupStaticInfo::MetricsGroupStaticInfo(const std::string& grp_name) : grp_name_{grp_name} {}

uint64_t MetricsGroupStaticInfo::register_counter(const std::string& name, const std::string& desc,
                                                  const std::string& report_name, const MetricLabel& label_pair) {
    counters_.emplace_back(name, desc, report_name, label_pair);
    return counters_.size() - 1;
}

uint64_t MetricsGroupStaticInfo::register_gauge(const std::string& name, const std::string& desc,
                                                const std::string& report_name, const MetricLabel& label_pair) {
    gauges_.emplace_back(name, desc, report_name, label_pair);
    return gauges_.size() - 1;
}

uint64_t MetricsGroupStaticInfo::register_histogram(const std::string& name, const std::string& desc,
                                                    const std::string& report_name, const MetricLabel& label_pair) {
    histograms_.emplace_back(name, desc, report_name, label_pair);
    return histograms_.size() - 1;
}

shared< MetricsGroupStaticInfo > MetricsGroupStaticInfo::create_or_get_info(const std::string& grp_name) {
    static folly::Synchronized< std::unordered_map< std::string, shared< MetricsGroupStaticInfo > > > grp_map;

    shared< MetricsGroupStaticInfo > ret;
    grp_map.withWLock([&grp_name, &ret](auto& m) {
        auto it_pair{m.try_emplace(grp_name, std::make_shared< MetricsGroupStaticInfo >(grp_name))};
        ret = it_pair.first->second;
    });
    return ret;
}

// ─── MetricsGroupImpl ──────────────────────────────────────────────────────────

MetricsGroupImpl::MetricsGroupImpl(const std::string& grp_name, const std::string& inst_name) {
    inst_name_ = MetricsFarm::getInstance().ensure_unique(grp_name, inst_name);
    static_info_ = MetricsGroupStaticInfo::create_or_get_info(grp_name);
    static_info_->mutex_.lock();
    // Held until registration_completed() — prevents concurrent instances of
    // the same group type from interleaving their register_counter calls.
}

MetricsGroupImpl::~MetricsGroupImpl() {
    for (size_t idx{0}; idx < counters_dinfo_.size(); ++idx) {
        counters_dinfo_[idx].unregister(static_info_->counters_[idx]);
    }
    for (size_t idx{0}; idx < gauges_dinfo_.size(); ++idx) {
        gauges_dinfo_[idx].unregister(static_info_->gauges_[idx]);
    }
    for (size_t idx{0}; idx < histograms_dinfo_.size(); ++idx) {
        histograms_dinfo_[idx].unregister(static_info_->histograms_[idx]);
    }
}

void MetricsGroupImpl::registration_completed() {
    gauge_values_.resize(static_info_->gauges_.size(), GaugeValue{});
    static_info_->reg_pending_ = false;
    static_info_->mutex_.unlock();
}

uint64_t MetricsGroupImpl::register_counter(const std::string& name, const std::string& desc,
                                            const std::string& report_name, const MetricLabel& label_pair,
                                            PublishAs ptype) {
    const auto idx = counters_dinfo_.size();
    if (static_info_->reg_pending_) {
        [[maybe_unused]] auto s_idx = static_info_->register_counter(name, desc, report_name, label_pair);
        assert(idx == s_idx);
    }
    counters_dinfo_.emplace_back(static_info_->counters_[idx], inst_name_, ptype);
    return idx;
}

uint64_t MetricsGroupImpl::register_counter(const std::string& name, const std::string& desc,
                                            const MetricLabel& label_pair, PublishAs ptype) {
    return register_counter(name, desc, "", label_pair, ptype);
}

uint64_t MetricsGroupImpl::register_counter(const std::string& name, const std::string& desc, PublishAs ptype) {
    return register_counter(name, desc, "", {"", ""}, ptype);
}

uint64_t MetricsGroupImpl::register_gauge(const std::string& name, const std::string& desc,
                                          const std::string& report_name, const MetricLabel& label_pair) {
    const auto idx = gauges_dinfo_.size();
    if (static_info_->reg_pending_) {
        [[maybe_unused]] auto s_idx = static_info_->register_gauge(name, desc, report_name, label_pair);
        assert(idx == s_idx);
    }
    gauges_dinfo_.emplace_back(static_info_->gauges_[idx], inst_name_);
    return idx;
}

uint64_t MetricsGroupImpl::register_gauge(const std::string& name, const std::string& desc,
                                          const MetricLabel& label_pair) {
    return register_gauge(name, desc, "", label_pair);
}

uint64_t MetricsGroupImpl::register_histogram(const std::string& name, const std::string& desc,
                                              const std::string& report_name, const MetricLabel& label_pair,
                                              PublishAs ptype) {
    const auto idx = histograms_dinfo_.size();
    if (static_info_->reg_pending_) {
        [[maybe_unused]] auto s_idx = static_info_->register_histogram(name, desc, report_name, label_pair);
        assert(idx == s_idx);
    }
    histograms_dinfo_.emplace_back(static_info_->histograms_[idx], inst_name_, ptype);
    return idx;
}

uint64_t MetricsGroupImpl::register_histogram(const std::string& name, const std::string& desc,
                                              const MetricLabel& label_pair, PublishAs ptype) {
    return register_histogram(name, desc, "", label_pair, ptype);
}

uint64_t MetricsGroupImpl::register_histogram(const std::string& name, const std::string& desc, PublishAs ptype) {
    return register_histogram(name, desc, "", {"", ""}, ptype);
}

void MetricsGroupImpl::gauge_update(uint64_t index, int64_t val) { gauge_values_[index].update(val); }

const std::string& MetricsGroupImpl::get_group_name() const { return static_info_->grp_name_; }
const std::string& MetricsGroupImpl::get_instance_name() const { return inst_name_; }

nlohmann::json MetricsGroupImpl::get_result_in_json(bool need_latest) {
    auto locked = lock();
    nlohmann::json json;
    nlohmann::json counter_entries;
    nlohmann::json gauge_entries;
    nlohmann::json hist_entries;

    if (on_gather_cb_) { on_gather_cb_(); }
    gather_result(
        need_latest,
        [&counter_entries, this](uint64_t idx, const CounterValue& result) {
            counter_entries[counter_static_info(idx).desc()] = result.get();
        },
        [&gauge_entries, this](uint64_t idx, const GaugeValue& result) {
            gauge_entries[gauge_static_info(idx).desc()] = result.get();
        },
        [&hist_entries, this](uint64_t idx, const folly::TDigest& digest) {
            const auto& info = hist_static_info(idx);
            if (hist_dynamic_info(idx).is_histogram_reporter()) {
                hist_entries[info.desc()] = fmt::format(
                    "{:.1f} / {:.1f} / {:.1f} / {:.1f}",
                    digest.count() > 0 ? digest.mean() : 0.0, digest.estimateQuantile(0.50),
                    digest.estimateQuantile(0.95), digest.estimateQuantile(0.99));
            } else {
                hist_entries[info.desc()] = digest.count() > 0 ? digest.mean() : 0.0;
            }
        });

    json["Counters"] = counter_entries;
    json["Gauges"] = gauge_entries;
    json["Histograms percentiles avg/p50/p95/p99"] = hist_entries;

    for (auto& cg : child_groups_) {
        json[cg->inst_name_] = cg->get_result_in_json(need_latest);
    }
    return json;
}

void MetricsGroupImpl::publish_result() {
    auto locked = lock();
    if (on_gather_cb_) { on_gather_cb_(); }
    gather_result(
        true,
        [this](uint64_t idx, const CounterValue& result) { counter_dynamic_info(idx).publish(result); },
        [this](uint64_t idx, const GaugeValue& result) { gauge_dynamic_info(idx).publish(result); },
        [this](uint64_t idx, const folly::TDigest& digest) { hist_dynamic_info(idx).publish(digest); });

    for (auto& cg : child_groups_) { cg->publish_result(); }
}

void MetricsGroupImpl::gather() {
    auto locked = lock();
    if (on_gather_cb_) { on_gather_cb_(); }
    gather_result(
        true,
        []([[maybe_unused]] uint64_t, [[maybe_unused]] const CounterValue&) {},
        []([[maybe_unused]] uint64_t, [[maybe_unused]] const GaugeValue&) {},
        []([[maybe_unused]] uint64_t, [[maybe_unused]] const folly::TDigest&) {});

    for (auto& cg : child_groups_) { cg->gather(); }
}

// ─── CounterStaticInfo / CounterDynamicInfo ────────────────────────────────────

CounterStaticInfo::CounterStaticInfo(const std::string& name, const std::string& desc,
                                     const std::string& report_name, const MetricLabel& label_pair) :
        name_{report_name.empty() ? name : report_name}, desc_{desc} {
    if (!label_pair.first.empty() && !label_pair.second.empty()) { label_pair_ = label_pair; }
}

CounterDynamicInfo::CounterDynamicInfo(const CounterStaticInfo& static_info, const std::string& instance_name,
                                       PublishAs ptype) {
    if (ptype == PublishAs::Counter) {
        report_counter_gauge_ = MetricsFarm::get_reporter().add_counter(static_info.name_, static_info.desc_,
                                                                        instance_name, static_info.label_pair_);
    } else {
        report_counter_gauge_ = MetricsFarm::get_reporter().add_gauge(static_info.name_, static_info.desc_,
                                                                      instance_name, static_info.label_pair_);
    }
}

void CounterDynamicInfo::publish(const CounterValue& value) {
    if (is_counter_reporter()) {
        as_counter()->set_value(static_cast< double >(value.get()));
    } else {
        as_gauge()->set_value(static_cast< double >(value.get()));
    }
}

void CounterDynamicInfo::unregister(const CounterStaticInfo& static_info) {
    if (is_counter_reporter()) {
        MetricsFarm::get_reporter().remove_counter(static_info.name_, as_counter());
    } else {
        MetricsFarm::get_reporter().remove_gauge(static_info.name_, as_gauge());
    }
}

// ─── GaugeStaticInfo / GaugeDynamicInfo ───────────────────────────────────────

GaugeStaticInfo::GaugeStaticInfo(const std::string& name, const std::string& desc, const std::string& report_name,
                                 const MetricLabel& label_pair) :
        name_{report_name.empty() ? name : report_name}, desc_{desc} {
    if (!label_pair.first.empty() && !label_pair.second.empty()) { label_pair_ = label_pair; }
}

GaugeDynamicInfo::GaugeDynamicInfo(const GaugeStaticInfo& static_info, const std::string& instance_name) {
    report_gauge_ =
        MetricsFarm::get_reporter().add_gauge(static_info.name_, static_info.desc_, instance_name, static_info.label_pair_);
}

void GaugeDynamicInfo::publish(const GaugeValue& value) {
    report_gauge_->set_value(static_cast< double >(value.get()));
}

void GaugeDynamicInfo::unregister(const GaugeStaticInfo& static_info) {
    MetricsFarm::get_reporter().remove_gauge(static_info.name_, report_gauge_);
}

// ─── HistogramStaticInfo / HistogramDynamicInfo ────────────────────────────────

HistogramStaticInfo::HistogramStaticInfo(const std::string& name, const std::string& desc,
                                         const std::string& report_name, const MetricLabel& label_pair) :
        name_{report_name.empty() ? name : report_name}, desc_{desc} {
    if (!label_pair.first.empty() && !label_pair.second.empty()) { label_pair_ = label_pair; }
}

HistogramDynamicInfo::HistogramDynamicInfo(const HistogramStaticInfo& static_info, const std::string& instance_name,
                                           PublishAs ptype) {
    if (ptype == PublishAs::Histogram) {
        report_histogram_gauge_ = MetricsFarm::get_reporter().add_histogram(
            static_info.name_, static_info.desc_, instance_name, static_info.label_pair_);
    } else {
        report_histogram_gauge_ = MetricsFarm::get_reporter().add_gauge(static_info.name_, static_info.desc_,
                                                                        instance_name, static_info.label_pair_);
    }
}

void HistogramDynamicInfo::publish(const folly::TDigest& digest) {
    if (is_histogram_reporter()) {
        const ReportHistogram::Summary s{
            .count = digest.count(),
            .sum = digest.sum(),
            .p50 = digest.estimateQuantile(0.50),
            .p95 = digest.estimateQuantile(0.95),
            .p99 = digest.estimateQuantile(0.99),
            .p999 = digest.estimateQuantile(0.999),
        };
        as_histogram()->set_value(s);
    } else {
        as_gauge()->set_value(digest.count() > 0 ? digest.mean() : 0.0);
    }
}

void HistogramDynamicInfo::unregister(const HistogramStaticInfo& static_info) {
    if (is_histogram_reporter()) {
        MetricsFarm::get_reporter().remove_histogram(static_info.name_, as_histogram());
    } else {
        MetricsFarm::get_reporter().remove_gauge(static_info.name_, as_gauge());
    }
}

} // namespace sisl
