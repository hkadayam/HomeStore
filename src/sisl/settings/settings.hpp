/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Author/Developer(s): Harihara Kadayam, Aditya Marella
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

#include <cassert>
#include <concepts>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <flatbuffers/idl.h>

#include <nlohmann/json.hpp>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <sisl/fds/rcu.h>

#define SETTINGS_INIT(schema_type, schema_name)                                                                        \
    extern unsigned char schema_name##_fbs[];                                                                          \
    extern unsigned int schema_name##_fbs_len;                                                                         \
    class schema_name##_factory : public ::sisl::SettingsFactory< schema_type##T > {                                   \
    public:                                                                                                            \
        static schema_name##_factory& instance() {                                                                     \
            static schema_name##_factory s_instance;                                                                   \
            return s_instance;                                                                                         \
        }                                                                                                              \
                                                                                                                       \
        schema_name##_factory() :                                                                                      \
                ::sisl::SettingsFactory< schema_type##T >{BOOST_PP_STRINGIZE(schema_name), schema_name##_fbs,          \
                                                                             schema_name##_fbs_len} {}                 \
    };

#define SETTINGS_FACTORY(schema_name) schema_name##_factory::instance()

namespace sisl {

inline bool diff_vector(const reflection::Schema* schema, const reflection::Field* field,
                        flatbuffers::VectorOfAny* v1, flatbuffers::VectorOfAny* v2);

inline bool diff(const reflection::Schema* schema, const reflection::Object* schema_object,
                 const flatbuffers::Table* root, const flatbuffers::Table* old_root) {
    if (root == nullptr || old_root == nullptr) { return root == nullptr && old_root == nullptr; }

    for (auto field : *schema_object->fields()) {
        if (field->attributes() != nullptr && field->attributes()->LookupByKey("hotswap") != nullptr) { continue; }
        switch (field->type()->base_type()) {
        case reflection::BaseType::Int:
        case reflection::BaseType::UInt:
        case reflection::BaseType::None:
        case reflection::BaseType::UType:
        case reflection::BaseType::Bool:
        case reflection::BaseType::Byte:
        case reflection::BaseType::UByte:
        case reflection::BaseType::Short:
        case reflection::BaseType::UShort:
        case reflection::BaseType::Long:
        case reflection::BaseType::ULong: {
            auto a1 = flatbuffers::GetAnyFieldI(*old_root, *field);
            auto a2 = flatbuffers::GetAnyFieldI(*root, *field);
            if (a1 != a2) { return true; }
            break;
        }

        case reflection::BaseType::Float:
        case reflection::BaseType::Double: {
            auto a1 = flatbuffers::GetAnyFieldF(*old_root, *field);
            auto a2 = flatbuffers::GetAnyFieldF(*root, *field);
            if (a1 != a2) { return true; }
            break;
        }

        case reflection::BaseType::String: {
            auto s1 = flatbuffers::GetFieldS(*old_root, *field);
            auto s2 = flatbuffers::GetFieldS(*root, *field);
            if (s1 != nullptr && s2 != nullptr && s1->str() != s2->str()) { return true; }
            break;
        }

        case reflection::BaseType::Vector: {
            auto v1 = flatbuffers::GetFieldAnyV(*old_root, *field);
            auto v2 = flatbuffers::GetFieldAnyV(*root, *field);
            if (diff_vector(schema, field, v1, v2)) { return true; }
            break;
        }

        case reflection::BaseType::Obj: {
            if (field->name()->str() != "processed") {
                auto object = (*schema->objects())[field->type()->index()];
                if (diff(schema, object, flatbuffers::GetFieldT(*root, *field),
                         flatbuffers::GetFieldT(*old_root, *field))) {
                    return true;
                }
            }
            break;
        }

        default:
            // Please do not use unions or arrays in settings.
            break;
        }
    }
    return false;
}

inline bool diff_vector(const reflection::Schema* schema, const reflection::Field* field,
                        flatbuffers::VectorOfAny* v1, flatbuffers::VectorOfAny* v2) {
    if (v1 == nullptr && v2 == nullptr) return false;
    if (v1 == nullptr || v2 == nullptr || v1->size() != v2->size()) return true;

    auto type = field->type()->element();
    switch (type) {
    case reflection::BaseType::Int:
    case reflection::BaseType::UInt:
    case reflection::BaseType::None:
    case reflection::BaseType::UType:
    case reflection::BaseType::Bool:
    case reflection::BaseType::Byte:
    case reflection::BaseType::UByte:
    case reflection::BaseType::Short:
    case reflection::BaseType::UShort:
    case reflection::BaseType::Long:
    case reflection::BaseType::ULong: {
        for (size_t idx = 0; idx < v1->size(); idx++) {
            if (flatbuffers::GetAnyVectorElemI(v1, type, idx) != flatbuffers::GetAnyVectorElemI(v2, type, idx)) {
                return true;
            }
        }
        break;
    }

    case reflection::BaseType::Float:
    case reflection::BaseType::Double: {
        for (size_t idx = 0; idx < v1->size(); idx++) {
            if (flatbuffers::GetAnyVectorElemF(v1, type, idx) != flatbuffers::GetAnyVectorElemF(v2, type, idx)) {
                return true;
            }
        }
        break;
    }

    case reflection::BaseType::String: {
        for (size_t idx = 0; idx < v1->size(); idx++) {
            if (flatbuffers::GetAnyVectorElemS(v1, type, idx) != flatbuffers::GetAnyVectorElemS(v2, type, idx)) {
                return true;
            }
        }
        break;
    }

    case reflection::BaseType::Vector:
        // Nested vector not supported as of flatbuffer 1.9.0
        break;

    case reflection::BaseType::Obj: {
        auto object = (*schema->objects())[field->type()->index()];
        for (size_t idx = 0; idx < v1->size(); idx++) {
            if (diff(schema, object, flatbuffers::GetAnyVectorElemPointer< const flatbuffers::Table >(v1, idx),
                     flatbuffers::GetAnyVectorElemPointer< const flatbuffers::Table >(v2, idx))) {
                return true;
            }
        }
        break;
    }

    default:
        // Please do not use unions or arrays in settings.
        break;
    }
    return false;
}

class SettingsFactoryBase {
public:
    SettingsFactoryBase() = default;
    SettingsFactoryBase(const SettingsFactoryBase&) = delete;
    SettingsFactoryBase& operator=(const SettingsFactoryBase&) = delete;
    SettingsFactoryBase(SettingsFactoryBase&&) = delete;
    SettingsFactoryBase& operator=(SettingsFactoryBase&&) = delete;
    virtual ~SettingsFactoryBase() = default;

