#pragma once

#include <cstdint>
#include <string>

#include "common/async.h"
#include "common/defs.h"
#include "sisl/fds/buffer.h"

#include "homedb/common/error.h"
#include "homedb/common/kv_spec.h"
#include "homedb/index/db_kv.h"
#include "homedb/index/unsharded_btree.h"
#include "homedb/wal/record.h"

namespace homedb {

// ── Table ────────────────────────────────────────────────────────────────────────────────────────────────────────
// Passive owner of one persistent index.  All writes flow in from Database via commit() during the
// on_commit path (either live or replay).  Reads via get() bypass the journal entirely.  Table holds no
// journal reference — Database drives the write side.

class Table {
public:
    Table(uint16_t table_id, std::string name, TableSpec spec,
          unique< UnshardedBtree< DbKey, DbValue > > index);

    Table(Table const&) = delete;
    Table& operator=(Table const&) = delete;
    Table(Table&&) = delete;
    Table& operator=(Table&&) = delete;
    ~Table() = default;

    uint16_t table_id() const { return table_id_; }
    std::string const& name() const { return name_; }
    TableSpec const& spec() const { return spec_; }

    /// Apply one durable entry to the index.  `lsn` is ignored by the non-mvcc index today; MVCC will stamp
    /// it into MvccKey.  key/value must outlive the co_await.
    Async< Result< void > > commit(uint64_t lsn, OpType op, sisl::Blob const& key, sisl::Blob const& value);

    /// Point read.  Returns null IoBufShared for not-found.
    Async< Result< sisl::IoBufShared > > get(sisl::Blob const& key);

private:
    uint16_t table_id_;
    std::string name_;
    TableSpec spec_;
    unique< UnshardedBtree< DbKey, DbValue > > index_;
};

} // namespace homedb
