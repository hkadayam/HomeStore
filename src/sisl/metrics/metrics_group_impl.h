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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include "common/defs.h"

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <folly/stats/TDigest.h>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include "reporter.h"

namespace sisl {

using OnGatherCb = std::function< void(void) >;

enum class GroupImplType : uint8_t {
    Rcu,
    Atomic,
};

enum class PublishAs : uint8_t {
    Counter,
    Gauge,
    Histogram,
};

/****************************** Counter ************************************/
class CounterValue {
public:
    CounterValue() = default;
    CounterValue(const CounterValue&) = default;
    CounterValue(CounterValue&&) noexcept = default;
    CounterValue& operator=(const CounterValue&) = delete;
    CounterValue& operator=(CounterValue&&) noexcept = delete;

    void increment(const int64_t value = 1) { value_ += value; }
    void decrement(const int64_t value = 1) { value_ -= value; }
    [[nodiscard]] int64_t get() const { return value_; }

private:
    int64_t value_{0};
};

class CounterStaticInfo {
    friend class CounterDynamicInfo;

public:
    CounterStaticInfo(const std::string& name, const std::string& desc, const std::string& report_name = "",
                      const MetricLabel& label_pair = {"", ""});

    CounterStaticInfo(const CounterStaticInfo&) = default;
    CounterStaticInfo(CounterStaticInfo&&) noexcept = default;
    CounterStaticInfo& operator=(const CounterStaticInfo&) = delete;
    CounterStaticInfo& operator=(CounterStaticInfo&&) noexcept = delete;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& desc() const { return desc_; }
    [[nodiscard]] std::string label_str() const {
        return (!label_pair_.first.empty() && !label_pair_.second.empty())
            ? label_pair_.first + "-" + label_pair_.second
            : "";
    }

private:
    std::string name_;
    std::string desc_;
    MetricLabel label_pair_;
};

class CounterDynamicInfo {
public:
    CounterDynamicInfo(const CounterStaticInfo& static_info, const std::string& instance_name,
                       PublishAs ptype = PublishAs::Counter);

    CounterDynamicInfo(const CounterDynamicInfo&) = default;
    CounterDynamicInfo(CounterDynamicInfo&&) noexcept = default;
    CounterDynamicInfo& operator=(const CounterDynamicInfo&) = delete;
    CounterDynamicInfo& operator=(CounterDynamicInfo&&) noexcept = delete;

    void publish(const CounterValue& value);
    void unregister(const CounterStaticInfo& static_info);

private:
    [[nodiscard]] bool is_counter_reporter() const {
        return std::holds_alternative< shared< ReportCounter > >(report_counter_gauge_);
    }

    shared< ReportCounter >& as_counter() {
        return std::get< shared< ReportCounter > >(report_counter_gauge_);
    }
    shared< ReportGauge >& as_gauge() {
        return std::get< shared< ReportGauge > >(report_counter_gauge_);
    }

    std::variant< shared< ReportCounter >, shared< ReportGauge > > report_counter_gauge_;
};

/****************************** Gauge ************************************/
class GaugeValue {
public:
    GaugeValue() : value_{0} {}
    GaugeValue(const std::atomic< int64_t >& oval) : value_{oval.load(std::memory_order_relaxed)} {}
    GaugeValue(const GaugeValue& other) : value_{other.get()} {}
    GaugeValue& operator=(const GaugeValue& rhs) {
        value_.store(rhs.get(), std::memory_order_relaxed);
        return *this;
    }
    GaugeValue(GaugeValue&& other) noexcept : value_{other.get()} {}
    GaugeValue& operator=(GaugeValue&& rhs) noexcept {
        value_.store(rhs.get(), std::memory_order_relaxed);
        return *this;
    }

    void update(const int64_t value) { value_.store(value, std::memory_order_relaxed); }
    [[nodiscard]] int64_t get() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic< int64_t > value_;
};

class GaugeStaticInfo {
    friend class GaugeDynamicInfo;

public:
    GaugeStaticInfo(const std::string& name, const std::string& desc, const std::string& report_name = "",
                    const MetricLabel& label_pair = {"", ""});

    GaugeStaticInfo(const GaugeStaticInfo&) = default;
    GaugeStaticInfo(GaugeStaticInfo&&) noexcept = default;
    GaugeStaticInfo& operator=(const GaugeStaticInfo&) = delete;
    GaugeStaticInfo& operator=(GaugeStaticInfo&&) noexcept = delete;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& desc() const { return desc_; }

private:
    const std::string name_;
    const std::string desc_;
    MetricLabel label_pair_;
};

class GaugeDynamicInfo {
public:
    GaugeDynamicInfo(const GaugeStaticInfo& static_info, const std::string& instance_name);

