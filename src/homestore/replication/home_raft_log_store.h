/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "common/async.h"

#include "common/defs.h"
#include "homestore/base/blk.h"
#include "homestore/logstore/log_store.h"
#include "homestore/replication/repl_decls.h"
#include "homestore/replication/replica_set.h"

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <libnuraft/nuraft.hxx>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif
#undef auto_lock

namespace nuraft {
class raft_server;
} // namespace nuraft

namespace homestore {

class LogStore;
class RawBlkStream;

// HomeRaftLogStore — implements nuraft::log_store on top of homestore::LogStore (+ optional RawBlkStream for the
// large-value optimization). Per ReplicaSet: log_store_ holds raft entries (append-mode, inline for small or a
// 13-byte indirect record for large); free_blks_journal_ is a sparse non-append-mode store of deferred app
// frees, created only when blob_stream_ is non-null; blob_stream_ stores the value blobs.
//
// nuraft's log_store API is coroutine-based (folly::coro::Task) under NURAFT_CORO_MODE. Reads (entry_at /
// term_at / last_entry / pack) first probe an in-memory entry_cache_ over the recent uncommitted window; on a
// hit they resolve without suspending (try_entry_at / try_term_at serve nuraft's hot vote / append_entries
// paths this way), on a miss they co_await LogStore::read. Writes (append / write_at / apply_pack) co_await the
// large-value blob write when the entry is indirect, and never block a reactor thread.
class HomeRaftLogStore : public nuraft::log_store {
public:
    // Callback type for the main-log replay walk.  Fires once per replayed entry during LogStoreManager
    // recovery — ReplicaSet uses it to fold ReplLogHeader.commit_lsn_at_write into commit_upto_lsn_.
    using OnLogFound = std::function< Async< void >(lsn_t, sisl::IoBufView const&) >;

    // First-boot create. Allocates a fresh main log_store; if blob_stream is non-null, also allocates a
    // free_blks log_store. Records both ids back into `sb` (caller persists the SB separately — the caller
    // owns the underlying MetaBlk; HomeRaftLogStore only mutates the two id fields).  `on_log_found` is
    // registered on the main log_store for replay callbacks — no replay happens on a fresh create, but
    // registering here keeps the API uniform with load().  Returns shared<> because nuraft's state_mgr
    // load_log_store() must hand back a shared_ptr<log_store>, and HomeRaftLogStore inherits log_store.
    static Async< shared< HomeRaftLogStore > > create(ReplicaSetSuperBlk& sb, shared< RawBlkStream > blob_stream,
                                                      OnLogFound on_log_found);

    // Restart load. Opens existing log_stores from ids in `sb`; throws if sb.free_blks_journal_id is set but
    // blob_stream is null (app removed the optimization across restart but persisted state still needs it).
    // `on_log_found` is registered on the main log_store and fires per entry during LogStoreManager's
    // recovery walk.  Only reads from `sb`.
    static Async< shared< HomeRaftLogStore > > load(ReplicaSetSuperBlk& sb, shared< RawBlkStream > blob_stream,
                                                    OnLogFound on_log_found);

    HomeRaftLogStore(HomeRaftLogStore const&) = delete;
    HomeRaftLogStore& operator=(HomeRaftLogStore const&) = delete;
    HomeRaftLogStore(HomeRaftLogStore&&) = delete;
    HomeRaftLogStore& operator=(HomeRaftLogStore&&) = delete;
    virtual ~HomeRaftLogStore() = default;

    /// Tear down everything this raft log store owns — frees indirect-handler BlkIds, destroys the
    /// free_blks_journal LogStore, and destroys the main raft LogStore.  Accessing this object after
    /// destroy() returns is undefined.
    Async< void > destroy();

    /// The first available slot of the store, starts with 1.
    /// @return Last log index number + 1
    virtual ulong next_slot() const override;

    /// The start index of the log store, at the very beginning, it must be 1. However, after some compact
    /// actions, this could be anything greater or equals to one.
    /// @return Starting log index number.
    virtual ulong start_index() const override;

    /// The last log entry in store.
    /// @return If no log entry exists: a dummy constant entry with value set to null and term set to zero.
    virtual Async< RaftLogEntryPtr > last_entry() const override;

    /// Append a log entry to store
    /// @param entry Log entry
    /// @return Log index number.
    virtual Async< ulong > append(RaftLogEntryPtr& entry) override;

    /// Overwrite a log entry at the given `index`.
    /// @param index Log index number to overwrite.
    /// @param entry New log entry to overwrite.
    virtual Async< void > write_at(ulong index, RaftLogEntryPtr& entry) override;

