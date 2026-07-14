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
#include "common/async.h"

#include <cstring>
#include <stdexcept>

#include <iomgr/iomgr.hpp>

#include "iomanager/iomanager.h"
#include <iomgr/iomgr_flip.hpp>
#include <libnuraft/raft_server.hxx>

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

// Minimum prefix size for an indirect record — [9B nuraft | ReplLogHdr fixed prefix].  The actual on-disk
// header is variable: [9B nuraft | ReplLogHdr | user_header | BlkIds trailer], with user_header inline so
// a small app-header doesn't burn an extra blob_stream_ block of its own.
static constexpr uint32_t kFixedHeaderSize = to_u32(nuraft::log_entry::kHdrSize + sizeof(ReplLogHeader));

namespace {

// Walks `entry`'s bufs_ chain skipping the first `skip` bytes (logical chain offset) and copying up to
// `dst.size()` bytes into `dst.bytes()`.  Works for any bufs_ shape — coalesced (bufs_.size()==1, e.g.
// from_serialized) or multi-piece chain.  Caller knows the byte count it expects.
void copy_entry_slice(nuraft::log_entry const& entry, uint32_t skip, sisl::Blob& dst) {
    auto const capacity = dst.size();
    auto* out = dst.bytes();
    uint32_t copied = 0;
    uint32_t skipped = 0;
    for (auto const& b : entry.bufs()) {
        if (copied >= capacity) {
            break;
        }
        auto const b_size = to_u32(b->size());
        uint32_t off_in_buf = 0;
        if (skipped < skip) {
            auto skip_take = std::min(skip - skipped, b_size);
            skipped += skip_take;
            off_in_buf += skip_take;
            if (skipped < skip) {
                continue;
            }
        }
        auto avail = b_size - off_in_buf;
        auto take = std::min(capacity - copied, avail);
        std::memcpy(out + copied, b->data_begin() + off_in_buf, take);
        copied += take;
    }
}

// Decompose entry into the value-bytes chain — for the coalesced from_serialized form (bufs_.size()==1)
// this is bufs_[0] starting at `header_size`; for the chain form (bufs_.size()>1) this is bufs_[2..]
// starting at offset 0.  Returns (chain, first_offset) so callers can build an SgList of IoBufSpans
// without copying.
std::pair< folly::small_vector< RaftBufferPtr, 4 >, uint32_t > get_entry_value(nuraft::log_entry const& entry,
                                                                               uint32_t header_size) {
    folly::small_vector< nuraft::ptr< nuraft::buffer >, 4 > bufs;
    auto const& chain = entry.bufs();
    if (chain.size() == 1) {
        bufs.push_back(chain[0]);
        return {std::move(bufs), header_size};
    }
    for (size_t i = 2; i < chain.size(); ++i) {
        bufs.push_back(chain[i]);
    }
    return {std::move(bufs), 0u};
}

} // namespace