    virtual void load() = 0;
    [[nodiscard]] virtual bool reload() = 0;
    virtual void save() = 0;
    [[nodiscard]] virtual const std::string get_json() const = 0;

    void set_config_file(const std::string& file) { base_file_ = file; }

protected:
    std::string base_file_;
};

class SettingsFactoryRegistry {
public:
    static SettingsFactoryRegistry& instance(const std::string& path = "",
                                             const std::vector< std::string >& override_cfgs = {}) {
        static SettingsFactoryRegistry inst{path, override_cfgs};
        return inst;
    }

    SettingsFactoryRegistry(const std::string& path = "", const std::vector< std::string >& override_cfgs = {});
    void register_factory(const std::string& s, SettingsFactoryBase* f);
    void unregister_factory(const std::string& s);

    bool reload_all();
    void save_all();
    nlohmann::json get_json() const;

private:
    mutable std::shared_mutex mtx_;
    std::string config_path_;
    std::unordered_map< std::string, SettingsFactoryBase* > factories_;
    std::unordered_map< std::string, nlohmann::json > override_cfgs_;
};

template < typename SettingsT >
class SettingsFactory : public sisl::SettingsFactoryBase {
protected:
    SettingsFactory(const std::string& schema_name, unsigned char* raw_fbs, const unsigned int raw_fbs_len) :
            schema_name_{schema_name}, raw_schema_{std::string((const char*)raw_fbs, (size_t)raw_fbs_len)} {
        SettingsFactoryRegistry::instance().register_factory(schema_name, (sisl::SettingsFactoryBase*)this);
    }

public:
    // Invoke callback with a safely RCU-protected const reference to settings; wait-free on the read path.
    template < typename CB >
        requires std::invocable< CB, const SettingsT& >
    [[nodiscard]] std::remove_reference_t< std::invoke_result_t< CB, const SettingsT& > > with_settings(CB cb) const {
        auto settings = rcu_data_.get(); // RAII RCU read-side guard
        const SettingsT& s = *settings.get();
        using ret_t = std::invoke_result_t< CB, const SettingsT& >;
        static_assert(!std::is_pointer_v< ret_t > && !std::is_reference_v< ret_t >,
                      "Do not return a pointer or reference to RCU-protected settings");
        return cb(s);
    }

