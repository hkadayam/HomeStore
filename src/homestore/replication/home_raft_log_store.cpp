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

#include "home_raft_log_store.h"

#include <cstring>
#include <stdexcept>

#include <folly/coro/BlockingWait.h>
#include <iomgr/iomgr_flip.hpp>

#include "common/homestore_assert.h"
#include "homestore/blob/raw_blk_stream.h"
#include "homestore/homestore.h"
#include "homestore/logstore/log_store.h"
#include "homestore/logstore/log_store_mgr.h"
#include "homestore/managers.h"
#include "sisl/fds/utils.h"
#include "storage_engine_buffer.h"

using namespace homestore;

#define REPL_STORE_LOG(level, msg, ...)                                                                                \
    LOG##level##MOD_FMT(replication, ([&](fmt::memory_buffer& buf, const char* msgcb, auto&&... args) -> bool {        \
                            fmt::vformat_to(fmt::appender{buf}, fmt::string_view{"[{}:{}] "},                          \
                                            fmt::make_format_args(file_name(__FILE__), __LINE__));                     \
                            fmt::vformat_to(                                                                           \
                                fmt::appender{buf}, fmt::string_view{"[{}={}] "},                                      \
                                fmt::make_format_args("replstore", log_store_ ? log_store_->store_id() : UINT32_MAX)); \
                            fmt::vformat_to(fmt::appender{buf}, fmt::string_view{msgcb},                               \
                                            fmt::make_format_args(std::forward< decltype(args) >(args)...));           \
                            return true;                                                                               \
                        }),                                                                                            \
                        msg, ##__VA_ARGS__);

