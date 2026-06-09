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
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

#include <folly/coro/Task.h>

#include "common/defs.h"
#include "homestore/base/blk.h"
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
    void truncate(uint32_t num_reserved_cnt, repl_lsn_t compact_lsn);

    /// Purge all logs in the log store. It is a dangerous operation and to be used with care
    folly::coro::Task< void > purge_all_logs();

    void set_last_durable_lsn(repl_lsn_t lsn);

    /// Applicable only if large value optimization is turned on.
    /// App-driven free. If `referenced_lsn` is still in the main log (≥ start_index), append a deferred-free
    /// record to free_blks_journal_ at the same lsn and return; the blob is freed later when main truncates
    /// or rolls back past that lsn. Otherwise free immediately via blob_stream_->invalidate.
    folly::coro::Task< void > deferred_free(BlkId blkid, ulong referenced_lsn);

    /// Applicable only if large value optimization is turned on.
    /// If the entry at `index` was written via the large-value optimization, return the BlkId where its value
    /// lives. nullopt for inline entries or when the optimization is off. Used by ReplicaSet::commit_ext_chained
    /// to surface blob_ref on the listener's on_commit.
    std::optional< BlkId > blkid_at(ulong index) const;

private:
    HomeRaftLogStore(shared< LogStore > log_store, shared< LogStore > free_blks_log_store,
                     shared< RawBlkStream > blob_stream);

    // Sync read that bridges nuraft's sync contract over the new LogStore's async read. Tries entry_cache_
    // first; on miss, blockingWait on LogStore::read and assemble. Only called via paths the transport routes
    // to slow_executor (or hot paths whose data is always cached).
    RaftLogEntryPtr fetch_entry_sync(ulong index) const;

    shared< LogStore > log_store_;
    shared< LogStore > free_blks_journal_; // null iff large-value optimization off
    shared< RawBlkStream > blob_stream_;   // null iff large-value optimization off

    RaftLogEntryPtr dummy_log_entry_;
    std::atomic< store_lsn_t > last_durable_lsn_{-1};

    // Single ring covering recent appended entries. Sized for nuraft's uncommitted window plus margin.
    // Writes (append / write_at) take cache_mtx_ unique; sync reads take it shared.
    mutable std::shared_mutex cache_mtx_;
    std::vector< std::pair< ulong, RaftLogEntryPtr > > entry_cache_;

    // Tracks lsns with active free_blks records so rollback can enumerate them without scanning the sparse
    // store slot-by-slot. Protected by free_blks_mtx_ because deferred_free / compact / rollback can run
    // concurrently across reactors.
    mutable std::mutex free_blks_mtx_;
    std::vector< ulong > free_blks_active_lsns_;
};

static constexpr repl_lsn_t to_repl_lsn(store_lsn_t store_lsn) {
    return store_lsn + 1;
}
} // namespace homestore