    /// Invoked after a batch of logs is written as a part of a single append_entries request.
    /// @param start The start log index number (inclusive)
    /// @param cnt The number of log entries written.
    virtual void end_of_append_batch(ulong start, ulong cnt) override;

    /// Get log entries with index [start, end). Return nullptr to indicate error if any log entry within the
    /// requested range could not be retrieved (e.g. due to external log truncation).
    /// @param start The start log index number (inclusive).
    /// @param end The end log index number (exclusive).
    /// @return The log entries between [start, end).
    virtual Async< nuraft::ptr< std::vector< RaftLogEntryPtr > > > log_entries(ulong start, ulong end) override;

    /// Get log entries with index [start, end). The total size of the returned entries is limited by
    /// batch_size_hint. Return nullptr to indicate error if any log entry within the requested range could not
    /// be retrieved (e.g. due to external log truncation).
    /// @param start The start log index number (inclusive).
    /// @param end The end log index number (exclusive).
    /// @param batch_size_hint_in_bytes Total size (in bytes) of the returned entries, see the detailed comment
    ///        at `state_machine::get_next_batch_size_hint_in_bytes()`.
    /// @return The log entries between [start, end) and limited by the total size given by the
    ///         batch_size_hint_in_bytes.
    virtual Async< nuraft::ptr< std::vector< RaftLogEntryPtr > > >
    log_entries_ext(ulong start, ulong end, int64_t batch_size_hint_in_bytes = 0) override;

    /// Get the log entry at the specified log index number.
    /// @param index Should be equal to or greater than 1.
    /// @return The log entry or null if index >= this->next_slot().
    virtual Async< RaftLogEntryPtr > entry_at(ulong index) override;

    /// Non-suspending fast path for entry_at — serves an entry_cache_ hit synchronously; nullopt on a miss so
    /// nuraft falls back to entry_at.  Lets the hot vote / append_entries paths avoid a coroutine suspension.
    virtual std::optional< RaftLogEntryPtr > try_entry_at(ulong index) override;

    /// Get the term for the log entry at the specified index. Suggest to stop the system if the index >=
    /// this->next_slot()
    /// @param index Should be equal to or greater than 1.
    /// @return The term for the specified log entry, or 0 if index < this->start_index().
    virtual Async< ulong > term_at(ulong index) override;

    /// Non-suspending fast path for term_at — same entry_cache_ hit / nullopt-on-miss contract as try_entry_at.
    virtual std::optional< ulong > try_term_at(ulong index) override;

    /// Pack cnt log items starts from index
    /// @param index The start log index number (inclusive).
    /// @param cnt The number of logs to pack.
    /// @return log pack
    virtual Async< RaftBufferPtr > pack(ulong index, int32_t cnt) override;

    /// Apply the log pack to current log store, starting from index.
    /// @param index The start log index number (inclusive).
    /// @param pack
    virtual Async< void > apply_pack(ulong index, nuraft::buffer& pack) override;

    /// Compact the log store by purging all log entries, including the log at the last_log_index. If current
    /// max log idx is smaller than given `last_log_index`, set start log idx to `last_log_index + 1`.
    /// @param last_log_index Log index number that will be purged up to (inclusive).
    /// @return True on success.
    virtual Async< bool > compact(ulong last_log_index) override;

    /// Synchronously flush all log entries in this log store to the backing storage so that all log entries
    /// are guaranteed to be durable upon process crash.
    /// @return `true` on success.
    virtual Async< bool > flush() override;

    /// This API is used only when `raft_params::parallel_log_appending_` flag is set. Please refer to the
    /// comment of the flag. NOTE: In homestore replication use cases, we use this even without
    /// parallel_log_appending_ flag is not set
    /// @return The last durable log index.
    virtual ulong last_durable_index() override;

public:
    ///////////////////// All Additional methods specific to HomeRaftLogStore //////////////////////////

    /// Purge all logs in the log store. It is a dangerous operation and to be used with care
    Async< void > purge_all_logs();

    void set_last_durable_lsn(raft_lsn_t lsn);

    /// Stores a non-owning pointer to the raft_server.  Caller must null it back via
    /// set_raft_server(nullptr) before raft_server is destroyed.
    void set_raft_server(nuraft::raft_server* rs) { raft_server_ = rs; }

