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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

#include <folly/coro/Task.h>

#include "common/defs.h"
#include "homestore/base/blk.h"
#include "homestore/logstore/log_store.h"
#include "homestore/replication/repl_decls.h"
#include "homestore/replication/replica_set.h"
#include "homestore/superblk_handler.hpp"

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
// nuraft's API is fully synchronous. Sync reads (entry_at / term_at / last_entry / pack) hit an in-memory
// entry_cache_ on the recent uncommitted window; on miss they blockingWait LogStore::read. The transport
// routes slow RPCs (snapshot, sync_log, install_snapshot) to a CPU thread pool where this blocking is fine;
// hot RPCs (vote, append_entries, election) only ever touch the cached window.
class HomeRaftLogStore : public nuraft::log_store {
public:
    // First-boot create. Allocates a fresh main log_store; if blob_stream is non-null, also allocates a
    // free_blks log_store. Records both ids back into `sb` (caller persists via sb.write()).
    static folly::coro::Task< unique< HomeRaftLogStore > > create(superblk< ReplicaSetSuperBlk >& sb,
                                                                  shared< RawBlkStream > blob_stream);

    // Restart load. Opens existing log_stores from ids in `sb`; throws if sb.free_blks_journal_id is set but
    // blob_stream is null (app removed the optimization across restart but persisted state still needs it).
    static folly::coro::Task< unique< HomeRaftLogStore > > load(superblk< ReplicaSetSuperBlk >& sb,
                                                                shared< RawBlkStream > blob_stream);

    HomeRaftLogStore(HomeRaftLogStore const&) = delete;
    HomeRaftLogStore& operator=(HomeRaftLogStore const&) = delete;
    HomeRaftLogStore(HomeRaftLogStore&&) = delete;
    HomeRaftLogStore& operator=(HomeRaftLogStore&&) = delete;
    virtual ~HomeRaftLogStore() = default;

    folly::coro::Task< void > remove_store();

    /// The first available slot of the store, starts with 1.
    /// @return Last log index number + 1
    virtual ulong next_slot() const override;

    /// The start index of the log store, at the very beginning, it must be 1. However, after some compact
    /// actions, this could be anything greater or equals to one.
    /// @return Starting log index number.
    virtual ulong start_index() const override;

    /// The last log entry in store.
    /// @return If no log entry exists: a dummy constant entry with value set to null and term set to zero.
    virtual RaftLogEntryPtr last_entry() const override;

    /// Append a log entry to store
    /// @param entry Log entry
    /// @return Log index number.
    virtual ulong append(RaftLogEntryPtr& entry) override;

    // An alternate method on entries already serialized into the raft buffer
    ulong append(RaftBufferPtr& buffer);

    /// Overwrite a log entry at the given `index`.
    /// @param index Log index number to overwrite.
    /// @param entry New log entry to overwrite.
    virtual void write_at(ulong index, RaftLogEntryPtr& entry) override;

    // An alternate method on entries already serialized into the raft buffer
    void write_at(ulong index, RaftBufferPtr& buffer);

    /// Invoked after a batch of logs is written as a part of a single append_entries request.
    /// @param start The start log index number (inclusive)
    /// @param cnt The number of log entries written.
    virtual void end_of_append_batch(ulong start, ulong cnt) override;

    /// Get log entries with index [start, end). Return nullptr to indicate error if any log entry within the
    /// requested range could not be retrieved (e.g. due to external log truncation).
    /// @param start The start log index number (inclusive).
    /// @param end The end log index number (exclusive).
    /// @return The log entries between [start, end).
    virtual nuraft::ptr< std::vector< RaftLogEntryPtr > > log_entries(ulong start, ulong end) override;

    /// Get log entries with index [start, end). The total size of the returned entries is limited by
    /// batch_size_hint. Return nullptr to indicate error if any log entry within the requested range could not
    /// be retrieved (e.g. due to external log truncation).
    /// @param start The start log index number (inclusive).
    /// @param end The end log index number (exclusive).
    /// @param batch_size_hint_in_bytes Total size (in bytes) of the returned entries, see the detailed comment
    ///        at `state_machine::get_next_batch_size_hint_in_bytes()`.
    /// @return The log entries between [start, end) and limited by the total size given by the
    ///         batch_size_hint_in_bytes.
    virtual nuraft::ptr< std::vector< RaftLogEntryPtr > >
    log_entries_ext(ulong start, ulong end, int64_t batch_size_hint_in_bytes = 0) override;

    /// Get the log entry at the specified log index number.
    /// @param index Should be equal to or greater than 1.
    /// @return The log entry or null if index >= this->next_slot().
    virtual RaftLogEntryPtr entry_at(ulong index) override;

    /// Get the term for the log entry at the specified index. Suggest to stop the system if the index >=
    /// this->next_slot()
    /// @param index Should be equal to or greater than 1.
    /// @return The term for the specified log entry, or 0 if index < this->start_index().
    virtual ulong term_at(ulong index) override;

    /// Pack cnt log items starts from index
    /// @param index The start log index number (inclusive).
    /// @param cnt The number of logs to pack.
    /// @return log pack
    virtual RaftBufferPtr pack(ulong index, int32_t cnt) override;