    // Copy-on-write update: copies current settings, applies callback, then atomically replaces via RCU.
    void modifiable_settings(const auto& cb) {
        std::scoped_lock lk(modify_mutex_);
        SettingsT new_settings;
        {
            auto settings = rcu_data_.get(); // RCU read
            new_settings = *settings.get();  // copy
        }
        cb(new_settings);
        rcu_data_.make_and_exchange(std::move(new_settings));
    }

    void load() override { load_file(base_file_); }
    [[nodiscard]] bool reload() override { return reload_file(base_file_); }
    void save() override {
        if (base_file_.length() != 0) { save(base_file_); }
    }

    void load_file(const std::string& config_file) { load(config_file, true /* is_config_file */); }
    void load_json(const std::string& json_string) { load(json_string, false /* is_config_file */); }
    [[nodiscard]] bool reload_file(const std::string& config_file) { return reload(config_file, true); }
    [[nodiscard]] bool reload_json(const std::string& json_string) { return reload(json_string, false); }

    void save(const std::string& filepath) {
        flatbuffers::Parser parser;
        parser.opts.strict_json = true;
        parser.opts.output_default_scalars_in_json = true;

        if (!parser.Parse(raw_schema_.c_str())) {
            LOGERROR("Error in parsing schema file to save");
            return;
        }

        {
            auto settings = rcu_data_.get(); // RCU read
            parser.builder_.Finish(
                SettingsT::TableType::Pack(parser.builder_, settings.get(), nullptr));
        }

        std::string fname = filepath;
        boost::replace_all(fname, ".json", "");
        if (GenTextFile(parser, "", fname) == nullptr) { LOGERROR("Error in Saving json to file"); }
    }

    const std::string& get_current_settings() const { return current_settings_; }
    const std::string& get_last_settings_error() const { return last_error_; }

    [[nodiscard]] const std::string get_json() const override {
        flatbuffers::Parser parser;
        parser.opts.strict_json = true;
        parser.opts.output_default_scalars_in_json = true;

        if (!parser.Parse(raw_schema_.c_str())) {
            LOGERROR("Error parsing flatbuffer settings schema");
            return {};
        }

        {
            auto settings = rcu_data_.get(); // RCU read
            parser.builder_.Finish(
                SettingsT::TableType::Pack(parser.builder_, settings.get(), nullptr));
        }

        std::string json;
        if (GenText(parser, parser.builder_.GetBufferPointer(), &json) == nullptr) {
            LOGERROR("Error generating json from flatbuffer");
        }
        return json;
    }

private:
    void load(const std::string& config, bool is_config_file) {
        try {
            SettingsT new_settings;
            parse_config(config, is_config_file, new_settings);
            rcu_data_.make_and_exchange(std::move(new_settings));
        } catch (std::exception& e) {
            throw std::runtime_error(fmt::format("Exception reading config {} (errmsg = {})",
                                                 (is_config_file ? config : " in json"), e.what()));
        }
    }

    bool reload(const std::string& config, bool is_config_file) {
        try {
            SettingsT new_settings;
            parse_config(config, is_config_file, new_settings);

            bool needs_restart;
            {
                auto current = rcu_data_.get(); // RCU read
                needs_restart = check_restart_needed(&new_settings, *current.get());
            }

            if (needs_restart) {
                current_settings_ = ""; // getSettings will return empty briefly before exiting
                return true;
            } else {
                rcu_data_.make_and_exchange(std::move(new_settings));
            }
        } catch (std::exception& e) {
            LOGERROR("Exception reading config {} (errmsg = {})", (is_config_file ? config : " in json"), e.what());
        }
        return false;
    }

    void parse_config(const std::string& config, bool is_file, SettingsT& out_settings) {
        std::string json_config_str;
        if (is_file) {
            if (!flatbuffers::LoadFile(config.c_str(), false, &json_config_str)) {
                last_error_ = "flatbuffer::LoadFile() returned false";
                throw std::invalid_argument(last_error_);
            }
        } else {
            json_config_str = config;
        }

        flatbuffers::Parser parser;
        parser.opts.skip_unexpected_fields_in_json = true;

        if (!parser.Parse(raw_schema_.c_str())) {
            last_error_ = parser.error_;
            throw std::invalid_argument(parser.error_);
        }
        if (!parser.Parse(json_config_str.c_str(), nullptr)) {
            last_error_ = parser.error_;
            throw std::invalid_argument(parser.error_);
        }

        current_settings_ = std::move(json_config_str);

        flatbuffers::GetRoot< typename SettingsT::TableType >(parser.builder_.GetBufferPointer())
            ->UnPackTo(&out_settings, nullptr);
    }