    GaugeDynamicInfo(const GaugeDynamicInfo&) = default;
    GaugeDynamicInfo(GaugeDynamicInfo&&) noexcept = default;
    GaugeDynamicInfo& operator=(const GaugeDynamicInfo&) = delete;
    GaugeDynamicInfo& operator=(GaugeDynamicInfo&&) noexcept = delete;

    void publish(const GaugeValue& value);
    void unregister(const GaugeStaticInfo& static_info);

private:
    shared< ReportGauge > report_gauge_;
};

/****************************** Histogram — TDigest based ************************************/
class HistogramStaticInfo {
    friend class HistogramDynamicInfo;

public:
    HistogramStaticInfo(const std::string& name, const std::string& desc, const std::string& report_name = "",
                        const MetricLabel& label_pair = {"", ""});

    HistogramStaticInfo(const HistogramStaticInfo&) = default;
    HistogramStaticInfo(HistogramStaticInfo&&) noexcept = default;
    HistogramStaticInfo& operator=(const HistogramStaticInfo&) = delete;
    HistogramStaticInfo& operator=(HistogramStaticInfo&&) noexcept = delete;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& desc() const { return desc_; }
    [[nodiscard]] std::string label_str() const {
        return (!label_pair_.first.empty() && !label_pair_.second.empty())
            ? label_pair_.first + "-" + label_pair_.second
            : "";
    }

private:
    const std::string name_;
    const std::string desc_;
    MetricLabel label_pair_;
};

class HistogramDynamicInfo {
    friend class MetricsGroupImpl;

public:
    HistogramDynamicInfo(const HistogramStaticInfo& static_info, const std::string& instance_name,
                         PublishAs ptype = PublishAs::Histogram);

    HistogramDynamicInfo(const HistogramDynamicInfo&) = default;
    HistogramDynamicInfo(HistogramDynamicInfo&&) noexcept = default;
    HistogramDynamicInfo& operator=(const HistogramDynamicInfo&) = delete;
    HistogramDynamicInfo& operator=(HistogramDynamicInfo&&) noexcept = delete;

    void publish(const folly::TDigest& digest);
    void unregister(const HistogramStaticInfo& static_info);

    [[nodiscard]] bool is_histogram_reporter() const {
        return std::holds_alternative< shared< ReportHistogram > >(report_histogram_gauge_);
    }

private:
    shared< ReportHistogram >& as_histogram() {
        return std::get< shared< ReportHistogram > >(report_histogram_gauge_);
    }
    shared< ReportGauge >& as_gauge() {
        return std::get< shared< ReportGauge > >(report_histogram_gauge_);
    }

    std::variant< shared< ReportHistogram >, shared< ReportGauge > > report_histogram_gauge_;
};

/****************************** MetricsGroupStaticInfo ************************************/
class MetricsGroupImpl;
using MetricsGroupImplPtr = shared< MetricsGroupImpl >;

// Shared per group name (not per instance) — stores names/descs registered once.
class MetricsGroupStaticInfo {
    friend class MetricsGroupImpl;
    friend class MetricsGroup;

public:
    MetricsGroupStaticInfo() = default;
    MetricsGroupStaticInfo(const std::string& grp_name);

    MetricsGroupStaticInfo(const MetricsGroupStaticInfo&) = delete;
    MetricsGroupStaticInfo(MetricsGroupStaticInfo&&) noexcept = delete;
    MetricsGroupStaticInfo& operator=(const MetricsGroupStaticInfo&) = delete;
    MetricsGroupStaticInfo& operator=(MetricsGroupStaticInfo&&) noexcept = delete;

    static shared< MetricsGroupStaticInfo > create_or_get_info(const std::string& grp_name);

    uint64_t register_counter(const std::string& name, const std::string& desc,
                              const std::string& report_name = "", const MetricLabel& label_pair = {"", ""});
    uint64_t register_gauge(const std::string& name, const std::string& desc,
                            const std::string& report_name = "", const MetricLabel& label_pair = {"", ""});
    uint64_t register_histogram(const std::string& name, const std::string& desc,
                                const std::string& report_name = "", const MetricLabel& label_pair = {"", ""});

public:
    std::string grp_name_;
    std::mutex mutex_;
    std::vector< CounterStaticInfo > counters_;
    std::vector< GaugeStaticInfo > gauges_;
    std::vector< HistogramStaticInfo > histograms_;
    bool reg_pending_{true};
};

/****************************** Gather callbacks ************************************/
using CounterGatherCb = std::function< void(uint64_t, const CounterValue&) >;
using GaugeGatherCb = std::function< void(uint64_t, const GaugeValue&) >;
using HistogramGatherCb = std::function< void(uint64_t, const folly::TDigest&) >;

/****************************** MetricsGroupImpl ************************************/
class MetricsGroupImpl {
    [[nodiscard]] auto lock() { return std::lock_guard< decltype(mutex_) >(mutex_); }

public:
    MetricsGroupImpl(const std::string& grp_name, const std::string& inst_name);
    virtual ~MetricsGroupImpl();