    /// Apply the log pack to current log store, starting from index.
    /// @param index The start log index number (inclusive).
    /// @param pack
    virtual void apply_pack(ulong index, nuraft::buffer& pack) override;

    /// Compact the log store by purging all log entries, including the log at the last_log_index. If current
    /// max log idx is smaller than given `last_log_index`, set start log idx to `last_log_index + 1`.
    /// @param last_log_index Log index number that will be purged up to (inclusive).
    /// @return True on success.
    virtual bool compact(ulong last_log_index) override;

    /// Synchronously flush all log entries in this log store to the backing storage so that all log entries
    /// are guaranteed to be durable upon process crash.
    /// @return `true` on success.
    virtual bool flush() override;

    /// This API is used only when `raft_params::parallel_log_appending_` flag is set. Please refer to the
    /// comment of the flag. NOTE: In homestore replication use cases, we use this even without
    /// parallel_log_appending_ flag is not set
    /// @return The last durable log index.
    virtual ulong last_durable_index() override;

public:
    ///////////////////// All Additional methods specific to HomeRaftLogStore //////////////////////////

    /// Returns the last completed index in the log store.
    /// @return The last completed index in the log store.
    ulong last_index() const;

    /// Truncates the log store
    /// @param num_reserved_cnt The number of log entries to be reserved.
    /// @param compact_lsn This is the truncation barrier passed down by raft server. Truncation should not
    ///        across this LSN;
    void truncate(uint32_t num_reserved_cnt, raft_lsn_t compact_lsn);

    /// Purge all logs in the log store. It is a dangerous operation and to be used with care
    folly::coro::Task< void > purge_all_logs();

    void set_last_durable_lsn(raft_lsn_t lsn);

    /// Wire the owning raft_server. ReplicaSet calls this once raft_server has been constructed in
    /// join_group — chicken-and-egg with the state_mgr::load_log_store contract forces a post-construction
    /// set rather than a ctor arg. ReplicaSet MUST null this back via set_raft_server(nullptr) before
    /// raft_server is destroyed (ReplicaSet owns both, so it can guarantee the order).
    void set_raft_server(nuraft::raft_server* rs) { raft_server_ = rs; }

    /// Applicable only if large value optimization is turned on.
    /// App-driven free. If `referenced_lsn` is still in the main log (≥ start_index), the free is deferred
    /// until that lsn is truncated/rolled back; otherwise it happens immediately. Crash-safe across restart
    /// — deferred intents persist with the same durability semantics as the main log.
    folly::coro::Task< void > deferred_free(BlkId blkid, raft_lsn_t referenced_lsn);

    /// Applicable only if large value optimization is turned on.
    /// If the entry at `index` was written via the large-value optimization, return the BlkId where its value
    /// lives. nullopt for inline entries or when the optimization is off. Used by ReplicaSet::commit_ext_chained
    /// to surface blob_ref on the listener's on_commit.
    std::optional< BlkId > blkid_at(ulong index) const;

    // ─────────────────────────────────────────────────────────────────────────────────────────────────────
    // IndirectBlkHandler — nested helper that owns the large-value optimization machinery for this
    // HomeRaftLogStore.  When an entry's value payload is at or above the size threshold, the value bytes
    // are written to blob_stream_ and the journal record carries only a shim (marker + BlkId list) — this
    // keeps the journal compact and avoids round-tripping multi-MB values through it.
    //
    // Locking: indirect_mtx_ protects head_lsn_/tail_lsn_/pending_blkids_; mutations of free_blks_journal_
    // (quick_write, rollback, truncate, flush) happen OUTSIDE the lock because LogStore does NOT support
    // concurrent quick_append + rollback, and a slow journal write shouldn't stall the critical section.
    // ─────────────────────────────────────────────────────────────────────────────────────────────────────
    class IndirectBlkHandler {
    public:
        // First-boot factory — creates a fresh free_blks_journal LogStore (non-append mode).  Records the
        // assigned id back into sb so it survives restart.  Constructor can't be a coroutine, hence the
        // static factory pattern.  `main_log_store` is the outer HomeRaftLogStore's main raft LogStore;
        // borrowed non-owning (HomeRaftLogStore outlives indirect_), needed for lazy seed_window().
        static folly::coro::Task< unique< IndirectBlkHandler > >
        create(shared< RawBlkStream > blob_stream, superblk< ReplicaSetSuperBlk >& sb, LogStore* main_log_store);

        // Restart factory — opens the existing free_blks_journal LogStore using the id persisted in sb, and
        // registers an on_log_found callback that walks every persisted (lsn → BlkId list) record to
        // rebuild pending_blkids_ in memory.  After this returns the handler is fully replayed except for
        // head_lsn_/tail_lsn_ — those stay zero until the first deferred_free/compact/rollback lazy-seeds
        // them from main_log_store_ under indirect_mtx_.
        static folly::coro::Task< unique< IndirectBlkHandler > >
        load(shared< RawBlkStream > blob_stream, superblk< ReplicaSetSuperBlk > const& sb, LogStore* main_log_store);

