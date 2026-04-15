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
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <sisl/logging/logging.h>

#include "metrics_atomic.h"
#include "metrics_folly.h"
#include "metrics_group_impl.h"

namespace sisl {

class MetricsGroupStaticInfo;
class MetricsFarm;

class MetricsGroup {
public:
    MetricsGroup(const std::string& grp_name, const std::string& inst_name = "Instance1",
                 GroupImplType type = GroupImplType::Rcu);
    ~MetricsGroup();

    MetricsGroup(const MetricsGroup&) = delete;
    MetricsGroup(MetricsGroup&&) noexcept = delete;
    MetricsGroup& operator=(const MetricsGroup&) = delete;
    MetricsGroup& operator=(MetricsGroup&&) noexcept = delete;

    MetricsGroupImplPtr impl_ptr_;
    shared< MetricsFarm > farm_ptr_; // Keep ref to prevent singleton destruction before us.

    std::atomic< bool > is_registered_{false};
    static MetricsGroupImplPtr make_group(const std::string& grp_name, const std::string& inst_name,
                                          GroupImplType type = GroupImplType::Rcu);

    void register_me_to_farm();
    void deregister_me_from_farm();
    void register_me_to_parent(MetricsGroup* parent);

    nlohmann::json get_result_in_json(bool need_latest);
    void gather();
    void attach_gather_cb(const OnGatherCb& cb);
    void detach_gather_cb();

    [[nodiscard]] std::string instance_name() const { return impl_ptr_->instance_name(); }
};

class MetricsFarm {
private:
    std::set< MetricsGroupImplPtr > mgroups_;
    std::unordered_map< std::string, uint64_t > uniq_inst_maintainer_;
    mutable std::mutex lock_;
    unique< Reporter > reporter_;

    MetricsFarm();
    [[nodiscard]] auto lock() const { return std::lock_guard< decltype(lock_) >(lock_); }

public:
    ~MetricsFarm();
    MetricsFarm(const MetricsFarm&) = delete;
    MetricsFarm(MetricsFarm&&) noexcept = delete;
    MetricsFarm& operator=(const MetricsFarm&) = delete;
    MetricsFarm& operator=(MetricsFarm&&) noexcept = delete;

    static MetricsFarm& getInstance() { return *get_instance_ptr(); }

    static shared< MetricsFarm > get_instance_ptr() {
        static shared< MetricsFarm > inst_ptr{new MetricsFarm()};
        return inst_ptr;
    }

    static Reporter& get_reporter();
    static bool is_initialized();

    void register_metrics_group(MetricsGroupImplPtr mgroup, bool add_to_farm_list = true);
    void deregister_metrics_group(MetricsGroupImplPtr mgroup);

    nlohmann::json get_result_in_json(bool need_latest = true);
    std::string get_result_in_json_string(bool need_latest = true);
    std::string report(ReportFormat format);
    void gather();

    std::string ensure_unique(const std::string& grp_name, const std::string& inst_name);
};

using MetricsGroupWrapper = MetricsGroup; // backward compat alias

} // namespace sisl

// ── Convenience macros ────────────────────────────────────────────────
// These expand to calls of the template-based register_counter/gauge/histogram
// functions above.  They must be used inside a MetricsGroup subclass constructor
// where `this` is a MetricsGroup*.
#define REGISTER_COUNTER(name, desc, ...) sisl::register_counter< #name >(*this, desc, ##__VA_ARGS__)
#define REGISTER_GAUGE(name, desc, ...) sisl::register_gauge< #name >(*this, desc, ##__VA_ARGS__)
#define REGISTER_HISTOGRAM(name, desc, ...) sisl::register_histogram< #name >(*this, desc, ##__VA_ARGS__)

#define COUNTER_INCREMENT(grp, name, val) sisl::counter_increment< #name >(grp, val)
#define COUNTER_DECREMENT(grp, name, val) sisl::counter_decrement< #name >(grp, val)
#define COUNTER_INCREMENT_IF_ELSE(grp, cond, name_a, name_b, val)                                                      \
    (cond) ? COUNTER_INCREMENT(grp, name_a, val) : COUNTER_INCREMENT(grp, name_b, val)
#define COUNTER_DECREMENT_IF_ELSE(grp, cond, name_a, name_b, val)                                                      \
    (cond) ? COUNTER_DECREMENT(grp, name_a, val) : COUNTER_DECREMENT(grp, name_b, val)
#define GAUGE_UPDATE(grp, name, val) sisl::gauge_update< #name >(grp, val)
#define HISTOGRAM_OBSERVE(grp, name, val) sisl::histogram_observe< #name >(grp, val)
#define HISTOGRAM_OBSERVE_IF_ELSE(grp, cond, name_a, name_b, val)                                                      \
    (cond) ? HISTOGRAM_OBSERVE(grp, name_a, val) : HISTOGRAM_OBSERVE(grp, name_b, val)

