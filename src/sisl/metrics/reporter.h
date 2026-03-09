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

#include <memory>
#include <string>

#include "common/defs.h"

namespace sisl {

using MetricLabel = std::pair< std::string, std::string >;

enum class ReportFormat { Unknown, Text, Json, ProtoBuffer };

class ReportCounter {
public:
    virtual void set_value(double value) = 0;
    virtual ~ReportCounter() = default;
};

class ReportGauge {
public:
    virtual void set_value(double value) = 0;
    virtual ~ReportGauge() = default;
};

class ReportHistogram {
public:
    // Computed quantile summary from TDigest — no predefined buckets needed.
    struct Summary {
        double count{0};
        double sum{0};
        double p50{0};
        double p95{0};
        double p99{0};
        double p999{0};
    };
    virtual void set_value(const Summary& summary) = 0;
    virtual ~ReportHistogram() = default;
};

class Reporter {
public:
    virtual ~Reporter() = default;
    virtual shared< ReportCounter > add_counter(const std::string& name, const std::string& desc,
                                                const std::string& instance_name,
                                                const MetricLabel& label_pair = {"", ""}) = 0;
    virtual shared< ReportGauge > add_gauge(const std::string& name, const std::string& desc,
                                            const std::string& instance_name,
                                            const MetricLabel& label_pair = {"", ""}) = 0;
    // No bucket boundaries — TDigest computes quantiles adaptively.
    virtual shared< ReportHistogram > add_histogram(const std::string& name, const std::string& desc,
                                                    const std::string& instance_name,
                                                    const MetricLabel& label_pair = {"", ""}) = 0;

    virtual void remove_counter(const std::string& name, const shared< ReportCounter >& rc) = 0;
    virtual void remove_gauge(const std::string& name, const shared< ReportGauge >& rg) = 0;
    virtual void remove_histogram(const std::string& name, const shared< ReportHistogram >& rh) = 0;

    virtual std::string serialize(ReportFormat format) = 0;
};

} // namespace sisl
