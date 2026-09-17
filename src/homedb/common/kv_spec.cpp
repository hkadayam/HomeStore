#include "homedb/common/kv_spec.h"

#include <fmt/format.h>

namespace homedb {

Result< void > KeySpec::validate_bytes(size_t n) const {
    if (auto* f = std::get_if< FixedKey >(&key_type)) {
        if (n != f->size) {
            return err_invalid_argument(fmt::format("key size {} does not match fixed key size {}", n, f->size));
        }
    } else {
        auto const cap = std::get< VariableKey >(key_type).max_size;
        if (n > cap) {
            return err_invalid_argument(fmt::format("key size {} exceeds variable-key max {}", n, cap));
        }
    }
    return {};
}

Result< void > ValueSpec::validate_bytes(size_t n) const {
    if (auto* f = std::get_if< FixedValue >(&value_type)) {
        if (n != f->size) {
            return err_invalid_argument(fmt::format("value size {} does not match fixed value size {}", n, f->size));
        }
    } else {
        auto const cap = std::get< VariableValue >(value_type).max_size;
        if (n > cap) {
            return err_invalid_argument(fmt::format("value size {} exceeds variable-value max {}", n, cap));
        }
    }
    return {};
}

TableSpecOnDisk TableSpecOnDisk::from(TableSpec const& t) {
    TableSpecOnDisk d{};
    if (auto* f = std::get_if< FixedKey >(&t.key_spec.key_type)) {
        d.key_kind = 0;
        d.key_size = to_u32(f->size);
    } else {
        d.key_kind = 1;
        d.key_size = to_u32(std::get< VariableKey >(t.key_spec.key_type).max_size);
    }
    if (auto* f = std::get_if< FixedValue >(&t.value_spec.value_type)) {
        d.value_kind = 0;
        d.value_size = to_u32(f->size);
    } else {
        d.value_kind = 1;
        d.value_size = to_u32(std::get< VariableValue >(t.value_spec.value_type).max_size);
    }
    if (auto* p = std::get_if< Prefixable >(&t.key_spec.prefix_type)) {
        d.prefix_kind = 1;
        d.prefix_size = p->prefix_size ? to_u32(*p->prefix_size) : 0;
    }
    d.mvcc_enabled = t.mvcc_enabled ? 1 : 0;
    d.partition_key_size = to_u32(t.partition_key_size);
    return d;
}

TableSpec TableSpecOnDisk::to_spec() const {
    KeyType kt = (key_kind == 0) ? KeyType{FixedKey{key_size}} : KeyType{VariableKey{key_size}};
    ValueType vt = (value_kind == 0) ? ValueType{FixedValue{value_size}} : ValueType{VariableValue{value_size}};
    PrefixType pt = (prefix_kind == 0)
        ? PrefixType{Regular{}}
        : PrefixType{Prefixable{prefix_size ? std::optional< size_t >{prefix_size} : std::nullopt}};
    TableSpec s{KeySpec{kt, pt}, ValueSpec{vt}};
    s.mvcc_enabled = (mvcc_enabled != 0);
    s.partition_key_size = partition_key_size;
    return s;
}

} // namespace homedb