////////////////////////////////////////// MetricTag + template record/register API ///////////////////////////////
//
// C++20 structural NTTP — same pattern as ModuleTag in logging.
// One MetricHandle<tag> singleton per distinct name string, storing the
// integer ID assigned at registration time.
//
// NOTE: IDs are global (not per-group-type).  If two distinct MetricsGroup
// types both register a metric named "foo", they must assign the same index
// (i.e. the same sequential position in their registration list) for the
// singleton to give correct results.  In practice, group types have
// non-overlapping metric sets, so this is not an issue.
//
namespace sisl {

template < std::size_t N >
struct MetricTag {
    char name[N]{};

    constexpr MetricTag(const char (&str)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i)
            name[i] = str[i];
    }
    constexpr std::string_view view() const noexcept { return {name, N - 1}; }
    constexpr bool operator==(const MetricTag&) const noexcept = default;
};

template < std::size_t N >
MetricTag(const char (&)[N]) -> MetricTag< N >;

template < auto Tag >
struct MetricHandle {
    static MetricHandle& instance() {
        static MetricHandle inst{};
        return inst;
    }
    uint64_t id{std::numeric_limits< uint64_t >::max()};
};

// ─── Registration (call once per name in the MetricsGroup subclass ctor) ─────

template < MetricTag Name >
inline uint64_t register_counter(MetricsGroup& grp, const std::string& desc, const std::string& report_name = "",
                                 const MetricLabel& label_pair = {"", ""}, PublishAs ptype = PublishAs::Counter) {
    const auto id = grp.impl_ptr_->register_counter(std::string{Name.view()}, desc, report_name, label_pair, ptype);
    MetricHandle< Name >::instance().id = id;
    return id;
}

template < MetricTag Name >
inline uint64_t register_gauge(MetricsGroup& grp, const std::string& desc, const std::string& report_name = "",
                               const MetricLabel& label_pair = {"", ""}) {
    const auto id = grp.impl_ptr_->register_gauge(std::string{Name.view()}, desc, report_name, label_pair);
    MetricHandle< Name >::instance().id = id;
    return id;
}

template < MetricTag Name >
inline uint64_t register_histogram(MetricsGroup& grp, const std::string& desc, const std::string& report_name = "",
                                   const MetricLabel& label_pair = {"", ""}, PublishAs ptype = PublishAs::Histogram) {
    const auto id = grp.impl_ptr_->register_histogram(std::string{Name.view()}, desc, report_name, label_pair, ptype);
    MetricHandle< Name >::instance().id = id;
    return id;
}

// ─── Record (hot path — call anywhere) ───────────────────────────────────────

template < MetricTag Name >
inline void counter_increment(MetricsGroup& grp, int64_t val = 1) {
    assert(MetricHandle< Name >::instance().id != std::numeric_limits< uint64_t >::max());
    grp.impl_ptr_->counter_increment(MetricHandle< Name >::instance().id, val);
}

template < MetricTag Name >
inline void counter_decrement(MetricsGroup& grp, int64_t val = 1) {
    assert(MetricHandle< Name >::instance().id != std::numeric_limits< uint64_t >::max());
    grp.impl_ptr_->counter_decrement(MetricHandle< Name >::instance().id, val);
}

template < MetricTag Name >
inline void gauge_update(MetricsGroup& grp, int64_t val) {
    assert(MetricHandle< Name >::instance().id != std::numeric_limits< uint64_t >::max());
    grp.impl_ptr_->gauge_update(MetricHandle< Name >::instance().id, val);
}

template < MetricTag Name >
inline void histogram_observe(MetricsGroup& grp, int64_t val) {
    assert(MetricHandle< Name >::instance().id != std::numeric_limits< uint64_t >::max());
    grp.impl_ptr_->histogram_observe(MetricHandle< Name >::instance().id, val);
}

template < MetricTag Name >
inline void histogram_observe(MetricsGroup& grp, int64_t val, uint64_t count) {
    assert(MetricHandle< Name >::instance().id != std::numeric_limits< uint64_t >::max());
    grp.impl_ptr_->histogram_observe(MetricHandle< Name >::instance().id, val, count);
}

// Conditional increment: increment NameA if cond is true, else NameB.
template < MetricTag NameA, MetricTag NameB >
inline void counter_increment_if_else(MetricsGroup& grp, bool cond, int64_t val = 1) {
    if (cond) {
        counter_increment< NameA >(grp, val);
    } else {
        counter_increment< NameB >(grp, val);
    }
}

template < MetricTag NameA, MetricTag NameB >
inline void histogram_observe_if_else(MetricsGroup& grp, bool cond, int64_t val) {
    if (cond) {
        histogram_observe< NameA >(grp, val);
    } else {
        histogram_observe< NameB >(grp, val);
    }
}

} // namespace sisl