    /// Applicable only if large value optimization is turned on.
    /// App-driven free. If `referenced_lsn` is still in the main log (≥ start_index), the free is deferred
    /// until that lsn is truncated/rolled back; otherwise it happens immediately. Crash-safe across restart
    /// — deferred intents persist with the same durability semantics as the main log.
    Async< void > deferred_free(BlkId blkid, raft_lsn_t referenced_lsn);

    /// Promotes the indirect entry's BlkIds at `lsn` from uncommitted to committed (CP-durable) and
    /// returns them.  Past this point the BlkIds are app-owned; only deferred_free brings them back.
    /// Returns an empty BlkIds for inline entries and when the large-value optimization is off.
    BlkIds on_commit(raft_lsn_t lsn);

    // ─────────────────────────────────────────────────────────────────────────────────────────────────────
    // IndirectBlkHandler — nested helper that owns the large-value optimization machinery for this
    // HomeRaftLogStore.  When an entry's value payload is at or above the size threshold, the value bytes
    // are written to blob_stream_ and the journal record carries only a shim (marker + BlkId list) — this
    // keeps the journal compact and avoids round-tripping multi-MB values through it.
    //
    // Locking: indirect_mtx_ protects uncommitted_blkids_ / deferred_free_blkids_ + serializes the
    // journal mutations.  LogStore does NOT support concurrent quick_append + rollback, so we hold the
    // lock across the journal call too.
    // ─────────────────────────────────────────────────────────────────────────────────────────────────────
    class IndirectBlkHandler {
    public:
        // Constructs with a pre-acquired free_blks_journal LogStore.  main_log_store is borrowed
        // non-owning.
        IndirectBlkHandler(shared< RawBlkStream > blob_stream, shared< LogStore > free_blks_journal,
                           LogStore* main_log_store);

        // Constructs by synchronously opening free_blks_journal_id and registering an on_log_found
        // handler (capturing `this`) that populates deferred_free_blkids_ when LogStoreManager::recover()
        // walks the journal records.  Throws if open returns null.
        IndirectBlkHandler(shared< RawBlkStream > blob_stream, logstore_id_t free_blks_journal_id,
                           LogStore* main_log_store);

        ~IndirectBlkHandler() = default;
        IndirectBlkHandler(IndirectBlkHandler const&) = delete;
        IndirectBlkHandler& operator=(IndirectBlkHandler const&) = delete;
        IndirectBlkHandler(IndirectBlkHandler&&) = delete;
        IndirectBlkHandler& operator=(IndirectBlkHandler&&) = delete;

        // Allocates BlkIds for the entry's value bytes, writes the value to blob_stream_, builds the
        // on-disk shim (9B nuraft hdr + ReplLogHdr(INDIRECT) + user_header + BlkIds trailer), parks the
        // shim on entry->set_private_buf(), and returns (LogBlob over the shim, allocated BlkIds).  Use
        // on_blkids_written() to bind the returned BlkIds to the lsn once the LogBlob is appended.  Inline
        // callers never reach here; this path co_awaits the blob_stream_ value write.
        Async< std::pair< LogBlob, BlkIds > > write(RaftLogEntryPtr& entry);

        // Records `bids` in uncommitted_blkids_[lsn] under the mutex.
        void on_blkids_written(raft_lsn_t lsn, BlkIds bids);

        // Promotes the entry's uncommitted BlkIds at `lsn` to committed via blob_stream_->commit_blks and
        // erases the lsn from uncommitted_blkids_.  Returns the promoted BlkIds (now app-owned; only
        // deferred_free brings them back).  Returns empty for inline entries or already-committed lsns.
        BlkIds on_commit(raft_lsn_t lsn);

        // Parses the indirect on-disk record carried in entry.bufs()[0] (9B nuraft hdr + ReplLogHdr +
        // user_header + BlkIds trailer), reads the value bytes from blob_stream_ into a fresh buf, flips
        // the header code to HS_DATA_INLINE, and installs the (hdr_buf, value_buf) chain via replace_chain.
        Async< void > reconstruct(nuraft::log_entry& entry);

        // If `referenced_lsn` is in [head_lsn, tail_lsn], records to free_blks_journal_ at that lsn +
        // pushes blkid into deferred_free_blkids_[lsn] and returns true.  Otherwise (referenced lsn is
        // already truncated / rolled back), free immediately via blob_stream_->invalidate_blk and return
        // false.
        Async< bool > deferred_free(BlkId blkid, raft_lsn_t referenced_lsn);

        // Drains uncommitted_blkids_ and deferred_free_blkids_ for lsn > to_lsn, rolls back
        // free_blks_journal_ to to_lsn, and bulk-invalidates the drained BlkIds against blob_stream_.
        // Atomic under indirect_mtx_.
        Async< void > rollback(raft_lsn_t to_lsn);

