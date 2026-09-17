#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "common/defs.h"
#include "homedb/common/error.h"

namespace homedb {

// ── KeyType ──────────────────────────────────────────────────────────────────────────────────────────────────────
// Fixed(n): every key is exactly n bytes.  Variable(max): keys may be 0..max bytes, size carried per-key.

struct FixedKey {
    size_t size;
};
struct VariableKey {
    size_t max_size;
};
using KeyType = std::variant< FixedKey, VariableKey >;

// ── PrefixType ───────────────────────────────────────────────────────────────────────────────────────────────────
// Reserved for a later prefix-compression phase.  Regular is the only Phase-1 value used at runtime; the
// Prefixable case is preserved from the Rust design so the on-disk TableSpec encoding stays stable.

struct Regular {};
struct Prefixable {
    std::optional< size_t > prefix_size; // nullopt = auto-detect
};
using PrefixType = std::variant< Regular, Prefixable >;

// ── KeySpec / ValueSpec / TableSpec ──────────────────────────────────────────────────────────────────────────────

class KeySpec {
public:
    KeyType key_type;
    PrefixType prefix_type;

    KeySpec() : key_type{VariableKey{256}}, prefix_type{Regular{}} {}
    KeySpec(KeyType kt, PrefixType pt) : key_type{std::move(kt)}, prefix_type{std::move(pt)} {}

    static KeySpec fixed(size_t n) { return KeySpec{FixedKey{n}, Regular{}}; }
    static KeySpec variable(size_t max) { return KeySpec{VariableKey{max}, Regular{}}; }

    // Fixed size in bytes, or nullopt for variable.  Consumed by DbKey to select serialization/node-variant.
    std::optional< size_t > fixed_size() const {
        if (auto* f = std::get_if< FixedKey >(&key_type)) {
            return f->size;
        }
        return std::nullopt;
    }
    size_t max_size() const {
        if (auto* f = std::get_if< FixedKey >(&key_type)) {
            return f->size;
        }
        return std::get< VariableKey >(key_type).max_size;
    }

    Result< void > validate_bytes(size_t n) const;
};

struct FixedValue {
    size_t size;
};
struct VariableValue {
    size_t max_size;
};
using ValueType = std::variant< FixedValue, VariableValue >;

class ValueSpec {
public:
    ValueType value_type;

    ValueSpec() : value_type{VariableValue{1024}} {}
    explicit ValueSpec(ValueType vt) : value_type{std::move(vt)} {}

    static ValueSpec fixed(size_t n) { return ValueSpec{FixedValue{n}}; }
    static ValueSpec variable(size_t max) { return ValueSpec{VariableValue{max}}; }

    std::optional< size_t > fixed_size() const {
        if (auto* f = std::get_if< FixedValue >(&value_type)) {
            return f->size;
        }
        return std::nullopt;
    }
    size_t max_size() const {
        if (auto* f = std::get_if< FixedValue >(&value_type)) {
            return f->size;
        }
        return std::get< VariableValue >(value_type).max_size;
    }

    Result< void > validate_bytes(size_t n) const;
};

// Per-table schema.  mvcc_enabled + partition_key_size are accepted at the API but v1 rejects any non-default
// combination (see Database::create_table validation).
class TableSpec {
public:
    KeySpec key_spec;
    ValueSpec value_spec;
    bool mvcc_enabled{false};
    size_t partition_key_size{0}; // 0 == unsharded; >0 rejected in v1 (Plan §3a).

    TableSpec() = default;
    TableSpec(KeySpec k, ValueSpec v) : key_spec{std::move(k)}, value_spec{std::move(v)} {}

    static TableSpec fixed_kv(size_t key_size, size_t value_size) {
        return TableSpec{KeySpec::fixed(key_size), ValueSpec::fixed(value_size)};
    }
};

// ── Wire codec for TableSpec (persisted inside COWBtreeSuperBlock::user_sb_data) ────────────────────────────────
// Fixed-layout binary — 32 bytes.  Enum discriminants are u8, sizes are u32 (large enough for any realistic
// key/value size).  Version byte at [0] so future format bumps have a safe recovery path.

#pragma pack(1)
struct TableSpecOnDisk {
    static constexpr uint8_t kFormatVersion = 1;

    uint8_t format_version{kFormatVersion};
    uint8_t key_kind{0};    // 0 = FixedKey, 1 = VariableKey
    uint8_t value_kind{0};  // 0 = FixedValue, 1 = VariableValue
    uint8_t prefix_kind{0}; // 0 = Regular, 1 = Prefixable  (Phase-1 only writes 0)
    uint32_t key_size{0};   // Fixed: exact size; Variable: max size
    uint32_t value_size{0}; // same convention
    uint8_t mvcc_enabled{0};
    uint8_t _pad[3]{};
    uint32_t partition_key_size{0};
    uint32_t prefix_size{0}; // 0 = auto-detect when prefix_kind==1
    uint32_t reserved0{0};
    uint32_t reserved1{0};

    static TableSpecOnDisk from(TableSpec const& t);
    TableSpec to_spec() const;
};
#pragma pack()

static_assert(sizeof(TableSpecOnDisk) == 32, "TableSpecOnDisk must remain a stable 32 bytes");

} // namespace homedb