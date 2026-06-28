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
#include <iomgr/iomgr.hpp>
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
std::pair< folly::small_vector< RaftBufferPtr, 4 >, uint32_t > get_raft_value(nuraft::log_entry const& entry,
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

    unique< IndirectBlkHandler > indirect;
    if (blob_stream) {
        indirect = co_await IndirectBlkHandler::create(std::move(blob_stream), sb, log_store.get());
    }

    auto self = unique< HomeRaftLogStore >(new HomeRaftLogStore(std::move(log_store), std::move(indirect)));
    LOGINFOMOD(replication, "Created HomeRaftLogStore raft_store={} free_blks_store={} blob_opt={}",
               sb->raft_log_store_id, sb->free_blks_journal_id, self->indirect_ ? "on" : "off");
    co_return self;
}

folly::coro::Task< unique< HomeRaftLogStore > > HomeRaftLogStore::load(superblk< ReplicaSetSuperBlk >& sb,
                                                                       shared< RawBlkStream > blob_stream) {
    HS_REL_ASSERT_NE(sb->raft_log_store_id, UINT32_MAX, "load() called with no persisted raft_log_store_id");

    // Replay handler is a no-op here — fetch_entry_sync materializes entries lazily, and the recovery walk
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
        indirect = co_await IndirectBlkHandler::load(std::move(blob_stream), sb, log_store.get());
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

folly::coro::Task< void > HomeRaftLogStore::remove_store() {
    REPL_STORE_LOG(DEBUG, "Logstore is being physically removed");
    // TODO: free every BlkId still referenced (walk indirect_'s pending_blkids_ + any blkid_at across active
    // range) and physically delete both LogStores via LogStoreManager. Currently LogStoreManager exposes no
    // delete API; that's tracked in the broader LogStoreManager removal work.
    log_store_.reset();
    indirect_.reset();
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

ulong HomeRaftLogStore::append(RaftLogEntryPtr& entry) {
    REPL_STORE_LOG(TRACE, "append entry term={}, log_val_type={} size={}", entry->get_term(),
                   static_cast< uint32_t >(entry->get_val_type()), entry->total_size());

    // Threshold dispatch — large-value path delegates to indirect_->write which allocates the BlkId(s), pushes
    // value bytes to blob_stream_, builds the on-disk shim and stashes it on the entry as private_buf.  The
    // returned LogBlob points at the shim.  Inline path coalesces or scatters via to_log_blob.
    LogBlob lb =
        (indirect_ && entry->total_size() >= kLargeValueThreshold) ? indirect_->write(entry) : to_log_blob(entry);

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
    // Main log rollback first.  Once main_log_store_->tail_lsn() recedes, a concurrent deferred_free
    // taking indirect_mtx_ reads the post-rollback tail and correctly classifies referenced_lsn > (index-1)
    // as out-of-range (immediate free).  indirect_->rollback then prunes pending_blkids_ > (index-1) and
    // runs the free_blks_journal_ rollback under the mutex.  TODO: also walk [index, old_tail] and collect
    // each entry's blkid_at to free BlkIds embedded in the rolled-back entries themselves.
    folly::coro::blockingWait(log_store_->rollback(to_store_lsn(index) - 1));
    if (indirect_) {
        folly::coro::blockingWait(indirect_->rollback(static_cast< raft_lsn_t >(index) - 1));
    }

    // we need to reset the durable lsn, because its ok to set to lower number as it will be updated on next flush
    // calls, but it is dangerous to set higher number.
    last_durable_lsn_.store(-1, std::memory_order_release);

    LogBlob lb =
        (indirect_ && entry->total_size() >= kLargeValueThreshold) ? indirect_->write(entry) : to_log_blob(entry);

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
    auto end_repl_lsn = start + cnt - 1;
    auto end_store_lsn = to_store_lsn(end_repl_lsn);
    REPL_STORE_LOG(TRACE, "end_of_append_batch start={} cnt={} end_lsn={}", start, cnt, end_repl_lsn);

    // Detached drain — the proposer thread (the one nuraft called us from) is NEVER blocked here. The
    // detached coroutine awaits the LogStore's actual flush (which may also kick on LogStream's size /
    // timer auto-flush in parallel); once persisted, we update last_durable_lsn_ and notify the raft_server
    // so its durability_signal_ wakes any handle_append_entries coroutines waiting on this lsn.
    iomanager::spawn_detached(iomanager::ReactorTarget::any(),
                              [this, end_repl_lsn, end_store_lsn]() -> folly::coro::Task< void > {
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
    // Zero-copy: wrap the disk bytes as a nuraft::buffer via take_ownership; the deleter captures the
    // IoBufView by value (its base_buf_ shared_ptr keeps the underlying storage alive) so the log_entry's
    // bufs_[0] stays valid until the log_entry is dropped. log_entry::from_serialized then constructs an
    // entry whose first 9 bytes ARE the [term | val_type] header read straight from disk — zero copy
    // through the entire read path.
    auto* data = const_cast< uint8_t* >(byte_view.bytes());
    size_t size = byte_view.size();
    auto serialized = nuraft::buffer::take_ownership(
        data, size, [held = std::move(byte_view)](nuraft::byte*) noexcept { (void)held; });
    auto entry = nuraft::log_entry::from_serialized(serialized);

    // Indirect-record handling: ReplLogHdr sits immediately after nuraft's 9B preamble.  If the on-disk code
    // reads HS_DATA_INDIRECT, hand the entry to indirect_->reconstruct which parses the on-disk record's
    // [ReplLogHdr | user_header | BlkIds] from bufs_[0], reads value bytes from blob_stream_, and rebuilds
    // the entry's bufs_ in INLINE form (so wire-shipping presents the same shape regardless of the replica's
    // storage decision).  The on-disk record is parked on the entry as private_buf so its bytes outlive the
    // read.
    if (indirect_ && size > kFixedHeaderSize) {
        auto const* base = serialized->data_begin();
        auto const* hdr = r_cast< ReplLogHeader const* >(base + nuraft::log_entry::kHdrSize);
        if (hdr->code == to_u8(JournalType::HS_DATA_INDIRECT)) {
            entry->set_private_buf(std::move(serialized));
            folly::coro::blockingWait(indirect_->reconstruct(*entry));
        }
    }
    return entry;
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
        // We are asked to apply/insert data behind next slot, so we must rollback before index and then append.
        // Main log first, then indirect — same ordering as write_at / compact.
        folly::coro::blockingWait(log_store_->rollback(to_store_lsn(index) - 1));
        if (indirect_) {
            folly::coro::blockingWait(indirect_->rollback(static_cast< raft_lsn_t >(index) - 1));
        }
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
    // Main log truncate first.  Once main_log_store_->head_lsn() advances, a concurrent deferred_free
    // taking indirect_mtx_ reads the post-truncate head and correctly classifies referenced_lsn ≤
    // compact_lsn as already-gone (immediate free) instead of pushing into pending_blkids_ that we are
    // about to drain.  indirect_->compact then drains pending_blkids_ ≤ compact_lsn and runs the
    // free_blks_journal_ truncate under the mutex.
    folly::coro::blockingWait(log_store_->truncate(to_store_lsn(compact_lsn)));
    if (indirect_) {
        folly::coro::blockingWait(indirect_->compact(static_cast< raft_lsn_t >(compact_lsn)));
    }
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

void HomeRaftLogStore::set_last_durable_lsn(raft_lsn_t lsn) {
    last_durable_lsn_.store(to_store_lsn(lsn), std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Large-value extensions — stubs; bodies land in the follow-up indirect-path pass
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > HomeRaftLogStore::deferred_free(BlkId blkid, raft_lsn_t referenced_lsn) {
    if (!indirect_) {
        // optimization off; caller shouldn't be holding a BlkId in the first place.
        co_return;
    }
    co_await indirect_->deferred_free(blkid, referenced_lsn);
}

std::optional< BlkId > HomeRaftLogStore::blkid_at(ulong index) const {
    if (!indirect_) {
        return std::nullopt;
    }
    // Source of truth is the on-disk shim parked on the entry's private_buf during fetch_entry_sync (the
    // working bufs_ chain is rebuilt to INLINE form so it no longer carries the BlkIds).  Look up the
    // entry in the cache first; if there, inspect its private_buf().  Cache miss falls through to a
    // blockingWait fetch — which itself populates private_buf — and we re-inspect.
    auto position_in_cache = index % entry_cache_.size();
    RaftLogEntryPtr entry;
    {
        std::shared_lock lk(cache_mtx_);
        auto& nle = entry_cache_[position_in_cache];
        if (nle.first == index) {
            entry = nle.second;
        }
    }
    if (!entry) {
        entry = fetch_entry_sync(index);
    }
    auto pb = entry->private_buf();
    if (!pb) {
        return std::nullopt;
    }

    auto const* base = pb->data_begin();
    auto const size = to_u32(pb->size());
    if (size <= kFixedHeaderSize) {
        return std::nullopt;
    }
    auto const* hdr = r_cast< ReplLogHeader const* >(base + nuraft::log_entry::kHdrSize);
    if (hdr->code != to_u8(JournalType::HS_DATA_INDIRECT)) {
        return std::nullopt;
    }
    auto const header_size = kFixedHeaderSize + hdr->user_header_size();

    BlkIds bids;
    bids.deserialize(sisl::Blob{base + header_size, size - header_size});
    return bids.empty() ? std::nullopt : std::optional< BlkId >{bids.front()};
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

folly::coro::Task< unique< HomeRaftLogStore::IndirectBlkHandler > >
HomeRaftLogStore::IndirectBlkHandler::create(shared< RawBlkStream > blob_stream, superblk< ReplicaSetSuperBlk >& sb,
                                             LogStore* main_log_store) {
    auto fbj = co_await log_store_mgr().create_log_store(/*append_mode=*/false);
    sb->free_blks_journal_id = fbj->store_id();
    co_return unique< IndirectBlkHandler >(
        new IndirectBlkHandler(std::move(blob_stream), std::move(fbj), main_log_store));
}

folly::coro::Task< unique< HomeRaftLogStore::IndirectBlkHandler > >
HomeRaftLogStore::IndirectBlkHandler::load(shared< RawBlkStream > blob_stream, superblk< ReplicaSetSuperBlk > const& sb,
                                           LogStore* main_log_store) {
    // Local map populated by the on_log_found callback synchronously during the LogStream recovery walk
    // (driven by the orchestrator's LogStoreManager::recover()).  After the walk completes, the local map
    // is moved into the constructed handler.  head_lsn_/tail_lsn_ are NOT touched here — they're lazy-
    // seeded under indirect_mtx_ on the first deferred_free/compact/rollback that needs the window.
    std::map< raft_lsn_t, BlkIds > pending;
    auto fbj = log_store_mgr().open_log_store(
        sb->free_blks_journal_id, [&pending](lsn_t store_lsn, sisl::IoBufView const& bv) {
            // Record bytes are produced symmetrically by deferred_free via BlkIds::serialize.
            BlkIds bids;
            bids.deserialize(sisl::Blob{bv.bytes(), to_u32(bv.size())});
            pending[to_raft_lsn(static_cast< store_lsn_t >(store_lsn))] = std::move(bids);
        });
    if (!fbj) {
        throw std::runtime_error(
            fmt::format("IndirectBlkHandler::load: unknown free_blks_journal_id={}", sb->free_blks_journal_id));
    }

    auto self =
        unique< IndirectBlkHandler >(new IndirectBlkHandler(std::move(blob_stream), std::move(fbj), main_log_store));
    self->pending_blkids_ = std::move(pending);
    co_return self;
}

LogBlob HomeRaftLogStore::IndirectBlkHandler::write(RaftLogEntryPtr& entry) {
    // Peek the ReplLogHdr fixed prefix so we know value_size + user_header_size before any allocation.
    ReplLogHeader hdr;
    copy_entry_slice(*entry, to_u32(nuraft::log_entry::kHdrSize),
                     sisl::Blob{to_u8ptr(&hdr), to_u32(sizeof(ReplLogHeader))});
    auto const header_size = kFixedHeaderSize + hdr.user_header_size();
    auto const value_size = hdr.value_size;

    // 1. Allocate BlkIds for value_size only (user_header stays inline so a tiny app-header doesn't burn
    //    an extra blob_stream_ block).  alloc_blks copes with fragmentation by returning multiple
    //    non-contiguous BlkIds when one contiguous run is not available.
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

    // 5. Zero-copy SgList over entry's value-bytes chain — get_raft_value() returns the relevant nuraft
    //    buffers + first-buf offset; each becomes an IoBufSpan aliasing the underlying bytes.  The chain's
    //    shared_ptrs keep the nuraft buffers alive through the blockingWait.  writev_multi routes bytes per
    //    BlkId, and DriveInterface tail-pads the trailing BlkId to LBA-multiple internally.
    auto [value_bufs, first_offset] = get_raft_value(*entry, header_size);
    std::vector< sisl::IoBufSpan > spans;
    spans.reserve(value_bufs.size());
    sisl::SgList sg;
    for (size_t i = 0; i < value_bufs.size(); ++i) {
        auto const off = (i == 0) ? first_offset : 0u;
        auto const sz = to_u32(value_bufs[i]->size() - off);
        spans.emplace_back(value_bufs[i]->data_begin() + off, sz, /*is_aligned=*/false);
        sg.bufs.push_back(&spans.back());
    }
    folly::coro::blockingWait(blob_stream_->writev_multi(bids, sg, /*buffered=*/false));
    auto cp_guard = cp_mgr().cp_guard();
    blob_stream_->commit_blks(cp_guard.get(), bids);

    // 7. Park indirect_entry_buf on the entry as private_buf — bytes stay alive across the in-flight
    //    LogStream flush via the cached entry's private_buf.  The entry's wire-side bufs_ chain is left
    //    untouched so the entry replicates unchanged regardless of any peer's storage decision.
    entry->set_private_buf(indirect_entry_buf);
    return LogBlob{sisl::IoBufSpan{indirect_entry_buf->data_begin(), to_u32(header_size + encoded.size()),
                                   /*is_aligned=*/false}};
}

folly::coro::Task< void > HomeRaftLogStore::IndirectBlkHandler::reconstruct(nuraft::log_entry& entry) {
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

    // Value buf — read straight from blob_stream_, no intermediate copy.
    auto value_buf = nuraft::buffer::alloc(value_size);
    sisl::IoBufSpan value_dst{value_buf->data_begin(), value_size, /*is_aligned=*/false};
    co_await blob_stream_->read_multi(bids, value_dst);

    nuraft::log_entry_chain chain;
    chain.push_back(std::move(hdr_buf));
    chain.push_back(std::move(value_buf));
    entry.replace_chain(std::move(chain));
    co_return;
}

folly::coro::Task< bool > HomeRaftLogStore::IndirectBlkHandler::deferred_free(BlkId blkid, raft_lsn_t referenced_lsn) {
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
            pending_blkids_[referenced_lsn].push_back(blkid);
        }
    }
    if (!defer) {
        // Immediate free — referenced main log entry already truncated or rolled back, so no journal
        // record is needed.  Outside the mutex so a slow invalidate doesn't stall the critical section.
        auto cp_guard = cp_mgr().cp_guard();
        co_await blob_stream_->invalidate(cp_guard.get(), blkid);
    }
    co_return defer;
}

folly::coro::Task< void > HomeRaftLogStore::IndirectBlkHandler::rollback(raft_lsn_t to_lsn) {
    // Two phases: under the mutex collect BlkIds to free and prune pending_blkids_ + run the journal
    // rollback; outside the mutex invalidate the BlkIds asynchronously.  Journal rollback stays under the
    // mutex so it serializes with concurrent quick_writes from deferred_free.
    BlkIds to_free;
    {
        std::lock_guard lk{indirect_mtx_};
        auto it = pending_blkids_.upper_bound(to_lsn);
        for (auto cur = it; cur != pending_blkids_.end(); ++cur) {
            for (auto const& bid : cur->second) {
                to_free.push_back(bid);
            }
        }
        pending_blkids_.erase(it, pending_blkids_.end());
        folly::coro::blockingWait(free_blks_journal_->rollback(to_store_lsn(to_lsn)));
    }
    auto cp_guard = cp_mgr().cp_guard();
    for (auto const& bid : to_free) {
        co_await blob_stream_->invalidate(cp_guard.get(), bid);
    }
    co_return;
}

folly::coro::Task< void > HomeRaftLogStore::IndirectBlkHandler::compact(raft_lsn_t upto_lsn) {
    // Mirrors rollback's two-phase pattern: under the mutex collect BlkIds for entries ≤ upto_lsn and
    // truncate the journal; outside the mutex invalidate asynchronously.
    BlkIds to_free;
    {
        std::lock_guard lk{indirect_mtx_};
        auto end_it = pending_blkids_.upper_bound(upto_lsn);
        for (auto cur = pending_blkids_.begin(); cur != end_it; ++cur) {
            for (auto const& bid : cur->second) {
                to_free.push_back(bid);
            }
        }
        pending_blkids_.erase(pending_blkids_.begin(), end_it);
        folly::coro::blockingWait(free_blks_journal_->truncate(to_store_lsn(upto_lsn)));
    }
    auto cp_guard = cp_mgr().cp_guard();
    for (auto const& bid : to_free) {
        co_await blob_stream_->invalidate(cp_guard.get(), bid);
    }
    co_return;
}

folly::coro::Task< void > HomeRaftLogStore::IndirectBlkHandler::cp_flush() {
    co_await free_blks_journal_->flush();
}

} // namespace homestore