        // Drains deferred_free_blkids_ at or below upto_lsn, truncates free_blks_journal_ to upto_lsn,
        // and bulk-invalidates the drained BlkIds against blob_stream_.  Atomic under indirect_mtx_.
        Async< void > compact(raft_lsn_t upto_lsn);

        // Flushes free_blks_journal_ so any deferred-free records written since the previous CP become
        // durable.
        Async< void > cp_flush();

        // Invalidates every BlkId still tracked (uncommitted_blkids_ + deferred_free_blkids_) and
        // destroys the free_blks_journal LogStore.
        Async< void > destroy();

    private:
        shared< RawBlkStream > blob_stream_;
        shared< LogStore > free_blks_journal_;
        // Non-owning back-ref to the outer HomeRaftLogStore's main raft LogStore.  Lifetime is fine —
        // HomeRaftLogStore owns indirect_ and log_store_ as siblings; log_store_ outlives indirect_.
        LogStore* main_log_store_;

        std::mutex indirect_mtx_;
        // BlkIds allocated by write() for entries that have not yet been raft-committed.  Pure in-memory
        // bookkeeping — never persisted; the underlying BlkAllocator state has them in its uncommitted
        // bitmap.  on_commit promotes them to committed; rollback/destroy invalidates them.  Ordered by
        // lsn so rollback can range-walk via upper_bound.
        std::map< raft_lsn_t, BlkIds > uncommitted_blkids_;
        // App's deferred-free intents keyed by the referencing raft lsn — populated by deferred_free() and
        // by recovery from free_blks_journal_.  Multiple BlkIds can pile up against the same lsn.  Ordered
        // by lsn for upper_bound/lower_bound range walks in rollback/compact.
        std::map< raft_lsn_t, BlkIds > deferred_free_blkids_;
    };

private:
    HomeRaftLogStore(shared< LogStore > log_store, unique< IndirectBlkHandler > indirect);

    // Cache-then-read entry fetch.  Probes entry_cache_ first (via cache_lookup); on a miss it co_awaits
    // LogStore::read and assembles the entry.  When need_value=false on a cache miss, the indirect reconstruct
    // (blob_stream read) is skipped — the returned entry's bufs_[0] is the raw on-disk record and only the 9B
    // header (term + val_type) is meaningful.  Pass false if you only want headers to skip the blob read. If
    // the value is inline you still get the value; the skip applies only when the value is indirect.
    Async< RaftLogEntryPtr > fetch_entry(ulong index, bool need_value = true) const;

    // Non-suspending probe of entry_cache_ for `index`; nullopt on a miss.  Backs try_entry_at / try_term_at
    // and the fast path of fetch_entry.
    std::optional< RaftLogEntryPtr > cache_lookup(ulong index) const;

    // Inline-path LogBlob builder.  Walks the entry's chain into a LogBlob scatter-gather; if the chain
    // exceeds LogBlob::kMaxParts the bytes are coalesced into a single nuraft::buffer and stashed back on
    // the entry via replace_with_coalesced so the cached entry's bufs_[0] keeps the bytes alive across
    // in-flight flushes.
    LogBlob to_log_blob(RaftLogEntryPtr& entry);

    shared< LogStore > log_store_;
    // Mutable: fetch_entry is const (last_entry's nuraft contract is const) but the indirect path
    // invokes indirect_->reconstruct which mutates indirect_'s internal state.  The semantic is "scratch
    // work to materialize the read result" — const at the API boundary, non-const internally.
    mutable unique< IndirectBlkHandler > indirect_; // null iff large-value optimization off

    // Non-owning back-pointer set by ReplicaSet::set_raft_server once raft_server is constructed; ReplicaSet
    // is responsible for nulling it before raft_server destruction so any in-flight detached completions in
    // end_of_append_batch never deref a dead pointer.
    nuraft::raft_server* raft_server_{nullptr};

    RaftLogEntryPtr dummy_log_entry_;
    std::atomic< store_lsn_t > last_durable_lsn_{-1};

    // Single ring covering recent appended entries. Sized for nuraft's uncommitted window plus margin.
    // Writes (append / write_at) take cache_mtx_ unique; sync reads take it shared.
    mutable std::shared_mutex cache_mtx_;
    std::vector< std::pair< ulong, RaftLogEntryPtr > > entry_cache_;
};

static constexpr raft_lsn_t to_raft_lsn(store_lsn_t store_lsn) {
    return store_lsn + 1;
}
} // namespace homestore
