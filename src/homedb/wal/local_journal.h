#pragma once

#include <atomic>

#include "common/async.h"
#include "common/defs.h"
#include "homestore/logstore/log_store.h"

#include "homedb/wal/journal.h"

namespace homedb {

// ── LocalJournal ─────────────────────────────────────────────────────────────────────────────────────────────────
// Journal backed by a single homestore::LogStore.  write() forwards the caller's LogBlob straight to
// append_and_flush (LogStore internally copies parts onto its own log-stream buffer), then hands the SAME
// LogBlob back to the sink.  Replay wraps LogStore's returned IoBufView in a 1-part LogBlob before invoking
// the sink so live and replay share one code path in Database.

class LocalJournal final : public Journal {
public:
    static Async< shared< LocalJournal > > create();
    static shared< LocalJournal > load(homestore::logstore_id_t store_id);

    LocalJournal(LocalJournal const&) = delete;
    LocalJournal& operator=(LocalJournal const&) = delete;
    LocalJournal(LocalJournal&&) = delete;
    LocalJournal& operator=(LocalJournal&&) = delete;
    ~LocalJournal() override = default;

    // ── Journal ──────────────────────────────────────────────────────────────
    void set_sink(CommitSink* sink) override;
    Async< Result< uint64_t > > write(homestore::LogBlob const& record_parts) override;
    uint64_t last_committed_lsn() const override;
    uint32_t journal_id() const override;
    Async< void > advance_truncate_upto(uint64_t lsn) override;

private:
    explicit LocalJournal(shared< homestore::LogStore > log_store);

    Async< void > on_replay(homestore::lsn_t lsn, sisl::IoBufView const& data);

    shared< homestore::LogStore > log_store_;
    std::atomic< CommitSink* > sink_{nullptr};
};

} // namespace homedb