    MetricsGroupImpl(const MetricsGroupImpl&) = delete;
    MetricsGroupImpl(MetricsGroupImpl&&) noexcept = delete;
    MetricsGroupImpl& operator=(const MetricsGroupImpl&) = delete;
    MetricsGroupImpl& operator=(MetricsGroupImpl&&) noexcept = delete;

    void registration_completed();

    /* Counter */
    uint64_t register_counter(const std::string& name, const std::string& desc, const std::string& report_name = "",
                              const MetricLabel& label_pair = {"", ""},
                              PublishAs ptype = PublishAs::Counter);
    uint64_t register_counter(const std::string& name, const std::string& desc, const MetricLabel& label_pair,
                              PublishAs ptype = PublishAs::Counter);
    uint64_t register_counter(const std::string& name, const std::string& desc, PublishAs ptype);

    /* Gauge */
    uint64_t register_gauge(const std::string& name, const std::string& desc, const std::string& report_name = "",
                            const MetricLabel& label_pair = {"", ""});
    uint64_t register_gauge(const std::string& name, const std::string& desc, const MetricLabel& label_pair);

    /* Histogram — no bucket boundaries needed */
    uint64_t register_histogram(const std::string& name, const std::string& desc, const std::string& report_name = "",
                                const MetricLabel& label_pair = {"", ""},
                                PublishAs ptype = PublishAs::Histogram);
    uint64_t register_histogram(const std::string& name, const std::string& desc, const MetricLabel& label_pair,
                                PublishAs ptype = PublishAs::Histogram);
    uint64_t register_histogram(const std::string& name, const std::string& desc, PublishAs ptype);

    virtual void counter_increment(uint64_t index, int64_t val = 1) = 0;
    virtual void counter_decrement(uint64_t index, int64_t val = 1) = 0;

    void gauge_update(uint64_t index, int64_t val);

    virtual void histogram_observe(uint64_t index, int64_t val) = 0;
    virtual void histogram_observe(uint64_t index, int64_t val, uint64_t count) = 0;

    nlohmann::json get_result_in_json(bool need_latest);
    [[nodiscard]] const std::string& get_group_name() const;
    [[nodiscard]] const std::string& get_instance_name() const;

    void publish_result();
    void gather();

    [[nodiscard]] virtual const CounterStaticInfo& counter_static_info(uint64_t idx) const {
        return static_info_->counters_[idx];
    }
    virtual CounterDynamicInfo& counter_dynamic_info(uint64_t idx) { return counters_dinfo_[idx]; }

    [[nodiscard]] virtual const GaugeStaticInfo& gauge_static_info(uint64_t idx) const {
        return static_info_->gauges_[idx];
    }
    virtual GaugeDynamicInfo& gauge_dynamic_info(uint64_t idx) { return gauges_dinfo_[idx]; }

    [[nodiscard]] virtual const HistogramStaticInfo& hist_static_info(uint64_t idx) const {
        return static_info_->histograms_[idx];
    }
    virtual HistogramDynamicInfo& hist_dynamic_info(uint64_t idx) { return histograms_dinfo_[idx]; }

    [[nodiscard]] virtual uint64_t num_counters() const { return counters_dinfo_.size(); }
    [[nodiscard]] virtual uint64_t num_gauges() const { return gauges_dinfo_.size(); }
    [[nodiscard]] virtual uint64_t num_histograms() const { return histograms_dinfo_.size(); }

    void attach_gather_cb(const OnGatherCb& cb) {
        auto locked{lock()};
        on_gather_cb_ = cb;
    }
    void detach_gather_cb() {
        auto locked{lock()};
        on_gather_cb_ = nullptr;
    }

    void add_child_group(const MetricsGroupImplPtr& child_grp) {
        auto locked{lock()};
        child_groups_.push_back(child_grp);
    }

    [[nodiscard]] std::string instance_name() const { return inst_name_; }
    [[nodiscard]] virtual GroupImplType impl_type() const = 0;
    virtual void on_register() = 0;

protected:
    virtual void gather_result(bool need_latest, const CounterGatherCb& counter_cb, const GaugeGatherCb& gauge_cb,
                               const HistogramGatherCb& histogram_cb) = 0;

protected:
    std::string inst_name_;
    std::mutex mutex_;
    OnGatherCb on_gather_cb_ = nullptr;
    shared< MetricsGroupStaticInfo > static_info_;

    std::vector< CounterDynamicInfo > counters_dinfo_;
    std::vector< GaugeDynamicInfo > gauges_dinfo_;
    std::vector< HistogramDynamicInfo > histograms_dinfo_;
    std::vector< GaugeValue > gauge_values_;
    std::vector< MetricsGroupImplPtr > child_groups_;
};

} // namespace sisl
