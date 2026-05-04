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
#include <csignal>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "flip.h"

namespace flip {

// Convenience builders for the most common FlipSpec shapes.  All inject_*_flip overloads construct a FlipSpecT in
// place (no protobuf, no flatbuffer serialization — runtime types only) and hand it to Flip::add().
class FlipClient {
public:
    FlipClient() = default;

    static FlipClient& instance() {
        static FlipClient s_instance;
        return s_instance;
    }

    template < typename T >
    void create_condition(const std::string& param_name, Operator oper, const T& value, FlipConditionT* out) {
        out->name = param_name;
        out->oper = oper;
        out->value = std::make_unique< ParamValueT >();
        to_param_converter< T >()(value, *out->value);
    }

    template < typename T >
    FlipConditionT create_condition(const std::string& param_name, Operator oper, const T& value) {
        FlipConditionT c;
        create_condition(param_name, oper, value, &c);
        return c;
    }

    bool inject_noreturn_flip(std::string flip_name, const std::vector< FlipConditionT >& conditions,
                              const FlipFrequencyT& freq) {
        FlipSpecT fspec;
        _create_flip_spec(std::move(flip_name), conditions, freq, fspec);
        fspec.flip_action = std::make_unique< FlipActionT >();
        fspec.flip_action->action.Set(NoActionT{});
        Flip::instance().add(std::move(fspec));
        return true;
    }

    template < typename T >
    bool inject_retval_flip(std::string flip_name, const std::vector< FlipConditionT >& conditions,
                            const FlipFrequencyT& freq, const T& retval) {
        FlipSpecT fspec;
        _create_flip_spec(std::move(flip_name), conditions, freq, fspec);
        fspec.flip_action = std::make_unique< FlipActionT >();
        ActionReturnsT ar;
        ar.retval = std::make_unique< ParamValueT >();
        to_param_converter< T >()(retval, *ar.retval);
        fspec.flip_action->action.Set(std::move(ar));
        Flip::instance().add(std::move(fspec));
        return true;
    }

    bool inject_delay_flip(std::string flip_name, const std::vector< FlipConditionT >& conditions,
                           const FlipFrequencyT& freq, uint64_t delay_usec) {
        FlipSpecT fspec;
        _create_flip_spec(std::move(flip_name), conditions, freq, fspec);
        fspec.flip_action = std::make_unique< FlipActionT >();
        ActionDelaysT ad;
        ad.delay_in_usec = delay_usec;
        fspec.flip_action->action.Set(std::move(ad));
        Flip::instance().add(std::move(fspec));
        return true;
    }

    template < typename T >
    bool inject_delay_and_retval_flip(std::string flip_name, const std::vector< FlipConditionT >& conditions,
                                      const FlipFrequencyT& freq, uint64_t delay_usec, const T& retval) {
        FlipSpecT fspec;
        _create_flip_spec(std::move(flip_name), conditions, freq, fspec);
        fspec.flip_action = std::make_unique< FlipActionT >();
        ActionDelayedReturnsT adr;
        adr.delay_in_usec = delay_usec;
        adr.retval = std::make_unique< ParamValueT >();
        to_param_converter< T >()(retval, *adr.retval);
        fspec.flip_action->action.Set(std::move(adr));
        Flip::instance().add(std::move(fspec));
        return true;
    }

    uint32_t remove_flip(const std::string& flip_name) { return Flip::instance().remove(flip_name); }

    static void test_and_abort(const std::string& flip_name) {
        if (Flip::instance().test_flip(flip_name)) { std::raise(SIGKILL); }
    }

private:
    void _create_flip_spec(std::string flip_name, const std::vector< FlipConditionT >& conditions,
                           const FlipFrequencyT& freq, FlipSpecT& out) {
        out.flip_name = std::move(flip_name);
        out.conditions.reserve(conditions.size());
        for (auto const& c : conditions) {
            // FlipConditionT contains a unique_ptr<ParamValueT> — deep-copy via the type's copy ctor (object-API
            // generates copy constructors with --gen-object-api).
            out.conditions.emplace_back(std::make_unique< FlipConditionT >(c));
        }
        out.flip_frequency = std::make_unique< FlipFrequencyT >(freq);
    }
};

} // namespace flip