        ~IndirectBlkHandler() = default;
        IndirectBlkHandler(IndirectBlkHandler const&) = delete;
        IndirectBlkHandler& operator=(IndirectBlkHandler const&) = delete;
        IndirectBlkHandler(IndirectBlkHandler&&) = delete;
        IndirectBlkHandler& operator=(IndirectBlkHandler&&) = delete;

        // Hot path — leader/follower entry write.  Writes the entry's value bytes (bufs_[2..]) to
        // blob_stream_, allocates the shim slab (nuraft hdr + ReplLogHdr + user_header + BlkId list, with
        // ReplLogHdr.code rewritten to HS_DATA_INDIRECT), attaches it via entry->set_private_buf().  Returns
        // a LogBlob that points at the shim (single contiguous part) for log_store_->quick_append.
        LogBlob write(RaftLogEntryPtr& entry);

        // For an on-disk entry whose ReplLogHdr.code reads HS_DATA_INDIRECT, reconstruct() parses the on-disk
        // record straight from entry's bufs_[0] (ReplLogHdr + user_header + BlkIds trailer), reads value bytes
        // from blob_stream_, builds a combined buffer with the header flipped to HS_DATA_INLINE, and installs
        // it via change_buf so the entry's chain matches the shape of an INLINE-written entry.
        folly::coro::Task< void > reconstruct(nuraft::log_entry& entry);

        // App's deferred free.  If `referenced_lsn` is in [head_lsn_, tail_lsn_], record to
        // free_blks_journal_ at that lsn + push BlkId into pending_blkids_[lsn]; otherwise (already
        // truncated / rolled back) free immediately via blob_stream_->invalidate.  Returns true if deferred.
        folly::coro::Task< bool > deferred_free(BlkId blkid, raft_lsn_t referenced_lsn);

        // Called by HomeRaftLogStore::rollback after the main log rolled back.  Frees BlkIds for pending
        // entries with lsn > to_lsn eagerly (deferred intent is moot), prunes pending_blkids_, updates
        // tail_lsn_ under the lock; drives free_blks_journal_->rollback(to_lsn) outside the lock.
        folly::coro::Task< void > rollback(raft_lsn_t to_lsn);

        // Called by HomeRaftLogStore::compact after the main log truncated up to upto_lsn.  Frees BlkIds for
        // pending entries at or below that lsn (referencing main lsn is gone), prunes pending_blkids_,
        // advances head_lsn_ under the lock; drives free_blks_journal truncate outside.
        folly::coro::Task< void > compact(raft_lsn_t upto_lsn);

        // CP hook — flushes free_blks_journal_ so any deferred-free records written since the previous CP
        // become durable with this CP boundary.  Driven by ReplicationManager's CPCallback.
        folly::coro::Task< void > cp_flush();

    private:
        // Used by the two static factories — already-opened free_blks_journal + the non-owning main log
        // back-ref handed in.  No cached head/tail — every deferred_free reads main_log_store_->head_lsn()
        // / tail_lsn() fresh under indirect_mtx_.  HomeRaftLogStore::compact and write_at order their main
        // log mutation BEFORE calling into indirect_, so the fresh reads under mtx see the post-mutation
        // values.
        IndirectBlkHandler(shared< RawBlkStream > blob_stream, shared< LogStore > free_blks_journal,
                           LogStore* main_log_store);

        shared< RawBlkStream > blob_stream_;
        shared< LogStore > free_blks_journal_;
        // Non-owning back-ref to the outer HomeRaftLogStore's main raft LogStore.  Lifetime is fine —
        // HomeRaftLogStore owns indirect_ and log_store_ as siblings; log_store_ outlives indirect_.
        LogStore* main_log_store_;

        std::mutex indirect_mtx_;
        // Pending deferred-free intents keyed by the referencing raft lsn.  Multiple BlkIds can pile up
        // against the same lsn (app may free several at once).  Ordered by lsn so rollback/compact can do
        // range walks via upper_bound / lower_bound.
        std::map< raft_lsn_t, BlkIds > pending_blkids_;
    };

private:
    HomeRaftLogStore(shared< LogStore > log_store, unique< IndirectBlkHandler > indirect);

    // Sync read that bridges nuraft's sync contract over the new LogStore's async read. Tries entry_cache_
    // first; on miss, blockingWait on LogStore::read and assemble. Only called via paths the transport routes
    // to slow_executor (or hot paths whose data is always cached).
    RaftLogEntryPtr fetch_entry_sync(ulong index) const;

    // Inline-path LogBlob builder.  Walks the entry's chain into a LogBlob scatter-gather; if the chain
    // exceeds LogBlob::kMaxParts the bytes are coalesced into a single nuraft::buffer and stashed back on
    // the entry via replace_with_coalesced so the cached entry's bufs_[0] keeps the bytes alive across
    // in-flight flushes.
    LogBlob to_log_blob(RaftLogEntryPtr& entry);

    shared< LogStore > log_store_;
    // Mutable: fetch_entry_sync is const (last_entry's nuraft contract is const) but the indirect path
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
