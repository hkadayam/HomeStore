#pragma once

#include "common/async.h"
#include "common/defs.h"

#include "homedb/wal/journal.h"

namespace homedb {

// ── ReplJournal ──────────────────────────────────────────────────────────────────────────────────────────────────
// Replicated Journal backed by a homestore::ReplicaSet.  Stubbed in Phase 1 — all methods throw so the
// intent is loud if a caller wires it up before the backend is implemented.  Journal::create /
// Journal::load dispatch here when JournalConfig::replicated == true.

class ReplJournal final : public Journal {
public:
    static Async< shared< ReplJournal > > create(JournalConfig const& config);
    static Async< shared< ReplJournal > > load(JournalConfig const& config, uint32_t journal_id);

    ReplJournal(ReplJournal const&) = delete;
    ReplJournal& operator=(ReplJournal const&) = delete;
    ReplJournal(ReplJournal&&) = delete;
    ReplJournal& operator=(ReplJournal&&) = delete;
    ~ReplJournal() override = default;

    void set_sink(CommitSink* sink) override;
    Async< Result< uint64_t > > write(homestore::LogBlob const& record_parts) override;
    uint64_t last_committed_lsn() const override;
    uint32_t journal_id() const override;
    Async< void > advance_truncate_upto(uint64_t lsn) override;

private:
    ReplJournal() = default;
};

} // namespace homedb