namespace homestore {

// Threshold above which an entry's value bytes are written to blob_stream_ instead of inline. Pinned, not
// configurable — see ReplApplication / ReplicaSetListener::blob_stream() for the per-group on/off switch.
static constexpr size_t kLargeValueThreshold = 4 * 1024;

// repl_lsn (nuraft side) starts at 1; LogStore lsn starts at 0. Stay 1:1-shifted just like the legacy code.
static constexpr logstore_seq_num_t to_store_lsn(uint64_t raft_lsn) {
    return static_cast< logstore_seq_num_t >(raft_lsn - 1);
}
static constexpr logstore_seq_num_t to_store_lsn(repl_lsn_t repl_lsn) {
    return static_cast< logstore_seq_num_t >(repl_lsn - 1);
}

// nuraft serializes a log_entry as: [term (8B)][val_type (1B)][has_crc32 (1B)][crc32 (4B)?][data ...]
// We only need the term for term_at() — first 8 bytes of the serialized payload.
static uint64_t extract_term(sisl::Blob const& blob) {
    return *r_cast< uint64_t const* >(blob.cbytes());
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Static factories
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

folly::coro::Task< unique< HomeRaftLogStore > > HomeRaftLogStore::create(superblk< ReplicaSetSuperBlk >& sb,
                                                                         shared< RawBlkStream > blob_stream) {
    auto log_store = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    sb->raft_log_store_id = log_store->store_id();

    shared< LogStore > free_blks_journal;
    if (blob_stream) {
        free_blks_journal = co_await log_store_mgr().create_log_store(/*append_mode=*/false);
        sb->free_blks_journal_id = free_blks_journal->store_id();
    }

    auto self = unique< HomeRaftLogStore >(
        new HomeRaftLogStore(std::move(log_store), std::move(free_blks_journal), std::move(blob_stream)));
    LOGINFOMOD(replication, "Created HomeRaftLogStore raft_store={} free_blks_store={} blob_opt={}",
               sb->raft_log_store_id, sb->free_blks_journal_id, self->blob_stream_ ? "on" : "off");
    co_return self;
}

folly::coro::Task< unique< HomeRaftLogStore > > HomeRaftLogStore::load(superblk< ReplicaSetSuperBlk >& sb,
                                                                       shared< RawBlkStream > blob_stream) {
    HS_REL_ASSERT_NE(sb->raft_log_store_id, UINT32_MAX, "load() called with no persisted raft_log_store_id");

    // Replay handler is a no-op here — fetch_entry_sync materializes entries lazily, and the recovery walk
    // happens internally inside LogStoreManager::recover() which Managers wired up before ReplicaSet load.
    auto log_store = log_store_mgr().open_log_store(sb->raft_log_store_id, [](lsn_t, sisl::ByteView const&) {});
    if (!log_store) {
        throw std::runtime_error(
            fmt::format("HomeRaftLogStore::load: unknown raft_log_store_id={}", sb->raft_log_store_id));
    }

    shared< LogStore > free_blks_journal;
    if (sb->free_blks_journal_id != UINT32_MAX) {
        // sb says optimization was on at create time. If the listener now returns null blob_stream, the
        // persisted free_blks records reference BlkIds we have no way to free — bail rather than corrupt.
        if (!blob_stream) {
            HS_REL_ASSERT(false,
                          "HomeRaftLogStore::load: free_blks_journal persisted but blob_stream "
                          "is null — app removed the large-value optimization across restart");
        }
        free_blks_journal =
            log_store_mgr().open_log_store(sb->free_blks_journal_id, [](lsn_t, sisl::ByteView const&) {});
        if (!free_blks_journal) {
            throw std::runtime_error(
                fmt::format("HomeRaftLogStore::load: unknown free_blks_journal_id={}", sb->free_blks_journal_id));
        }
    } else if (blob_stream) {
        // Symmetric mismatch: app turned the optimization ON across restart but sb doesn't have a free_blks
        // store. We can still run inline-only — drop blob_stream silently would be confusing; require app
        // restart with matching state. Easier to log+disable than fail outright.
        LOGWARNMOD(replication,
                   "HomeRaftLogStore::load: blob_stream provided but sb has no free_blks_journal_id — "
                   "disabling large-value optimization for this group");
        blob_stream.reset();
    }

    auto self = unique< HomeRaftLogStore >(
        new HomeRaftLogStore(std::move(log_store), std::move(free_blks_journal), std::move(blob_stream)));
    LOGINFOMOD(replication, "Loaded HomeRaftLogStore raft_store={} free_blks_store={} blob_opt={}",
               sb->raft_log_store_id, sb->free_blks_journal_id, self->blob_stream_ ? "on" : "off");
    co_return self;
}

HomeRaftLogStore::HomeRaftLogStore(shared< LogStore > log_store, shared< LogStore > free_blks_journal,
                                   shared< RawBlkStream > blob_stream) :
        log_store_{std::move(log_store)},
        free_blks_journal_{std::move(free_blks_journal)},
        blob_stream_{std::move(blob_stream)},
        // repl_lsn starts from 1, so we set lsn 0 to be dummy
        entry_cache_(100, std::make_pair(0, nullptr)) {
    dummy_log_entry_ = nuraft::cs_new< nuraft::log_entry >(0, nuraft::buffer::alloc(0), nuraft::log_val_type::app_log);
}

folly::coro::Task< void > HomeRaftLogStore::remove_store() {
    REPL_STORE_LOG(DEBUG, "Logstore is being physically removed");
    // TODO: free every BlkId still referenced (walk free_blks_journal_ + any blkid_at across active range)
    // and physically delete both LogStores via LogStoreManager. Currently LogStoreManager exposes no delete
    // API; that's tracked in the broader LogStoreManager removal work.
    log_store_.reset();
    free_blks_journal_.reset();
    blob_stream_.reset();
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Index / size accessors
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ulong HomeRaftLogStore::next_slot() const {
    // tail_lsn is the last appended store_lsn (-1 when empty). next_slot is the next repl_lsn we'd assign.
    return to_ulong(log_store_->tail_lsn() + 2);
}

ulong HomeRaftLogStore::last_index() const {
    // last_index is the highest committed repl_lsn (= flushed_upto + 1).
    return to_ulong(log_store_->flushed_upto() + 1);
}

ulong HomeRaftLogStore::start_index() const {
    // start_index starts from 1. head_lsn is the lowest store_lsn still present.
    return std::max< ulong >(1, to_ulong(log_store_->head_lsn() + 1));
}

RaftLogEntryPtr HomeRaftLogStore::last_entry() const {
    store_lsn_t max_seq = log_store_->tail_lsn();
    if (max_seq < 0) {
        return dummy_log_entry_;
    }
    ulong lsn = to_ulong(max_seq + 1);
    return fetch_entry_sync(lsn);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Append / overwrite
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ulong HomeRaftLogStore::append(RaftLogEntryPtr& entry) {
    REPL_STORE_LOG(TRACE, "append entry term={}, log_val_type={} size={}", entry->get_term(),
                   static_cast< uint32_t >(entry->get_val_type()), entry->total_size());

    // TODO: large-value indirect path — if entry size ≥ kLargeValueThreshold and blob_stream_ is set,
    // alloc a BlkId, write the value bytes to blob_stream_, replace the journal payload with a 13-byte
    // indirect record {kIndirectMarker, BlkId, original_size}. Wired in a follow-up pass.

    // Build a LogBlob over the entry's chain (bufs_[0]=hdr + bufs_[1..]=value parts). Fast path when the
    // chain fits inline; otherwise coalesce into a single contiguous nuraft::buffer and stash it on the
    // entry (replace_with_coalesced) so the cached entry's bufs_[0] keeps the bytes alive across in-flight
    // flushes.
    auto const& bufs = entry->bufs();
    LogBlob lb;
    if (LogBlob::can_build_trivially(bufs.size())) {
        for (auto const& b : bufs) {
            lb.append(sisl::IoBlob{b->data_begin(), to_u32(b->size()), /*is_aligned=*/false});
        }
    } else {
        auto coalesced = nuraft::buffer::alloc(entry->total_size());
        size_t off = 0;
        for (auto const& b : bufs) {
            std::memcpy(coalesced->data_begin() + off, b->data_begin(), b->size());
            off += b->size();
        }
        entry->replace_with_coalesced(coalesced);
        lb = LogBlob{sisl::IoBlob{coalesced->data_begin(), to_u32(coalesced->size()), /*is_aligned=*/false}};
    }

    auto const store_lsn = log_store_->quick_append(lb);
    ulong lsn = to_ulong(store_lsn + 1);

    auto position_in_cache = lsn % entry_cache_.size();
    {
        std::unique_lock lk(cache_mtx_);
        entry_cache_[position_in_cache] = std::make_pair(lsn, entry);
    }
    return lsn;
}

void HomeRaftLogStore::write_at(ulong index, RaftLogEntryPtr& entry) {
    folly::coro::blockingWait(log_store_->rollback(to_store_lsn(index) - 1));

    // we need to reset the durable lsn, because its ok to set to lower number as it will be updated on next flush
    // calls, but it is dangerous to set higher number.
    last_durable_lsn_.store(-1, std::memory_order_release);

    // Build a LogBlob over the entry's chain — same scatter-gather/coalesce flow as append().
    auto const& bufs = entry->bufs();
    LogBlob lb;
    if (LogBlob::can_build_trivially(bufs.size())) {
        for (auto const& b : bufs) {
            lb.append(sisl::IoBlob{b->data_begin(), to_u32(b->size()), /*is_aligned=*/false});
        }
    } else {
        auto coalesced = nuraft::buffer::alloc(entry->total_size());
        size_t off = 0;
        for (auto const& b : bufs) {
            std::memcpy(coalesced->data_begin() + off, b->data_begin(), b->size());
            off += b->size();
        }
        entry->replace_with_coalesced(coalesced);
        lb = LogBlob{sisl::IoBlob{coalesced->data_begin(), to_u32(coalesced->size()), /*is_aligned=*/false}};
    }

    log_store_->quick_append(lb);

    auto position_in_cache = index % entry_cache_.size();
    {
        std::unique_lock lk(cache_mtx_);
        entry_cache_[position_in_cache] = std::make_pair(index, entry);

        // remove all cached entries after this index
        for (size_t i{0}; i < entry_cache_.size(); ++i) {
            if (entry_cache_[i].first > index) {
                entry_cache_[i] = std::make_pair(0, nullptr);
            }
        }
    }

    // flushing the log before returning to ensure new(over-written) log is persisted to disk.
    end_of_append_batch(index, 1);
}

void HomeRaftLogStore::end_of_append_batch(ulong start, ulong cnt) {
    folly::coro::blockingWait(log_store_->flush());
    auto end_lsn = to_store_lsn(start + cnt - 1);
    last_durable_lsn_.store(end_lsn, std::memory_order_release);
    REPL_STORE_LOG(TRACE, "end_of_append_batch flushed upto start={} cnt={} lsn={}", start, cnt, start + cnt - 1);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Reads — cache then blockingWait on the new LogStore's async read
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

RaftLogEntryPtr HomeRaftLogStore::fetch_entry_sync(ulong index) const {
    auto position_in_cache = index % entry_cache_.size();
    {
        std::shared_lock lk(cache_mtx_);
        auto nle = entry_cache_[position_in_cache];
        if (nle.first == index) {
            return nle.second;
        }
    }
    // Cache miss — block on the underlying LogStore read. Transport routes slow callers (snapshot, sync_log)
    // onto the slow_executor where blocking is safe; hot callers (election, append_resp uncommitted-window
    // walk) only ever touch lsns within the cache window so they never reach this branch.
    auto byte_view = folly::coro::blockingWait(log_store_->read(to_store_lsn(index)));
    if (byte_view.size() == 0) {
        REPL_STORE_LOG(ERROR, "fetch_entry_sync({}) out_of_range start={} end={}", index, start_index(), last_index());
        return dummy_log_entry_;
    }
    // TODO: indirect-record handling — if byte_view starts with kIndirectMarker, fetch the BlkId from
    // blob_stream_ and assemble the full log_entry. Inline path only for now.
    //
    // Zero-copy: wrap the disk bytes as a nuraft::buffer via take_ownership; the deleter captures the
    // ByteView by value (its base_buf_ shared_ptr keeps the underlying storage alive) so the log_entry's
    // bufs_[0] stays valid until the log_entry is dropped. log_entry::from_serialized then constructs an
    // entry whose first 9 bytes ARE the [term | val_type] header read straight from disk — zero copy
    // through the entire read path.
    auto* data = const_cast< uint8_t* >(byte_view.bytes());
    size_t size = byte_view.size();
    auto serialized = nuraft::buffer::take_ownership(
        data, size, [held = std::move(byte_view)](nuraft::byte*) noexcept { (void)held; });
    return nuraft::log_entry::from_serialized(std::move(serialized));
}

RaftLogEntryPtr HomeRaftLogStore::entry_at(ulong index) {
    return fetch_entry_sync(index);
}

ulong HomeRaftLogStore::term_at(ulong index) {
    auto position_in_cache = index % entry_cache_.size();
    {
        std::shared_lock lk(cache_mtx_);
        auto nle = entry_cache_[position_in_cache];
        if (nle.first == index) {
            return nle.second->get_term();
        }
    }
    return fetch_entry_sync(index)->get_term();
}

nuraft::ptr< std::vector< RaftLogEntryPtr > > HomeRaftLogStore::log_entries(ulong start, ulong end) {
    auto out_vec = std::make_shared< std::vector< RaftLogEntryPtr > >();
    out_vec->reserve(end - start);
    for (ulong i = start; i < end; ++i) {
        out_vec->emplace_back(fetch_entry_sync(i));
    }
    REPL_STORE_LOG(TRACE, "Num log entries start={} end={} num_entries={}", start, end, out_vec->size());
    return out_vec;
}

nuraft::ptr< std::vector< RaftLogEntryPtr > > HomeRaftLogStore::log_entries_ext(ulong start, ulong end,
                                                                                int64_t batch_size_hint_in_bytes) {
    // WARNING: we interpret batch_size_hint_in_bytes as count as of now.
    auto batch_size_hint_cnt = batch_size_hint_in_bytes;
    auto new_end = end;
    // batch_size_hint_in_bytes < 0 indicates that follower is busy now and do not want to receive any more log entry.
    if (batch_size_hint_cnt < 0) {
        new_end = start;
    } else if (batch_size_hint_cnt > 0) {
        // limit to the hint, also prevent overflow by a huge batch_size_hint_cnt
        if (sisl_unlikely(start + (uint64_t)batch_size_hint_cnt < start)) {
            new_end = end;
        } else {
            new_end = start + (uint64_t)batch_size_hint_cnt;
        }
        // limit to original end
        new_end = std::min(new_end, end);
    }
    DEBUG_ASSERT(new_end <= end, "new end {} should be <= original end {}", new_end, end);
    DEBUG_ASSERT(start <= new_end, "start {} should be <= new_end {}", start, new_end);
    REPL_STORE_LOG(TRACE, "log_entries_ext, start={} end={}, hint {}, adjusted range {} ~ {}, cnt {}", start, end,
                   batch_size_hint_cnt, start, new_end, new_end - start);
    return log_entries(start, new_end);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Pack / apply_pack
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

RaftBufferPtr HomeRaftLogStore::pack(ulong index, int32_t cnt) {
    static constexpr size_t estimated_record_size = 128;
    size_t estimated_size = cnt * estimated_record_size + sizeof(uint32_t);

    //   << Format >>
    // # records (N)        4 bytes
    // +---
    // | log length (X)     4 bytes
    // | log data           X bytes
    // +--- repeat N
    RaftBufferPtr out_buf = nuraft::buffer::alloc(estimated_size);
    out_buf->put(cnt);

    for (int32_t i = 0; i < cnt; ++i) {
        auto entry = fetch_entry_sync(index + i);
        auto serialized = entry->serialize();
        size_t const total_entry_size = serialized->size() + sizeof(uint32_t);
        size_t avail_size = out_buf->size() - out_buf->pos();
        // available size of packing buffer should be able to hold entry.size() and the length of this entry
        if (avail_size < total_entry_size) {
            avail_size += std::max(out_buf->size() * 2, total_entry_size);
            out_buf = nuraft::buffer::expand(*out_buf, avail_size);
        }
        REPL_STORE_LOG(TRACE, "packing lsn={} of size={}, avail_size in buffer={}", index + i, serialized->size(),
                       avail_size);
        out_buf->put(serialized->data_begin(), serialized->size());
    }
    return out_buf;
}

void HomeRaftLogStore::apply_pack(ulong index, nuraft::buffer& pack) {
    pack.pos(0);
    auto num_entries = pack.get_int();

    auto slot = next_slot();
    if (index < slot) {
        // We are asked to apply/insert data behind next slot, so we must rollback before index and then append
        folly::coro::blockingWait(log_store_->rollback(to_store_lsn(index) - 1));
    } else if (index > slot) {
        // We are asked to apply/insert data after next slot, so we need to fill in with dummy entries upto the slot
        // before append the entries
        REPL_STORE_LOG(WARN,
                       "RaftLogStore is asked to apply pack on lsn={}, but current lsn={} is behind, will be filling "
                       "with dummy data to make it functional, however, this could result in inconsistent data",
                       index, to_store_lsn(slot));
        while (index++ < slot) {
            append(dummy_log_entry_);
        }
    }

    for (int i{0}; i < num_entries; ++i) {
        size_t entry_len;
        auto* entry = pack.get_bytes(entry_len);
        // Cold path (snapshot install). pack is a raw nuraft::buffer& we can't extend the lifetime of, and
        // the constructed log_entry gets cached in entry_cache_ inside append() — so we copy into a fresh
        // nuraft::buffer that the entry owns outright. The copy bytes ARE already in serialized form
        // [term | val_type | value], so from_serialized wraps them directly into bufs_[0].
        auto copy_buf = nuraft::buffer::alloc(entry_len);
        std::memcpy(copy_buf->data_begin(), entry, entry_len);
        auto nle = nuraft::log_entry::from_serialized(std::move(copy_buf));
        this->append(nle);
        REPL_STORE_LOG(TRACE, "unpacking nth_entry={} of size={}, lsn={}", i + 1, entry_len, slot + i);
    }
    this->end_of_append_batch(slot, num_entries);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Compact / flush / durable
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

bool HomeRaftLogStore::compact(ulong compact_lsn) {
    auto cur_max_lsn = log_store_->tail_lsn();
    if (cur_max_lsn < to_store_lsn(compact_lsn)) {
        // if compact_lsn is beyond the current max_lsn, it indicates a hole from cur_max_lsn to compact_lsn.
        // we directly compact and truncate up to compact_lsn assuming there are dummy logs.
        REPL_STORE_LOG(DEBUG, "Compact with log holes from {} to={}", cur_max_lsn + 1, to_store_lsn(compact_lsn));
    }
    // TODO: process free_blks_journal_ entries for store_lsn ≤ compact_lsn — free the listed BlkIds via
    // blob_stream_->invalidate, then truncate(free_blks_journal_) to the same lsn. Wired in a follow-up pass.
    folly::coro::blockingWait(log_store_->truncate(to_store_lsn(compact_lsn)));
    return true;
}

bool HomeRaftLogStore::flush() {
    folly::coro::blockingWait(log_store_->flush());
    return true;
}

ulong HomeRaftLogStore::last_durable_index() {
    auto durable = log_store_->flushed_upto();
    last_durable_lsn_.store(durable, std::memory_order_release);
    return to_ulong(durable + 1);
}

folly::coro::Task< void > HomeRaftLogStore::purge_all_logs() {
    auto last_lsn = log_store_->tail_lsn();
    REPL_STORE_LOG(INFO, "Purging all logs in the log store, last_lsn={}", last_lsn);
    co_await log_store_->truncate(last_lsn);
}

void HomeRaftLogStore::set_last_durable_lsn(repl_lsn_t lsn) {
    last_durable_lsn_.store(to_store_lsn(lsn), std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Large-value extensions — stubs; bodies land in the follow-up indirect-path pass
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > HomeRaftLogStore::deferred_free(BlkId blkid, ulong referenced_lsn) {
    (void)referenced_lsn;
    if (!blob_stream_) {
        // optimization off; caller shouldn't be holding a BlkId in the first place.
        co_return;
    }
    // TODO: if referenced_lsn ≥ start_index(), encode the BlkId and quick_write to free_blks_journal_ at the
    // same lsn (track in free_blks_active_lsns_ under free_blks_mtx_). Else, immediate-free via
    // blob_stream_->invalidate.
    (void)blkid;
    co_return;
}

std::optional< BlkId > HomeRaftLogStore::blkid_at(ulong index) const {
    if (!blob_stream_) {
        return std::nullopt;
    }
    // TODO: peek the cache slot for `index`; if its serialized buf starts with kIndirectMarker, extract and
    // return the BlkId. Else nullopt.
    (void)index;
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Legacy alternate append/write_at overloads (raw RaftBufferPtr) — kept for parity, mostly used by apply_pack
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ulong HomeRaftLogStore::append(RaftBufferPtr& buffer) {
    // RaftBufferPtr is nuraft::ptr<nuraft::buffer> — already a shared_ptr; from_serialized takes it directly
    // (bufs_[0] = buffer, where the first 9 bytes are the [term | val_type] header). Zero copy.
    auto entry = nuraft::log_entry::from_serialized(buffer);
    return append(entry);
}

void HomeRaftLogStore::write_at(ulong index, RaftBufferPtr& buffer) {
    auto entry = nuraft::log_entry::from_serialized(buffer);
    write_at(index, entry);
}

} // namespace homestore
