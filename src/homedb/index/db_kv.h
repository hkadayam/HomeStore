#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/defs.h"
#include "sisl/fds/buffer.h"
#include "homestore/index/btree/btree_kv.h"

#include "homedb/common/kv_spec.h"

namespace homedb {

// ── DbKey / DbValue ──────────────────────────────────────────────────────────────────────────────────────────────
// homestore::BtreeKey / BtreeValue implementations that carry a `sisl::Blob view_` (always the source of
// bytes) plus an optional `sisl::IoBufShared owner_` (present when we allocated the bytes ourselves).
//
// Two construction paths and no variant / discriminant — it's just RAII plus an optional owner:
//
//   1. From a caller-lived Blob (write path).  We do NOT copy.  The caller (Database/ApplyRouter) holds the
//      log-record buffer alive for the duration of the co_await, so DbKey's view stays valid until the
//      btree memcpys the bytes into a leaf node.  owner_ is null.
//
//   2. From the btree during read-path deserialize(copy=true).  We allocate owner_ + memcpy the bytes so the
//      returned DbValue outlives the leaf-node buffer.  view_ points into owner_.
//
// deserialize(b, copy=false) sets view_=b and leaves owner_ null — safe only for transient use inside a
// single btree op (compare / range test).  deserialize(copy=true) always owns.

class DbKey : public homestore::BtreeKey {
public:
    DbKey() = default;

    /// Borrow-from-caller construction.  Caller must guarantee `src`'s bytes outlive every use of this DbKey.
    /// The write path passes the log-record buffer which is held on the caller's stack across the co_await.
    DbKey(sisl::Blob const& src, std::optional< size_t > fixed_size);

    /// Copy ctor shares the refcounted owner_ if present (no bytes copy).  If the source was borrowed
    /// (owner_ == null), the borrowed view is copied — caller must still keep the underlying bytes alive.
    DbKey(DbKey const& other) = default;
    DbKey& operator=(DbKey const& other) = default;
    DbKey(DbKey&&) noexcept = default;
    DbKey& operator=(DbKey&&) noexcept = default;
    ~DbKey() override = default;

    uint8_t const* data() const { return view_.cbytes(); }
    uint32_t size() const { return view_.size(); }
    std::optional< size_t > fixed_size() const { return fixed_size_; }

    /// The refcounted owner if we allocated our own bytes; null if we're a borrowed view.
    sisl::IoBufShared const& buf() const { return owner_; }

    // BtreeKey overrides
    int compare(homestore::BtreeKey const& other) const override;
    sisl::Blob serialize() const override;
    uint32_t serialized_size() const override;
    void deserialize(sisl::Blob const& b, bool copy) override;
    std::string to_string() const override;

private:
    sisl::Blob view_;
    sisl::IoBufShared owner_; // non-null iff this DbKey allocated its own bytes
    std::optional< size_t > fixed_size_;
};

class DbValue : public homestore::BtreeValue {
public:
    DbValue() = default;

    DbValue(sisl::Blob const& src, std::optional< size_t > fixed_size);

    DbValue(DbValue const& other) = default;
    DbValue& operator=(DbValue const& other) = default;
    DbValue(DbValue&&) noexcept = default;
    DbValue& operator=(DbValue&&) noexcept = default;
    ~DbValue() override = default;

    uint8_t const* data() const { return view_.cbytes(); }
    uint32_t size() const { return view_.size(); }
    std::optional< size_t > fixed_size() const { return fixed_size_; }

    sisl::IoBufShared const& buf() const { return owner_; }

    // BtreeValue overrides
    sisl::Blob serialize() const override;
    uint32_t serialized_size() const override;
    void deserialize(sisl::Blob const& b, bool copy) override;
    std::string to_string() const override;

private:
    sisl::Blob view_;
    sisl::IoBufShared owner_;
    std::optional< size_t > fixed_size_;
};

} // namespace homedb