    // Returns true if any non-hotswappable field changed between new_settings and current_settings.
    // Caches the serialized schema binary to avoid re-parsing it on every call.
    bool check_restart_needed(const SettingsT* new_settings, const SettingsT& current_settings) {
        // Cache the serialized schema binary (for reflection) — parse schema text only once.
        if (schema_binary_.empty()) {
            flatbuffers::Parser schema_parser;
            if (!schema_parser.Parse(raw_schema_.c_str())) {
                LOGERROR("Error parsing flatbuffer settings schema: {}", schema_parser.error_);
                return false;
            }
            schema_parser.Serialize();
            const auto* buf = schema_parser.builder_.GetBufferPointer();
            schema_binary_.assign(buf, buf + schema_parser.builder_.GetSize());
        }

        const reflection::Schema* schema = reflection::GetSchema(schema_binary_.data());
        if (schema->root_table() == nullptr) {
            LOGINFO("schema->root_table() is null in check_restart_needed(..)");
            return false;
        }

        // Pack new settings to binary.
        flatbuffers::Parser new_parser;
        if (!new_parser.Parse(raw_schema_.c_str())) {
            LOGERROR("Error parsing flatbuffer settings schema: {}", new_parser.error_);
            return false;
        }
        new_parser.builder_.Finish(SettingsT::TableType::Pack(new_parser.builder_, new_settings, nullptr));

        // Pack current (old) settings to binary.
        flatbuffers::Parser old_parser;
        if (!old_parser.Parse(raw_schema_.c_str())) {
            LOGERROR("Error parsing flatbuffer settings schema: {}", old_parser.error_);
            return false;
        }
        old_parser.builder_.Finish(
            SettingsT::TableType::Pack(old_parser.builder_, &current_settings, nullptr));

        auto* root_obj = flatbuffers::GetAnyRoot(new_parser.builder_.GetBufferPointer());
        auto* old_root_obj = flatbuffers::GetAnyRoot(old_parser.builder_.GetBufferPointer());

        bool restart = diff(schema, schema->root_table(), root_obj, old_root_obj);
        if (!restart) {
            LOGINFO("check_restart_needed(..) found no changes which need restart");
        } else {
            LOGINFO("check_restart_needed(..) found changes which need restart");
        }
        return restart;
    }

private:
    std::string schema_name_;
    std::string raw_schema_;

    // Lazily-cached serialized schema binary (for reflection in check_restart_needed).
    std::vector< uint8_t > schema_binary_;

    // Unparsed settings string (last successfully parsed config).
    std::string current_settings_;

    // Last settings parse error.
    std::string last_error_;

    // Serializes concurrent modifiable_settings() callers.
    std::mutex modify_mutex_;

    // RCU-protected settings data; reads are wait-free.
    Rcu::data< SettingsT > rcu_data_;
};

} // namespace sisl

/*
 * SETTINGS(sname, var, ...) — call with_settings on the named factory, binding the settings ref to var.
 * SETTINGS_THIS(sname, var, ...) — same, capturing `this` in the lambda.
 * SETTINGS_THIS_CAP1(sname, var, cap1, ...) — same, capturing `this` and one extra variable.
 * SETTINGS_VALUE(sname, path_expr) — return a single value from settings by dotted path expression.
 */
#define SETTINGS(sname, var, ...) SETTINGS_FACTORY(sname).with_settings([](auto& var) __VA_ARGS__)
#define SETTINGS_THIS(sname, var, ...) SETTINGS_FACTORY(sname).with_settings([this](auto& var) __VA_ARGS__)
#define SETTINGS_THIS_CAP1(sname, var, cap1, ...)                                                                      \
    SETTINGS_FACTORY(sname).with_settings([ this, cap1 ](auto& var) __VA_ARGS__)
#define SETTINGS_VALUE(sname, path_expr) SETTINGS_FACTORY(sname).with_settings([](auto& s_) { return s_.path_expr; })
