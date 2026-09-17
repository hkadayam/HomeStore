#pragma once

#include <cstdint>
#include <set>

#include "common/async.h"
#include "common/defs.h"
#include "sisl/fds/buffer.h"

#include "homestore/logstore/log_blob.h"

#include "homedb/common/error.h"
#include "homedb/wal/record.h"

namespace homedb {

// ── JournalConfig ────────────────────────────────────────────────────────────────────────────────────────────────
// Selects which concrete Journal backend Journal::create / Journal::load construct.  Populated by the caller
// (typically Database, threaded down from user code) and consumed by the Journal factory to pick a backend.

struct JournalConfig {
    bool replicated{false};

    // Fields consumed only when replicated=true.
    Uuid repl_group_id{};
    std::set< Uuid > repl_members;
};

// ── Journal ──────────────────────────────────────────────────────────────────────────────────────────────────────
// Abstract write-ahead log.  Byte-agnostic — knows nothing about tables, record formats, or the KV layer.
// Its only job is: accept a scatter-gather record from the caller, make it durable, then hand the same
// record back to the caller's CommitSink along with the assigned lsn.  Replay walks recovered records and
// drives the same on_commit path — the sink cannot distinguish a live commit from a replayed one.
//
// One Journal per Database.  Concrete backends:
//   LocalJournal — HomeStore LogStore-backed, single-node durability.
//   ReplJournal  — HomeStore ReplicaSet-backed, replicated quorum durability.
//
// Construction goes exclusively through Journal::create / Journal::load; both dispatch on config.

class Journal {
public:
    // ── CommitSink ──────────────────────────────────────────────────────────────────────────────────
    // Implemented by Database.  Fires once per durable record, in lsn order:
    //   Live path: `record_parts` is exactly the LogBlob the caller passed to write().
    //   Replay path: `record_parts` is a 1-part LogBlob wrapping the IoBufView LogStore returned.
    // Either way the sink can walk it with a scatter-gather cursor and decode fields by known sizes.
    class CommitSink {
    public:
        virtual ~CommitSink() = default;
        virtual Async< void > on_commit(uint64_t lsn, homestore::LogBlob const& record_parts) = 0;
    };

    static Async< shared< Journal > > create(JournalConfig const& config);
    static Async< shared< Journal > > load(JournalConfig const& config, uint32_t journal_id);

    virtual ~Journal() = default;

    /// Bind the sink that receives on_commit for every durable record (live and replay).  Must be called
    /// before any write and before replay fires.
    virtual void set_sink(CommitSink* sink) = 0;

    /// Durably record the scatter-gather parts.  Returns the assigned lsn once durable and the sink's
    /// on_commit has fired.  Each Blob in `record_parts` must outlive the co_await (caller's coroutine
    /// frame keeps them alive through the whole chain).
    virtual Async< Result< uint64_t > > write(homestore::LogBlob const& record_parts) = 0;

    /// Highest LSN currently durable.  Used later by MVCC snapshot creation; exposed here for tests.
    virtual uint64_t last_committed_lsn() const = 0;

    /// Backend-specific identifier persisted per Table so recovery can reopen the right journal.
    virtual uint32_t journal_id() const = 0;

    /// Advance the journal head after a CP flush with the max lsn already flushed into the persistent index.
    virtual Async< void > advance_truncate_upto(uint64_t lsn) = 0;
};

} // namespace homedb
