/*********************************************************************************
 *
 * Author/Developer(s): Brian Szmyd, Harihara Kadayam
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

#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/tuple/rem.hpp>
#include <boost/preprocessor/tuple/remove.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>
#include <cxxopts.hpp>

namespace sisl {
namespace options {

using shared_opt = std::shared_ptr< cxxopts::Options >;
using shared_opt_res = std::shared_ptr< cxxopts::ParseResult >;

// C++17 inline variables: one instance per executable across all TUs.
// No weak-symbol tricks or per-TU static storage needed.
inline shared_opt g_options;
inline shared_opt_res g_results;
inline std::vector< std::function< void(cxxopts::Options&) > > g_pending;

inline shared_opt GetOptions() { return g_options; }
inline shared_opt_res GetResults() { return g_results; }

// SislOption registers one option with cxxopts.  If SISL_OPTIONS_LOAD has not
// been called yet (g_options is null), the registration is deferred until it is.
struct SislOption {
    template < class... Args >
    explicit SislOption(std::string const& group, Args... args) {
        if (g_options) {
            g_options->add_option(group, args...);
        } else {
            // C++20 pack init-capture — deferred until SISL_OPTIONS_LOAD creates Options.
            g_pending.push_back([group, ...args = args](cxxopts::Options& o) { o.add_option(group, args...); });
        }
    }
};

} // namespace options
} // namespace sisl

// ─── Public macros ───────────────────────────────────────────────────────────

// SISL_OPTION_GROUP — define a group of options in any translation unit.
//
//   SISL_OPTION_GROUP(logging,
//       (verbosity, "v", "verbosity", "Log level", ::cxxopts::value<std::string>()->default_value("info"), "level"),
//       ...)
//
// Options auto-register on SISL_OPTIONS_LOAD. No SISL_OPTIONS_ENABLE or group
// listing in SISL_OPTIONS_LOAD is required.
#define SISL_OPTION(r, group, args)                                                                                    \
    inline sisl::options::SislOption const BOOST_PP_CAT(_option_, BOOST_PP_TUPLE_ELEM(0, args)){                       \
        BOOST_PP_STRINGIZE(group), BOOST_PP_TUPLE_REM_CTOR(BOOST_PP_TUPLE_REMOVE(args, 0))};

#define SISL_OPTION_GROUP(group, ...)                                                                                  \
    namespace sisl_opts_##group {                                                                                      \
    BOOST_PP_SEQ_FOR_EACH(SISL_OPTION, (group), BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))                                 \
    }

#define SISL_OPTIONS (*sisl::options::g_results)
#define SISL_PARSER (*sisl::options::g_options)

// SISL_OPTIONS_LOAD — create the Options object, drain all pending registrations,
// then parse argc/argv.  No group list needed.
#define SISL_OPTIONS_LOAD(argc, argv)                                                                                  \
    sisl::options::g_options = std::make_shared< cxxopts::Options >(argv[0]);                                          \
    for (auto& _fn : sisl::options::g_pending) {                                                                       \
        _fn(*sisl::options::g_options);                                                                                \
    }                                                                                                                  \
    sisl::options::g_pending.clear();                                                                                  \
    sisl::options::g_results =                                                                                         \
        std::make_shared< cxxopts::ParseResult >(sisl::options::g_options->parse(argc, argv));                         \
    if (sisl::options::g_results->count("help")) {                                                                     \
        std::cout << sisl::options::g_options->help() << std::endl;                                                    \
        exit(0);                                                                                                       \
    }