// raft_lsn (nuraft side) starts at 1; LogStore lsn starts at 0. Stay 1:1-shifted just like the legacy code.
static constexpr logstore_seq_num_t to_store_lsn(uint64_t raft_lsn) {
    return static_cast< logstore_seq_num_t >(raft_lsn - 1);
}
static constexpr logstore_seq_num_t to_store_lsn(raft_lsn_t raft_lsn) {
    return static_cast< logstore_seq_num_t >(raft_lsn - 1);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Static factories
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< unique< HomeRaftLogStore > > HomeRaftLogStore::create(superblk< ReplicaSetSuperBlk >& sb,
                                                             shared< RawBlkStream > blob_stream) {
    auto log_store = co_await log_store_mgr().create_log_store(/*append_mode=*/true);
    sb->raft_log_store_id = log_store->store_id();

    unique< IndirectBlkHandler > indirect;
    if (blob_stream) {
        auto fbj = co_await log_store_mgr().create_log_store(/*append_mode=*/false);
        sb->free_blks_journal_id = fbj->store_id();
        indirect = std::make_unique< IndirectBlkHandler >(std::move(blob_stream), std::move(fbj), log_store.get());
    }

    auto self = unique< HomeRaftLogStore >(new HomeRaftLogStore(std::move(log_store), std::move(indirect)));
    LOGINFOMOD(replication, "Created HomeRaftLogStore raft_store={} free_blks_store={} blob_opt={}",
               sb->raft_log_store_id, sb->free_blks_journal_id, self->indirect_ ? "on" : "off");
    co_return self;
}

Async< unique< HomeRaftLogStore > > HomeRaftLogStore::load(superblk< ReplicaSetSuperBlk >& sb,
                                                           shared< RawBlkStream > blob_stream) {
    HS_REL_ASSERT_NE(sb->raft_log_store_id, UINT32_MAX, "load() called with no persisted raft_log_store_id");

    // Replay handler is a no-op here — fetch_entry materializes entries lazily, and the recovery walk
    // happens internally inside LogStoreManager::recover() which Managers wired up before ReplicaSet load.
    auto log_store = log_store_mgr().open_log_store(sb->raft_log_store_id, [](lsn_t, sisl::IoBufView const&) {});
    if (!log_store) {
        throw std::runtime_error(
            fmt::format("HomeRaftLogStore::load: unknown raft_log_store_id={}", sb->raft_log_store_id));
    }

    unique< IndirectBlkHandler > indirect;
    if (sb->free_blks_journal_id != UINT32_MAX) {
        // sb says optimization was on at create time. If the listener now returns null blob_stream, the
        // persisted free_blks records reference BlkIds we have no way to free — bail rather than corrupt.
        HS_REL_ASSERT(blob_stream != nullptr,
                      "HomeRaftLogStore::load: free_blks_journal persisted but blob_stream "
                      "is null — app removed the large-value optimization across restart");
        indirect =
            std::make_unique< IndirectBlkHandler >(std::move(blob_stream), sb->free_blks_journal_id, log_store.get());
    } else if (blob_stream) {
        // Symmetric mismatch: app turned the optimization ON across restart but sb doesn't have a free_blks
        // store. Run inline-only for this group; the unused blob_stream just goes out of scope here.
        LOGWARNMOD(replication,
                   "HomeRaftLogStore::load: blob_stream provided but sb has no free_blks_journal_id — "
                   "disabling large-value optimization for this group");
    }

    auto self = unique< HomeRaftLogStore >(new HomeRaftLogStore(std::move(log_store), std::move(indirect)));
    LOGINFOMOD(replication, "Loaded HomeRaftLogStore raft_store={} free_blks_store={} blob_opt={}",
               sb->raft_log_store_id, sb->free_blks_journal_id, self->indirect_ ? "on" : "off");
    co_return self;
}

HomeRaftLogStore::HomeRaftLogStore(shared< LogStore > log_store, unique< IndirectBlkHandler > indirect) :
        log_store_{std::move(log_store)},
        indirect_{std::move(indirect)},
        // raft_lsn starts from 1, so we set lsn 0 to be dummy
        entry_cache_(100, std::make_pair(0, nullptr)) {
    dummy_log_entry_ = nuraft::cs_new< nuraft::log_entry >(0, nuraft::buffer::alloc(0), nuraft::log_val_type::app_log);
}

Async< void > HomeRaftLogStore::destroy() {
    REPL_STORE_LOG(DEBUG, "Logstore is being physically destroyed");
    if (indirect_) {
        co_await indirect_->destroy();
    }
    co_await log_store_mgr().destroy_log_store(log_store_->store_id());
    indirect_.reset();
    log_store_.reset();
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Index / size accessors
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ulong HomeRaftLogStore::next_slot() const {
    // tail_lsn is the last appended store_lsn (-1 when empty). next_slot is the next repl_lsn we'd assign.
    return to_ulong(log_store_->tail_lsn() + 2);
}

ulong HomeRaftLogStore::start_index() const {
    // start_index starts from 1. head_lsn is the lowest store_lsn still present.
    return std::max< ulong >(1, to_ulong(log_store_->head_lsn() + 1));
}

Async< RaftLogEntryPtr > HomeRaftLogStore::last_entry() const {
    store_lsn_t max_seq = log_store_->tail_lsn();
    if (max_seq < 0) {
        co_return dummy_log_entry_;
    }
    ulong lsn = to_ulong(max_seq + 1);
    co_return co_await fetch_entry(lsn);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Append / overwrite
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

LogBlob HomeRaftLogStore::to_log_blob(RaftLogEntryPtr& entry) {
    auto const& bufs = entry->bufs();
    LogBlob lb;
    if (LogBlob::can_build_trivially(bufs.size())) {
        for (auto const& b : bufs) {
            lb.append(sisl::IoBufSpan{b->data_begin(), to_u32(b->size()), /*is_aligned=*/false});
        }
        return lb;
    }
    auto coalesced = nuraft::buffer::alloc(entry->total_size());
    size_t off = 0;
    for (auto const& b : bufs) {
        std::memcpy(coalesced->data_begin() + off, b->data_begin(), b->size());
        off += b->size();
    }
    entry->replace_with_coalesced(coalesced);
    return LogBlob{sisl::IoBufSpan{coalesced->data_begin(), to_u32(coalesced->size()), /*is_aligned=*/false}};
}

Async< ulong > HomeRaftLogStore::append(RaftLogEntryPtr& entry) {
    REPL_STORE_LOG(TRACE, "append entry term={}, log_val_type={} size={}", entry->get_term(),
                   static_cast< uint32_t >(entry->get_val_type()), entry->total_size());

    // Threshold dispatch — large-value path delegates to indirect_->write which allocates the BlkId(s),
    // pushes value bytes to blob_stream_, builds the on-disk shim and stashes it on the entry as
    // private_buf.  The lsn isn't known until log_store_->quick_append assigns it — nuraft paths like
    // set_user_ctx / update_srv_config don't hold cli_lock_ across next_slot() and append, so a predicted
    // slot can be stolen.  Instead: write produces (LogBlob, BlkIds); quick_append returns the real lsn;
    // on_blkids_written records the mapping.
    LogBlob lb;
    BlkIds bids;
    bool const is_indirect = indirect_ && entry->total_size() >= kLargeValueThreshold;
    if (is_indirect) {
        std::tie(lb, bids) = co_await indirect_->write(entry);
    } else {
        lb = to_log_blob(entry);
    }

    auto const store_lsn = log_store_->quick_append(lb);
    ulong lsn = to_ulong(store_lsn + 1);

    if (is_indirect) {
        indirect_->on_blkids_written(static_cast< raft_lsn_t >(lsn), std::move(bids));
    }

    auto position_in_cache = lsn % entry_cache_.size();
    {
        std::unique_lock lk(cache_mtx_);
        entry_cache_[position_in_cache] = std::make_pair(lsn, entry);
    }
    co_return lsn;
}

Async< void > HomeRaftLogStore::write_at(ulong index, RaftLogEntryPtr& entry) {
    // Roll back the main log past index-1 first so concurrent deferred_free sees the post-rollback tail
    // and classifies a stale referenced_lsn correctly (immediate free).  Then roll back indirect_'s own
    // state (uncommitted_blkids_ release, deferred_free drain).
    co_await log_store_->rollback(to_store_lsn(index) - 1);
    if (indirect_) {
        co_await indirect_->rollback(static_cast< raft_lsn_t >(index) - 1);
    }

    // we need to reset the durable lsn, because its ok to set to lower number as it will be updated on next flush
    // calls, but it is dangerous to set higher number.
    last_durable_lsn_.store(-1, std::memory_order_release);

    LogBlob lb;
    BlkIds bids;
    bool const is_indirect = indirect_ && entry->total_size() >= kLargeValueThreshold;
    if (is_indirect) {
        std::tie(lb, bids) = co_await indirect_->write(entry);
    } else {
        lb = to_log_blob(entry);
    }

    log_store_->quick_append(lb);

    if (is_indirect) {
        indirect_->on_blkids_written(static_cast< raft_lsn_t >(index), std::move(bids));
    }

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
    auto end_repl_lsn = start + cnt - 1;
    auto end_store_lsn = to_store_lsn(end_repl_lsn);
    REPL_STORE_LOG(TRACE, "end_of_append_batch start={} cnt={} end_lsn={}", start, cnt, end_repl_lsn);

    // Detached drain — the proposer thread (the one nuraft called us from) is NEVER blocked here. The
    // detached coroutine awaits the LogStore's actual flush (which may also kick on LogStream's size /
    // timer auto-flush in parallel); once persisted, we update last_durable_lsn_ and notify the raft_server
    // so its durability_signal_ wakes any handle_append_entries coroutines waiting on this lsn.
    iomanager::spawn_detached(iomanager::ReactorTarget::any(), [this, end_repl_lsn, end_store_lsn]() -> Async< void > {
        co_await log_store_->flush();
        last_durable_lsn_.store(end_store_lsn, std::memory_order_release);
        if (raft_server_) {
            raft_server_->notify_durable(end_repl_lsn);
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Reads — cache then blockingWait on the new LogStore's async read
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

std::optional< RaftLogEntryPtr > HomeRaftLogStore::cache_lookup(ulong index) const {
    auto position_in_cache = index % entry_cache_.size();
    std::shared_lock lk(cache_mtx_);
    auto const& nle = entry_cache_[position_in_cache];
    if (nle.first == index) {
        return nle.second;
    }
    return std::nullopt;
}

Async< RaftLogEntryPtr > HomeRaftLogStore::fetch_entry(ulong index, bool need_value) const {
    if (auto cached = cache_lookup(index)) {
        co_return *cached;
    }
    // Cache miss — co_await the underlying LogStore read. Hot callers (election, append_resp uncommitted-window
    // walk) only ever touch lsns within the cache window so they never reach this branch; cold callers
    // (snapshot, sync_log) suspend here instead of blocking their reactor.
    auto byte_view = co_await log_store_->read(to_store_lsn(index));
    if (byte_view.size() == 0) {
        REPL_STORE_LOG(ERROR, "fetch_entry({}) out_of_range start={} end={}", index, start_index(),
                       to_ulong(log_store_->flushed_upto() + 1));
        co_return dummy_log_entry_;
    }

    // Zero-copy: wrap the disk bytes as a nuraft::buffer via take_ownership; the deleter captures the
    // IoBufView by value (its base_buf_ shared_ptr keeps the underlying storage alive) so the log_entry's
    // bufs_[0] stays valid until the log_entry is dropped. log_entry::from_serialized then constructs an
    // entry whose first 9 bytes ARE the [term | val_type] header read straight from disk — zero copy
    // through the entire read path.
    size_t size = byte_view.size();
    auto raft_buf =
        nuraft::buffer::take_ownership(const_cast< uint8_t* >(byte_view.bytes()), size,
                                       [held = std::move(byte_view)](nuraft::byte*) noexcept { (void)held; });
    auto entry = nuraft::log_entry::from_serialized(raft_buf);

    // Indirect-record handling: ReplLogHdr sits immediately after nuraft's 9B preamble.  If the on-disk code
    // reads HS_DATA_INDIRECT and the caller actually wants the value, hand the entry to indirect_->reconstruct
    // which parses the on-disk record's [ReplLogHdr | user_header | BlkIds] from bufs_[0], reads value bytes
    // from blob_stream_, and rebuilds the entry's bufs_ in INLINE form.  When need_value=false (e.g. term_at)
    // the blob read is skipped — caller will only inspect the 9B header.
    if (need_value && indirect_ && size > kFixedHeaderSize) {
        auto const* base = raft_buf->data_begin();
        auto const* hdr = r_cast< ReplLogHeader const* >(base + nuraft::log_entry::kHdrSize);
        if (hdr->code == to_u8(JournalType::HS_DATA_INDIRECT)) {
            entry->set_private_buf(std::move(raft_buf));
            co_await indirect_->reconstruct(*entry);
        }
    }
    co_return entry;
}

Async< RaftLogEntryPtr > HomeRaftLogStore::entry_at(ulong index) {
    co_return co_await fetch_entry(index);
}

std::optional< RaftLogEntryPtr > HomeRaftLogStore::try_entry_at(ulong index) {
    return cache_lookup(index);
}

Async< ulong > HomeRaftLogStore::term_at(ulong index) {
    auto entry = co_await fetch_entry(index, /*need_value=*/false);
    co_return entry->get_term();
}

std::optional< ulong > HomeRaftLogStore::try_term_at(ulong index) {
    if (auto cached = cache_lookup(index)) {
        return (*cached)->get_term();
    }
    return std::nullopt;
}

Async< nuraft::ptr< std::vector< RaftLogEntryPtr > > > HomeRaftLogStore::log_entries(ulong start, ulong end) {
    auto out_vec = std::make_shared< std::vector< RaftLogEntryPtr > >();
    out_vec->reserve(end - start);
    for (ulong i = start; i < end; ++i) {
        out_vec->emplace_back(co_await fetch_entry(i));
    }
    REPL_STORE_LOG(TRACE, "Num log entries start={} end={} num_entries={}", start, end, out_vec->size());
    co_return out_vec;
}

Async< nuraft::ptr< std::vector< RaftLogEntryPtr > > >
HomeRaftLogStore::log_entries_ext(ulong start, ulong end, int64_t batch_size_hint_in_bytes) {
    if (batch_size_hint_in_bytes < 0) {
        // Follower-busy signal — ship zero entries (NOT nullptr, which nuraft treats as retrieval failure).
        co_return std::make_shared< std::vector< RaftLogEntryPtr > >();
    } else if (batch_size_hint_in_bytes == 0) {
        // No limit — unbounded fetch.
        co_return co_await log_entries(start, end);
    } else {
        auto out_vec = std::make_shared< std::vector< RaftLogEntryPtr > >();
        auto const hint = to_u64(batch_size_hint_in_bytes);
        out_vec->reserve(end - start);
        uint64_t accumulated = 0;
        for (ulong i = start; i < end; ++i) {
            auto entry = co_await fetch_entry(i);
            accumulated += entry->total_size();
            out_vec->emplace_back(std::move(entry));
            if (accumulated >= hint) {
                break;
            }
        }
        REPL_STORE_LOG(TRACE, "log_entries_ext start={} end={} hint={} returned={} bytes={}", start, end, hint,
                       out_vec->size(), accumulated);
        co_return out_vec;
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Pack / apply_pack
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< RaftBufferPtr > HomeRaftLogStore::pack(ulong index, int32_t cnt) {
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
        auto entry = co_await fetch_entry(index + i);
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
    co_return out_buf;
}

Async< void > HomeRaftLogStore::apply_pack(ulong index, nuraft::buffer& pack) {
    pack.pos(0);
    auto num_entries = pack.get_int();

    auto slot = next_slot();
    if (index < slot) {
        // We are asked to apply/insert data behind next slot, so we must rollback before index and then append.
        // Main log first so concurrent deferred_free sees the post-rollback tail; then indirect_'s state.
        co_await log_store_->rollback(to_store_lsn(index) - 1);
        if (indirect_) {
            co_await indirect_->rollback(static_cast< raft_lsn_t >(index) - 1);
        }
    } else if (index > slot) {
        // We are asked to apply/insert data after next slot, so we need to fill in with dummy entries upto the slot
        // before append the entries
        REPL_STORE_LOG(WARN,
                       "RaftLogStore is asked to apply pack on lsn={}, but current lsn={} is behind, will be filling "
                       "with dummy data to make it functional, however, this could result in inconsistent data",
                       index, to_store_lsn(slot));
        while (index++ < slot) {
            co_await append(dummy_log_entry_);
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
        co_await this->append(nle);
        REPL_STORE_LOG(TRACE, "unpacking nth_entry={} of size={}, lsn={}", i + 1, entry_len, slot + i);
    }
    this->end_of_append_batch(slot, num_entries);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Compact / flush / durable
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< bool > HomeRaftLogStore::compact(ulong compact_lsn) {
    auto cur_max_lsn = log_store_->tail_lsn();
    if (cur_max_lsn < to_store_lsn(compact_lsn)) {
        // if compact_lsn is beyond the current max_lsn, it indicates a hole from cur_max_lsn to compact_lsn.
        // we directly compact and truncate up to compact_lsn assuming there are dummy logs.
        REPL_STORE_LOG(DEBUG, "Compact with log holes from {} to={}", cur_max_lsn + 1, to_store_lsn(compact_lsn));
    }
    // Main log truncate first.  Once main_log_store_->head_lsn() advances, a concurrent deferred_free
    // taking indirect_mtx_ reads the post-truncate head and correctly classifies referenced_lsn ≤
    // compact_lsn as already-gone (immediate free) instead of pushing into deferred_free_blkids_ that we are
    // about to drain.  indirect_->compact then drains deferred_free_blkids_ ≤ compact_lsn and runs the
    // free_blks_journal_ truncate under the mutex.
    co_await log_store_->truncate(to_store_lsn(compact_lsn));
    if (indirect_) {
        co_await indirect_->compact(static_cast< raft_lsn_t >(compact_lsn));
    }
    co_return true;
}

Async< bool > HomeRaftLogStore::flush() {
    co_await log_store_->flush();
    co_return true;
}

ulong HomeRaftLogStore::last_durable_index() {
    auto durable = log_store_->flushed_upto();
    last_durable_lsn_.store(durable, std::memory_order_release);
    return to_ulong(durable + 1);
}

Async< void > HomeRaftLogStore::purge_all_logs() {
    auto last_lsn = log_store_->tail_lsn();
    REPL_STORE_LOG(INFO, "Purging all logs in the log store, last_lsn={}", last_lsn);
    co_await log_store_->truncate(last_lsn);
}

void HomeRaftLogStore::set_last_durable_lsn(raft_lsn_t lsn) {
    last_durable_lsn_.store(to_store_lsn(lsn), std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Large-value extensions
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< void > HomeRaftLogStore::deferred_free(BlkId blkid, raft_lsn_t referenced_lsn) {
    if (!indirect_) {
        // optimization off; caller shouldn't be holding a BlkId in the first place.
        co_return;
    }
    co_await indirect_->deferred_free(blkid, referenced_lsn);
}

BlkIds HomeRaftLogStore::on_commit(raft_lsn_t lsn) {
    if (indirect_) {
        return indirect_->on_commit(lsn);
    }
    return {};
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//                                          IndirectBlkHandler
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

HomeRaftLogStore::IndirectBlkHandler::IndirectBlkHandler(shared< RawBlkStream > blob_stream,
                                                         shared< LogStore > free_blks_journal,
                                                         LogStore* main_log_store) :
        blob_stream_{std::move(blob_stream)},
        free_blks_journal_{std::move(free_blks_journal)},
        main_log_store_{main_log_store} {
}

HomeRaftLogStore::IndirectBlkHandler::IndirectBlkHandler(shared< RawBlkStream > blob_stream,
                                                         logstore_id_t free_blks_journal_id, LogStore* main_log_store) :
        blob_stream_{std::move(blob_stream)}, main_log_store_{main_log_store} {
    // The on_log_found callback fires asynchronously later when LogStoreManager::recover() walks the
    // journal records.  Capturing `this` is safe — the handler's lifetime extends past recover() because
    // HomeRaftLogStore (which owns this) outlives the LogStoreManager's reference to the callback.
    free_blks_journal_ =
        log_store_mgr().open_log_store(free_blks_journal_id, [this](lsn_t store_lsn, sisl::IoBufView const& bv) {
            BlkIds bids;
            bids.deserialize(sisl::Blob{bv.bytes(), to_u32(bv.size())});
            deferred_free_blkids_[to_raft_lsn(static_cast< store_lsn_t >(store_lsn))] = std::move(bids);
        });
    if (!free_blks_journal_) {
        throw std::runtime_error(
            fmt::format("IndirectBlkHandler load: unknown free_blks_journal_id={}", free_blks_journal_id));
    }
}

Async< std::pair< LogBlob, BlkIds > > HomeRaftLogStore::IndirectBlkHandler::write(RaftLogEntryPtr& entry) {
    // Peek the ReplLogHdr fixed prefix so we know value_size + user_header_size before any allocation.
    ReplLogHeader hdr;
    copy_entry_slice(*entry, to_u32(nuraft::log_entry::kHdrSize),
                     sisl::Blob{to_u8ptr(&hdr), to_u32(sizeof(ReplLogHeader))});
    auto const header_size = kFixedHeaderSize + hdr.user_header_size();
    auto const value_size = hdr.value_size;

    // 1. Allocate BlkIds for value_size only (user_header stays inline so a tiny app-header doesn't burn
    //    an extra blob_stream_ block).  alloc_blks copes with fragmentation by returning multiple
    //    non-contiguous BlkIds when one contiguous run is not available.  These blocks stay in the
    //    BlkAllocator's uncommitted state until on_blkids_written(lsn, bids) records them and on_commit(lsn)
    //    promotes them to committed.
    BlkIds bids;
    auto const alloc_status =
        blob_stream_->alloc_blks(blob_stream_->size_to_nblks(value_size), blk_alloc_hints{}, bids);
    HS_REL_ASSERT_EQ(alloc_status, BlkAllocStatus::SUCCESS, "blob_stream_ alloc_blks failed for value_size={}",
                     value_size);
    auto encoded = bids.serialize();

    // 2. Allocate the indirect entry buffer — exact size = [NuRaftHdr | ReplLogHdr | user_header | BlkIds trailer].
    auto indirect_entry_buf = nuraft::buffer::alloc(header_size + encoded.size());

    // 3. copy_header into indirect_entry_buf — [9B nuraft | ReplLogHdr | user_header].
    copy_entry_slice(*entry, 0, sisl::Blob{indirect_entry_buf->data_begin(), header_size});

    // 4. Flip ReplLogHdr.code to HS_DATA_INDIRECT, append serialized BlkIds right after the header.
    r_cast< ReplLogHeader* >(indirect_entry_buf->data_begin() + nuraft::log_entry::kHdrSize)->code =
        to_u8(JournalType::HS_DATA_INDIRECT);
    std::memcpy(indirect_entry_buf->data_begin() + header_size, encoded.bytes(), encoded.size());

    // 5. Zero-copy SgList over entry's value-bytes chain — get_entry_value() returns the relevant nuraft
    //    buffers + first-buf offset; each becomes an IoBufSpan aliasing the underlying bytes.  The chain's
    //    shared_ptrs (held on the coroutine frame) keep the nuraft buffers alive across the co_await.
    //    writev_multi routes bytes per BlkId, and DriveInterface tail-pads the trailing BlkId to LBA-multiple
    //    internally.
    auto [value_bufs, first_offset] = get_entry_value(*entry, header_size);
    std::vector< sisl::IoBufSpan > spans;
    spans.reserve(value_bufs.size());
    sisl::SgList sg;
    for (size_t i = 0; i < value_bufs.size(); ++i) {
        auto const off = (i == 0) ? first_offset : 0u;
        auto const sz = to_u32(value_bufs[i]->size() - off);
        spans.emplace_back(value_bufs[i]->data_begin() + off, sz, /*is_aligned=*/false);
        sg.bufs.push_back(&spans.back());
    }
    co_await blob_stream_->writev_multi(bids, sg, /*buffered=*/false);

    // 6. Park indirect_entry_buf on the entry as private_buf so its bytes stay alive across the in-flight
    //    LogStream flush.  The entry's wire-side bufs_ chain is left untouched so the entry replicates
    //    unchanged regardless of any peer's storage decision.
    entry->set_private_buf(indirect_entry_buf);
    LogBlob lb{sisl::IoBufSpan{indirect_entry_buf->data_begin(), to_u32(header_size + encoded.size()),
                               /*is_aligned=*/false}};
    co_return std::pair< LogBlob, BlkIds >{std::move(lb), std::move(bids)};
}

Async< void > HomeRaftLogStore::IndirectBlkHandler::reconstruct(nuraft::log_entry& entry) {
    // entry is in from_serialized state — bufs_[0] is the whole on-disk record
    // [9B nuraft | ReplLogHdr(INDIRECT) | user_header | BlkIds trailer].  Build two-buf chain:
    //   bufs_[0] = shrunk copy of the on-disk prefix [9B | ReplLogHdr(flipped to INLINE) | user_header]
    //              (excludes BlkIds trailer)
    //   bufs_[1] = freshly-allocated value buf, read directly from blob_stream_
    // Installed atomically via replace_chain — no coalescing, no value-bytes memcpy.
    auto const& current_bufs = entry.bufs();
    auto const* data_ptr = current_bufs[0]->data_begin();
    auto const size = to_u32(current_bufs[0]->size());

    auto const* rhdr = r_cast< ReplLogHeader const* >(data_ptr + nuraft::log_entry::kHdrSize);
    auto const user_header_size = rhdr->user_header_size();
    auto const header_size = kFixedHeaderSize + user_header_size;
    auto const value_size = rhdr->value_size;

    BlkIds bids;
    bids.deserialize(sisl::Blob{data_ptr + header_size, size - header_size});

    // Shrunk header buf — copies header_size bytes from the on-disk record (excludes BlkIds trailer),
    // flips the code byte in place.
    auto hdr_buf = nuraft::buffer::alloc(header_size);
    std::memcpy(hdr_buf->data_begin(), data_ptr, header_size);
    r_cast< ReplLogHeader* >(hdr_buf->data_begin() + nuraft::log_entry::kHdrSize)->code =
        to_u8(JournalType::HS_DATA_INLINE);

    // Read into a buf sized for the full BlkId-run (read_multi writes blk_count * blk_size per BlkId so
    // the buf has to cover total_blk_bytes), then alias the first value_size bytes for bufs_[1] — the
    // captured shared_ptr keeps read_buf alive, the trailing pad bytes stay allocated but are never read.
    auto const read_size = to_u32(bids.total_blks() * blob_stream_->block_size());
    auto read_buf = nuraft::buffer::alloc(read_size);
    sisl::IoBufSpan read_dst{read_buf->data_begin(), read_size, /*is_aligned=*/false};
    co_await blob_stream_->read_multi(bids, read_dst);

    auto value_buf = nuraft::buffer::take_ownership(read_buf->data_begin(), value_size,
                                                    [held = read_buf](nuraft::byte*) noexcept { (void)held; });

    nuraft::log_entry_chain chain;
    chain.push_back(std::move(hdr_buf));
    chain.push_back(std::move(value_buf));
    entry.replace_chain(std::move(chain));
    co_return;
}

Async< bool > HomeRaftLogStore::IndirectBlkHandler::deferred_free(BlkId blkid, raft_lsn_t referenced_lsn) {
    bool defer = false;
    {
        std::lock_guard lk{indirect_mtx_};
        // Authoritative window from main log under our mutex.  HomeRaftLogStore::compact and write_at do
        // their main-log mutation BEFORE calling into indirect_, so during the gap a deferred_free taking
        // indirect_mtx_ here sees the post-mutation head/tail and classifies correctly.
        defer = (referenced_lsn >= to_raft_lsn(main_log_store_->head_lsn())) &&
            (referenced_lsn <= to_raft_lsn(main_log_store_->tail_lsn()));
        if (defer) {
            // Encode as a single-BlkId BlkIds record + quick_write at the referenced lsn.  Still inside
            // the mutex because LogStore disallows concurrent quick_append + rollback, and our mutex is
            // the cross-op serialization point for free_blks_journal_.  quick_write is sync.
            BlkIds bids{blkid};
            auto encoded = bids.serialize();
            LogBlob lb{sisl::IoBufSpan{encoded.bytes(), to_u32(encoded.size()), /*is_aligned=*/false}};
            free_blks_journal_->quick_write(to_store_lsn(referenced_lsn), lb);
            deferred_free_blkids_[referenced_lsn].push_back(blkid);
        }
    }

    if (!defer) {
        // Immediate free — referenced main log entry already truncated or rolled back, so no journal
        // record is needed.  Outside the mutex so a slow invalidate doesn't stall the critical section.
        auto cp_guard = cp_mgr().cp_guard();
        co_await blob_stream_->invalidate_blk(cp_guard.get(), blkid);
    }
    co_return defer;
}

void HomeRaftLogStore::IndirectBlkHandler::on_blkids_written(raft_lsn_t lsn, BlkIds bids) {
    std::lock_guard lk{indirect_mtx_};
    uncommitted_blkids_[lsn] = std::move(bids);
}

BlkIds HomeRaftLogStore::IndirectBlkHandler::on_commit(raft_lsn_t lsn) {
    // Raft committed the entry at lsn — promote its uncommitted BlkIds to committed (CP-durable) and hand
    // them back to the caller so they can be surfaced to the listener's on_commit.  Past this point the
    // app owns them; we never free them on our own again.  Empty result for inline entries (not in
    // uncommitted_blkids_).
    BlkIds bids;
    {
        std::lock_guard lk{indirect_mtx_};
        auto it = uncommitted_blkids_.find(lsn);
        if (it == uncommitted_blkids_.end()) {
            return {};
        }
        bids = std::move(it->second);
        uncommitted_blkids_.erase(it);
    }
    auto cp_guard = cp_mgr().cp_guard();
    blob_stream_->commit_blks(cp_guard.get(), bids);
    return bids;
}

Async< void > HomeRaftLogStore::IndirectBlkHandler::rollback(raft_lsn_t to_lsn) {
    // Two BlkId sets to invalidate for entries in (to_lsn, tail]:
    //   (a) uncommitted_blkids_[lsn] — BlkIds allocated for entries that never raft-committed.
    //   (b) deferred_free_blkids_[lsn] — app deferred-free intents tagged to those lsns.
    // Under indirect_mtx_: drain both maps + rollback free_blks_journal_.  Outside the mutex: bulk
    // invalidate via blob_stream_->invalidate_blks.
    BlkIds to_free;
    {
        std::lock_guard lk{indirect_mtx_};

        // (a) Release our own uncommitted allocations.
        auto u_it = uncommitted_blkids_.upper_bound(to_lsn);
        for (auto cur = u_it; cur != uncommitted_blkids_.end(); ++cur) {
            for (auto const& bid : cur->second) {
                to_free.push_back(bid);
            }
        }
        uncommitted_blkids_.erase(u_it, uncommitted_blkids_.end());

        // (b) Drain app deferred-frees.
        auto d_it = deferred_free_blkids_.upper_bound(to_lsn);
        for (auto cur = d_it; cur != deferred_free_blkids_.end(); ++cur) {
            for (auto const& bid : cur->second) {
                to_free.push_back(bid);
            }
        }
        deferred_free_blkids_.erase(d_it, deferred_free_blkids_.end());

        iomanager::blocking_wait(free_blks_journal_->rollback(to_store_lsn(to_lsn)));
    }
    auto cp_guard = cp_mgr().cp_guard();
    co_await blob_stream_->invalidate_blks(cp_guard.get(), to_free);
    co_return;
}

Async< void > HomeRaftLogStore::IndirectBlkHandler::compact(raft_lsn_t upto_lsn) {
    // Mirrors rollback's two-phase pattern: under the mutex collect BlkIds for entries ≤ upto_lsn and
    // truncate the journal; outside the mutex invalidate asynchronously.
    BlkIds to_free;
    {
        std::lock_guard lk{indirect_mtx_};
        auto end_it = deferred_free_blkids_.upper_bound(upto_lsn);
        for (auto cur = deferred_free_blkids_.begin(); cur != end_it; ++cur) {
            for (auto const& bid : cur->second) {
                to_free.push_back(bid);
            }
        }
        deferred_free_blkids_.erase(deferred_free_blkids_.begin(), end_it);
        iomanager::blocking_wait(free_blks_journal_->truncate(to_store_lsn(upto_lsn)));
    }
    auto cp_guard = cp_mgr().cp_guard();
    co_await blob_stream_->invalidate_blks(cp_guard.get(), to_free);
    co_return;
}

Async< void > HomeRaftLogStore::IndirectBlkHandler::cp_flush() {
    co_await free_blks_journal_->flush();
}

Async< void > HomeRaftLogStore::IndirectBlkHandler::destroy() {
    // Drain everything we still own — both uncommitted (entries we wrote but were never raft-committed)
    // and deferred-free intents the app hadn't reaped yet — into a single list, then hand to blob_stream_
    // as one bulk invalidate.  RawBlkStream is agnostic to our committed/uncommitted distinction; the
    // distinction only matters inside this handler.
    BlkIds to_free;
    {
        std::lock_guard lk{indirect_mtx_};
        for (auto& [_, bids] : uncommitted_blkids_) {
            for (auto const& b : bids) {
                to_free.push_back(b);
            }
        }
        uncommitted_blkids_.clear();
        for (auto& [_, bids] : deferred_free_blkids_) {
            for (auto const& b : bids) {
                to_free.push_back(b);
            }
        }
        deferred_free_blkids_.clear();
    }
    auto cp_guard = cp_mgr().cp_guard();
    co_await blob_stream_->invalidate_blks(cp_guard.get(), to_free);
    co_await log_store_mgr().destroy_log_store(free_blks_journal_->store_id());
    free_blks_journal_.reset();
    co_return;
}

} // namespace homestore
