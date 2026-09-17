#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/SharedMutex.h>

#include "common/async.h"
#include "common/defs.h"
#include "sisl/fds/buffer.h"

#include "homedb/common/error.h"
#include "homedb/common/kv_spec.h"
#include "homedb/db_table/table.h"
#include "homedb/wal/journal.h"

namespace homedb {

// ── Database ─────────────────────────────────────────────────────────────────────────────────────────────────────
// A collection of Tables sharing one Journal.  Database is the caller-facing surface for the write side:
// put/remove/get(table_name, ...).  Reads bypass the journal (Table.get direct).  Writes encode the op as
// a 1-entry transaction record and hand a scatter-gather LogBlob to journal.write() — no record-buffer
// allocation on the live path.  When a real Transaction API arrives, multi-entry batches encode the same
// way (extra entries appended to the same TxnRecordHeader) and reuse this on_commit dispatch.
//
// Database implements Journal::CommitSink and installs itself on the journal at open time; both live writes
// and replay drive on_commit → per-entry dispatch to Table.commit.

class Database final : public Journal::CommitSink {
public:
    static Async< shared< Database > > open(JournalConfig const& config);

    Database(Database const&) = delete;
    Database& operator=(Database const&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;
    ~Database();

    /// Create a fresh table.  Allocates a per-table BlobDev + COWBtree, wraps in an UnshardedBtree, wires
    /// it into Database's registry.  Fails if a table with `name` already exists.
    Async< Result< shared< Table > > > create_table(std::string const& name, TableSpec const& spec);

    /// Table lookup by name.  Null shared_ptr when absent.
    shared< Table > get_table(std::string const& name) const;

    /// All tables currently registered.
    std::vector< shared< Table > > tables() const;

    // ── Write surface (single-op) — encoded as a 1-entry txn record and routed through the journal ─────
    Async< Result< uint64_t > > put(std::string const& table, sisl::Blob const& key, sisl::Blob const& value);
    Async< Result< uint64_t > > remove(std::string const& table, sisl::Blob const& key);

    /// Convenience: table lookup + Table.get, which bypasses the journal.
    Async< Result< sisl::IoBufShared > > get(std::string const& table, sisl::Blob const& key);

    Journal& journal() { return *journal_; }
    Journal const& journal() const { return *journal_; }

    // ── Journal::CommitSink ─────────────────────────────────────────────────
    /// Fires from journal.write() (live) and journal.on_replay() (recovery).  Walks the record parts and
    /// dispatches each entry to its target Table.commit.
    Async< void > on_commit(uint64_t lsn, homestore::LogBlob const& record_parts) override;

private:
    explicit Database(shared< Journal > journal);

    /// Write helper — builds the 4-part LogBlob for a single (table_id, op, key, value) entry and pushes it
    /// through the journal.  Both TxnRecordHeader and TxnEntryHeader live on the caller's coroutine frame
    /// through the co_await chain.
    Async< Result< uint64_t > > write_single_entry(uint16_t table_id, OpType op, sisl::Blob const& key,
                                                   sisl::Blob const& value);

    /// Lookup by table_id used by on_commit dispatch.  O(1).
    Table* table_by_id(uint16_t table_id) const;

    shared< Journal > journal_; // declared FIRST so it's destroyed LAST (after Tables clear)
    mutable folly::SharedMutex tables_mtx_;
    std::unordered_map< std::string, shared< Table > > tables_by_name_;
    std::unordered_map< uint16_t, Table* > tables_by_id_;
    uint16_t next_table_id_{0};
};

} // namespace homedb
