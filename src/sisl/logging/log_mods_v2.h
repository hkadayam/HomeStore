/*********************************************************************************
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

#define SPDLOG_FUNCTION __PRETTY_FUNCTION__
#define SPDLOG_NO_NAME

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/spdlog.h>

//
// C++20 structural NTTP for compile-time module names.
//
// ModuleTag<N> holds a null-terminated string of length N (including '\0').
// Because all members are public and of structural type (char array), it
// satisfies the C++20 structural-type requirement and may appear as a
// non-type template parameter.
//
template <std::size_t N>
struct ModuleTag {
    char name[N]{};

    // Implicit construction from a string literal keeps call-sites tidy.
    constexpr ModuleTag(const char (&str)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) name[i] = str[i];
    }

    constexpr std::string_view view() const noexcept { return {name, N - 1}; }
    constexpr bool operator==(const ModuleTag&) const noexcept = default;
};

template <std::size_t N>
ModuleTag(const char (&)[N]) -> ModuleTag<N>;

//
// Public macros
//
// LEVELCHECK(btree, spdlog::level::debug)
//   expands to a plain virtual-function read on a per-module singleton —
//   no mutex, no map lookup after first construction.
//
#define LEVELCHECK(mod, lvl) (ModuleName<ModuleTag{#mod}>::instance().get_level() <= (lvl))


namespace sisl {
namespace logging {

class ModuleBase {
public:
    ModuleBase() = default;
    virtual std::string get_name() const = 0;
    virtual void set_level(spdlog::level::level_enum level) = 0;
    virtual spdlog::level::level_enum get_level() const = 0;
    virtual ~ModuleBase() = default;
};

class LogModulesV2 {
public:
    // Default level applied to every module unless overridden via set_module_level().
    static constexpr spdlog::level::level_enum k_default_level{spdlog::level::level_enum::info};

    static LogModulesV2& instance() {
        static LogModulesV2 s_inst{};
        return s_inst;
    }

    void register_module(ModuleBase* mod) {
        std::unique_lock lock{m_mutex};
        m_registered_modules.emplace(mod->get_name(), mod);

        // If a level was requested before this module was first used, apply it now.
        auto const it = m_requested_modules.find(mod->get_name());
        if (it != m_requested_modules.end()) {
            mod->set_level(it->second);
            m_requested_modules.erase(it);
        } else {
            mod->set_level(k_default_level);
        }
    }

    void set_module_level(const std::string& name, spdlog::level::level_enum level) {
        std::unique_lock lock{m_mutex};
        auto it = m_registered_modules.find(name);
        if (it != m_registered_modules.end()) {
            it->second->set_level(level);
        } else {
            // Module not yet used; stash the request — register_module() will pick it up.
            m_requested_modules[name] = level;
        }
    }

    spdlog::level::level_enum get_module_level(const std::string& module_name) {
        std::unique_lock lock{m_mutex};
        if (auto it = m_registered_modules.find(module_name); it != m_registered_modules.end()) {
            return it->second->get_level();
        }
        if (auto it2 = m_requested_modules.find(module_name); it2 != m_requested_modules.end()) {
            return it2->second;
        }
        return k_default_level;
    }

    std::unordered_map< std::string, spdlog::level::level_enum > get_all_module_levels() {
        std::unique_lock lock{m_mutex};
        std::unordered_map< std::string, spdlog::level::level_enum > ret = m_requested_modules;
        for (auto& [name, mod] : m_registered_modules) {
            ret[name] = mod->get_level();
        }
        return ret;
    }

    void set_all_module_levels(spdlog::level::level_enum level) {
        std::unique_lock lock{m_mutex};
        for (auto& [name, mod] : m_registered_modules) {
            mod->set_level(level);
        }
        for (auto& [name, lvl] : m_requested_modules) {
            lvl = level;
        }
    }

private:
    LogModulesV2() = default;

    std::mutex m_mutex;
    std::unordered_map< std::string, ModuleBase* > m_registered_modules;
    std::unordered_map< std::string, spdlog::level::level_enum > m_requested_modules;
};

} // namespace logging
} // namespace sisl

//
// Per-module singleton.
//
// One instantiation exists per distinct ModuleTag value (i.e. per module name).
// The constructor auto-registers with LogModulesV2 the first time instance() is
// called, which happens on the first LEVELCHECK for that module — zero overhead
// on every subsequent call (the static local is already constructed).
//
template <auto tag>
class ModuleName : public sisl::logging::ModuleBase {
public:
    ModuleName(const ModuleName&) = delete;
    ModuleName(ModuleName&&) noexcept = delete;
    ModuleName& operator=(const ModuleName&) = delete;
    ModuleName& operator=(ModuleName&&) noexcept = delete;

    static ModuleName& instance() {
        static ModuleName inst{};
        return inst;
    }

    spdlog::level::level_enum get_level() const override { return m_level; }
    void set_level(spdlog::level::level_enum level) override { m_level = level; }
    std::string get_name() const override { return std::string{tag.view()}; }

private:
    ModuleName() {
        // Auto-register on first use.  LogModulesV2 may already have a pending
        // level request (from set_module_level() called before any log); if so,
        // register_module() applies it immediately.
        sisl::logging::LogModulesV2::instance().register_module(this);
    }

    // Starts at 'off' so that register_module() always drives the initial level
    // (either k_default_level or a pre-requested level).
    spdlog::level::level_enum m_level{spdlog::level::level_enum::off};
};
