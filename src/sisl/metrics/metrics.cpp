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
#include "sisl/logging/logging.h"
#include "sisl/metrics/metrics.h"

namespace sisl {

// ─── NoOpReporter ─────────────────────────────────────────────────────────────
//
// Placeholder until an OpenTelemetry OTLP reporter is wired in.
// The get_result_in_json() path works without any reporter — it aggregates
// directly via gather_result() callbacks.  publish_result() / report() call
// the reporter, which here does nothing.
//
// TODO: replace with OtelReporter that exports via OTLP.

class NoOpReportCounter : public ReportCounter {
public:
    void set_value(double) override {}
};

class NoOpReportGauge : public ReportGauge {
public:
    void set_value(double) override {}
};

class NoOpReportHistogram : public ReportHistogram {
public:
    void set_value(const Summary&) override {}
};

class NoOpReporter : public Reporter {
public:
    shared< ReportCounter > add_counter(const std::string&, const std::string&, const std::string&,
                                        const MetricLabel&) override {
        return std::make_shared< NoOpReportCounter >();
    }
    shared< ReportGauge > add_gauge(const std::string&, const std::string&, const std::string&,
                                    const MetricLabel&) override {
        return std::make_shared< NoOpReportGauge >();
    }
    shared< ReportHistogram > add_histogram(const std::string&, const std::string&, const std::string&,
                                            const MetricLabel&) override {
        return std::make_shared< NoOpReportHistogram >();
    }
    void remove_counter(const std::string&, const shared< ReportCounter >&) override {}
    void remove_gauge(const std::string&, const shared< ReportGauge >&) override {}
    void remove_histogram(const std::string&, const shared< ReportHistogram >&) override {}
    std::string serialize(ReportFormat) override { return {}; }
};

// ─── MetricsFarm ──────────────────────────────────────────────────────────────

static std::atomic< bool > metrics_farm_initialized{false};

Reporter& MetricsFarm::get_reporter() { return *getInstance().reporter_; }

MetricsFarm::MetricsFarm() {
    metrics_farm_initialized = true;
    reporter_ = std::make_unique< NoOpReporter >();
}

MetricsFarm::~MetricsFarm() { metrics_farm_initialized = false; }

bool MetricsFarm::is_initialized() { return metrics_farm_initialized.load(); }

void MetricsFarm::register_metrics_group(MetricsGroupImplPtr mgrp_impl, const bool add_to_farm_list) {
    assert(mgrp_impl != nullptr);
    auto locked{lock()};
    mgrp_impl->on_register();
    if (add_to_farm_list) { mgroups_.insert(mgrp_impl); }
    mgrp_impl->registration_completed();
}

void MetricsFarm::deregister_metrics_group(MetricsGroupImplPtr mgrp_impl) {
    assert(mgrp_impl != nullptr);
    auto locked{lock()};
    mgroups_.erase(mgrp_impl);
}

nlohmann::json MetricsFarm::get_result_in_json(bool need_latest) {
    nlohmann::json json;
    auto locked{lock()};
    for (auto& mgroup : mgroups_) {
        json[mgroup->get_group_name()][mgroup->get_instance_name()] = mgroup->get_result_in_json(need_latest);
    }
    return json;
}

std::string MetricsFarm::get_result_in_json_string(bool need_latest) { return get_result_in_json(need_latest).dump(); }

std::string MetricsFarm::report(ReportFormat format) {
    auto locked{lock()};
    for (auto& mgroup : mgroups_) {
        mgroup->publish_result();
    }
    return reporter_->serialize(format);
}

void MetricsFarm::gather() {
    auto locked{lock()};
    for (auto& mgroup : mgroups_) {
        mgroup->gather();
    }
}

std::string MetricsFarm::ensure_unique(const std::string& grp_name, const std::string& inst_name) {
    auto locked{lock()};
    const auto it{uniq_inst_maintainer_.find(grp_name + inst_name)};
    if (it == std::end(uniq_inst_maintainer_)) {
        uniq_inst_maintainer_.insert({grp_name + inst_name, 1});
        return inst_name;
    } else {
        ++(it->second);
        return inst_name + "_" + std::to_string(it->second);
    }
}

} // namespace sisl
