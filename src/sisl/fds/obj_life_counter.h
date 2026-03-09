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
#include <typeinfo>
#include <iostream>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cstring>
#if defined(__linux__) || defined(__APPLE__)
#include <cxxabi.h>
#endif
#include <sisl/metrics/metrics.h>

namespace sisl {

class ObjCounterMetrics;
class ObjCounterRegistry {
public:
    using pair_of_atomic_ptrs = std::pair< std::atomic< int64_t >*, std::atomic< int64_t >* >;

private:
    std::unordered_map< std::string, pair_of_atomic_ptrs > tracker_map_;
    std::unique_ptr< ObjCounterMetrics > metrics_;

public:
    static ObjCounterRegistry& inst() {
        static ObjCounterRegistry instance;
        return instance;
    }

    static decltype(tracker_map_)& tracker() { return inst().tracker_map_; }

    static void register_obj(const char* name, pair_of_atomic_ptrs ptrs) { tracker()[std::string(name)] = ptrs; }

    static void foreach (const std::function< void(const std::string&, int64_t, int64_t) >& closure) {
        for (auto& e : ObjCounterRegistry::tracker()) {
            closure(e.first, e.second.first->load(std::memory_order_acquire),
                    e.second.second->load(std::memory_order_acquire));
        }
    }

    static ObjCounterMetrics* metrics() { return inst().metrics_.get(); }
    static inline void enable_metrics_reporting();
};

class ObjCounterMetrics : public MetricsGroup {
public:
    ObjCounterMetrics(const std::vector< std::string >& v) : MetricsGroup("ObjectLife", "Singleton") {
        for (const auto& name : v) {
            std::string prom_name{name};
            std::transform(prom_name.begin(), prom_name.end(), prom_name.begin(), [](unsigned char c) -> unsigned char {
                if (c == '<' || c == '>' || c == ',' || c == '(' || c == ')' || c == ' ') {
                    return '_';
                } else if (c == '*') {
                    return 'P';
                } else {
                    return c;
                }
            });
            const auto idx{
                this->impl_ptr_->register_gauge(prom_name, prom_name + " created", prom_name, {"type", "created"})};
            const auto nidx{
                this->impl_ptr_->register_gauge(prom_name, prom_name + " alive", prom_name, {"type", "alive"})};
            assert(nidx == idx + 1);
            name_gauge_map_.emplace(name, std::make_pair(idx, nidx));
        }
        register_me_to_farm();
        attach_gather_cb(std::bind(&ObjCounterMetrics::on_gather, this));
    }
    ~ObjCounterMetrics() { deregister_me_from_farm(); }

    void on_gather() {
        ObjCounterRegistry::foreach ([this](const std::string& name, const int64_t created, const int64_t alive) {
            const auto it{name_gauge_map_.find(name)};
            if (it != name_gauge_map_.cend()) {
                const auto [create_idx, alive_idx] = it->second;
                this->impl_ptr_->gauge_update(create_idx, created);
                this->impl_ptr_->gauge_update(alive_idx, alive);
            }
        });
    }

private:
    std::unordered_map< std::string, std::pair< uint64_t, uint64_t > > name_gauge_map_;
};

inline void ObjCounterRegistry::enable_metrics_reporting() {
    auto& t{tracker()};
    std::vector< std::string > v;
    std::transform(t.begin(), t.end(), std::back_inserter(v), [](const auto& p) { return p.first; });
    inst().metrics_ = std::make_unique< ObjCounterMetrics >(v);
}

template < typename T >
struct ObjTypeWrapper {
    ObjTypeWrapper(std::atomic< int64_t >* pc, std::atomic< int64_t >* pa) {
        int status{-1};
        char* realname{nullptr};
#if defined(__linux__) || defined(__APPLE__)
        realname = abi::__cxa_demangle(typeid(T).name(), 0, 0, &status);
#endif
        if (status == 0) {
            ObjCounterRegistry::register_obj(realname, std::make_pair(pc, pa));
            std::free(realname);
        } else {
            ObjCounterRegistry::register_obj(typeid(T).name(), std::make_pair(pc, pa));
        }
    }
    int dummy_{0};
};

template < typename T >
struct ObjLifeCounter {
    ObjLifeCounter() {
        s_created.fetch_add(1, std::memory_order_relaxed);
        s_alive.fetch_add(1, std::memory_order_relaxed);
        s_type.dummy_ = 0;
    }

    ~ObjLifeCounter() {
        assert(s_alive.load() > 0);
        s_alive.fetch_sub(1, std::memory_order_relaxed);
    }

    ObjLifeCounter(const ObjLifeCounter&) noexcept { s_alive.fetch_add(1, std::memory_order_relaxed); }
    static std::atomic< int64_t > s_created;
    static std::atomic< int64_t > s_alive;
    static ObjTypeWrapper< T > s_type;
};

template < typename T >
std::atomic< int64_t > ObjLifeCounter< T >::s_created(0);

template < typename T >
std::atomic< int64_t > ObjLifeCounter< T >::s_alive(0);

template < typename T >
ObjTypeWrapper< T > ObjLifeCounter< T >::s_type(&ObjLifeCounter< T >::s_created, &ObjLifeCounter< T >::s_alive);

} // namespace sisl
