/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/async.h"
#include "sisl/fds/buffer.h"
#include "sisl/fds/stream_tracker.h"

#include "homestore/base/homestore_decl.h"
#include "homestore/logstore/log_stream.h"
#include "homestore/meta/meta_blk.h"

namespace homestore {

class MetaClient;

// One rollback event: records the lsn-tail position and the highest log_id that existed at the time of the
// rollback. A record on disk is suppressed at replay if its lsn falls above above_lsn AND its log_id is ≤
// max_log_id (i.e. it was written BEFORE the rollback). New writes after the rollback get fresh log_ids
// strictly greater than max_log_id, so they replay normally even when they land on the same lsn the rollback
// invalidated. Handles sparse / out-of-order non-append-mode writes without needing multiple logid sub-ranges.
struct rollback_record {
    lsn_t above_lsn{0};
    logid_t max_log_id{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Persisted per-store sb (single MetaBlk per LogStore: "<dev>_logstore_sb_<store_id>")
//
// Layout: { store_id, append_mode, head_lsn, n_rollback_records, rollback_records[n_rollback_records] }.  The
// trailing array is variable-size — same trick as AppendByteStreamSb's chunk_ids[].
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(1)
struct LogStoreSb {
    logstore_id_t store_id{0};
    uint8_t append_mode{0};
    uint8_t _reserved[3]{};
    lsn_t head_lsn{0};
    uint32_t n_rollback_records{0};
    // followed by rollback_record rollback_records[n_rollback_records]

    rollback_record* rollback_records() { return r_cast< rollback_record* >(this + 1); }
    const rollback_record* rollback_records() const { return r_cast< const rollback_record* >(this + 1); }
    static size_t size_for(uint32_t n) { return sizeof(LogStoreSb) + n * sizeof(rollback_record); }
};
#pragma pack()
static_assert(sizeof(LogStoreSb) == 20, "LogStoreSb header must be 20 bytes on disk");

// ─────────────────────────────────────────────────────────────────────────────
// In-memory: LogStoreRecord (StreamTracker entry, indexed by lsn)
//
// Carries two stream_keys.  dev_key — where the record's log_record_header lands; used by read().  trunc_key —
// the safe-truncate boundary associated with this lsn.  In the ascending-write case this is the record's own
// group_stream_offset; in the out-of-order-write case it is pinned to the highest-tail's trunc_key so a truncate
// to this lsn cannot drop a higher-lsn record's group.  Trivially copyable for StreamTracker.
// ─────────────────────────────────────────────────────────────────────────────
struct LogStoreRecord {
    logid_t log_id{0};
    uint64_t record_stream_offset{0};
    uint64_t trunc_stream_offset{0};

    LogStoreRecord(const stream_key& key, uint64_t trunc_stream_offset) :
            log_id{key.log_id},
            record_stream_offset{key.record_stream_offset},
            trunc_stream_offset{trunc_stream_offset} {}

    LogStoreRecord() = default;
    LogStoreRecord(logid_t lid, uint64_t record_off, uint64_t trunc_off) :
            log_id{lid}, record_stream_offset{record_off}, trunc_stream_offset{trunc_off} {}
};
static_assert(std::is_trivially_copyable_v< LogStoreRecord >, "StreamTracker requires trivially copyable T");

// ─────────────────────────────────────────────────────────────────────────────
// Replay handler invoked by LogStore::on_log_found for each recovered record once the store is opened.
// ─────────────────────────────────────────────────────────────────────────────
using log_replay_cb = std::function< void(lsn_t lsn, const sisl::IoBufView& data) >;

// ─────────────────────────────────────────────────────────────────────────────
// LogStore
//
// One per logical client of a shared LogStream.  Lifecycle (managed by LogStoreManager):
//   1. create(stream, sid, append_mode)        → fresh; sb persisted with head_lsn=0, no rollback ranges.
//   2. load(stream, sb_payload)                → parses sb (head_lsn + rollback ranges); records empty.
//   3. open(handler)                           → attaches replay handler.
//   4. mgr.recover()                           → triggers LogStream::recover; on_log_found per record.
//   5. Normal append/read/truncate/rollback usable.
//
// append_mode: when true, only quick_append / append_and_flush are allowed; quick_write / write_and_flush /
// fill_gap assert.  Append-mode auto-assigns lsns from next_lsn_ via fetch_add.
//
// Concurrency:
//   • Append path (quick_append, quick_write) — lock-free.
//   • on_write_completion fires from inside LogStream::flush which holds flush_mtx_; serialises naturally
//     against truncate/rollback (which take stream_->flush_lock() at entry).
//   • on_log_found fires from LogStream::recover, single-threaded at boot — no lock needed.
//   • truncate, rollback acquire stream_->flush_lock() and hold through the sb persist.  Append unaffected.
//   • Reads of records_ use StreamTracker's internal synchronisation; lsns are atomics.
// ─────────────────────────────────────────────────────────────────────────────
class LogStore : public LogStreamClient {
public:
    LogStore(const LogStore&) = delete;
    LogStore& operator=(const LogStore&) = delete;
    LogStore(LogStore&&) = delete;
    LogStore& operator=(LogStore&&) = delete;
    ~LogStore() override = default;

    // ── Factories (called by LogStoreManager) ────────────────────────────────

    static Async< shared< LogStore > > create(logstore_id_t sid, shared< MetaClient > meta_client,
                                              shared< LogStream > stream, bool append_mode);

    static Async< shared< LogStore > > load(shared< LogStream > stream, MetaBlkWrapper&& mb);

    // ── Open / state ─────────────────────────────────────────────────────────

    /// Attach a replay handler.  Must be called before LogStoreManager::recover() if the client wants per-record
    /// replay callbacks.  Transitions store to opened state.
    void open(log_replay_cb handler);

    bool is_open() const { return s_cast< bool >(handler_); }

    bool is_append_mode() const { return append_mode_; }

    // ── Append-mode API (also valid in non-append mode) ──────────────────────

    /// Auto-assign next lsn (next_lsn_.fetch_add(1)), reserve slot in records_ (active-bit only), enqueue into
    /// the underlying LogStream.  Synchronous; durability via on_write_completion.
    lsn_t quick_append(const LogBlob& data);

    /// quick_append + co_await flush_upto(lsn).
    Async< lsn_t > append_and_flush(const LogBlob& data);

    // ── Non-append-mode API (asserts !append_mode_) ──────────────────────────

    /// Caller-specified lsn write.  Reserve slot, enqueue.  next_lsn_ advances on completion via atomic-update-max.
    void quick_write(lsn_t lsn, const LogBlob& data);

    /// quick_write + co_await flush_upto(lsn).
    Async< void > write_and_flush(lsn_t lsn, const LogBlob& data);

    /// Insert an empty record at lsn (used to plug holes in non-append mode so truncate can advance over them).
    void fill_gap(lsn_t lsn);

    // ── Read / truncate / rollback / flush ───────────────────────────────────

    /// Read the data bytes for `lsn`.  Internally awaits flush_upto(lsn) so records_ is populated, then resolves
    /// the dev_key and reads via the underlying LogStream.  Returns an empty IoBufView if lsn is outside
    /// [head_lsn, next_lsn).
    Async< sisl::IoBufView > read(lsn_t lsn);

    /// Drains pending records (via stream_->flush()) until tail_lsn_ >= upto_lsn or bounded retry exhausted.
    Async< void > flush();

    /// Advance head_lsn to upto_lsn+1.  Evicts records [<= upto_lsn] from records_, captures the trunc_key for
    /// the manager's cross-store min aggregation, persists the new sb.  in_memory_only=true skips the manager
    /// notification (used when the manager itself is driving truncate).
    Async< void > truncate(lsn_t upto_lsn, bool in_memory_only = false);

    /// Drains in-flight via stream_->flush(), then under stream_->flush_lock(): captures the lsn-tail boundary
    /// (above_lsn=to_lsn) and the max log_id at the time of rollback, appends to rollback_records_, rolls
    /// records_ back, rewinds tail_lsn_, persists sb.  Returns false if to_lsn out of range.  Works for sparse
    /// non-append-mode stores — no requirement that to_lsn+1 be an active slot.
    Async< bool > rollback(lsn_t to_lsn);

    // ── Callbacks from LogStream ─────────────────────────────────────────────

    /// Computes trunc_key (ascending: this record's own key; out-of-order: pinned to current tail's trunc_key),
    /// updates records_[lsn] with both keys, advances tail_lsn_ and next_lsn_ (atomic-update-max).
    void on_write_completion(lsn_t lsn, const stream_key& key) override;

    /// Same trunc_key derivation as on_write_completion.  Inserts the record with both keys (recovery path
    /// doesn't reserve via create() at append time), advances tail_lsn_ and next_lsn_, fires the replay handler.
    /// Skips records below head_lsn_ or inside any persisted rollback range.
    void on_log_found(lsn_t lsn, const stream_key& key, const sisl::IoBufView& data) override;

    // ── Accessors ────────────────────────────────────────────────────────────

    logstore_id_t store_id() const override { return store_id_; }
    uint64_t stream_id() const { return stream_->stream_id(); }
    lsn_t head_lsn() const { return head_lsn_.load(std::memory_order_acquire); }
    lsn_t tail_lsn() const { return tail_lsn_.load(std::memory_order_acquire); }
    lsn_t flushed_upto() const;

    /// Returns the safest stream byte offset this store can be truncated to (== records_.at(head_lsn_).trunc_key
    /// .group_stream_offset).  std::nullopt when the store is empty (head_lsn > tail_lsn) — this store does not
    /// constrain the cross-store min aggregation in that case.
    std::optional< uint64_t > min_trunc_stream_offset() const;

    const shared< LogStream >& stream() const { return stream_; }

    /// Exposed so LogStoreManager::drop_unopened_stores can hand the underlying MetaBlk to
    /// MetaClient::remove_meta_blk.  Not for general use.
    const MetaBlk& sb_blk() const { return meta_blk_.meta_blk(); }

    LogStore(shared< LogStream > stream, MetaBlkWrapper&& mb, logstore_id_t sid, bool is_append_mode, lsn_t head_lsn,
             std::vector< rollback_record > rollback_records);

private:
    /// True if (lsn, log_id) was invalidated by any persisted rollback — i.e. lsn lies above some rollback's
    /// above_lsn AND log_id is ≤ that rollback's max_log_id (was written before the rollback).  Read by
    /// on_log_found during single-threaded recovery; written by rollback() under stream_->flush_lock().
    bool in_rollback_range(lsn_t lsn, logid_t log_id) const;

    /// Serialise current state into the sb mblk.  Caller holds stream_->flush_lock().
    Async< void > persist_sb();

    logstore_id_t store_id_{0};
    shared< LogStream > stream_;
    MetaBlkWrapper meta_blk_;
    bool append_mode_{false};

    std::atomic< lsn_t > head_lsn_{0};
    std::atomic< lsn_t > tail_lsn_{-1}; // -1 == empty
    mutable std::atomic< lsn_t > prev_contiguous_lsn_hint_{-1};

    log_replay_cb handler_{};

    sisl::StreamTracker< LogStoreRecord, /*AutoTruncate=*/false, /*TrackCompletion=*/false > records_;

    // Mutated only by rollback() under stream_->flush_lock(); read by on_log_found during single-threaded
    // recovery (which runs before any rollback can fire).  No additional synchronisation needed.
    std::vector< rollback_record > rollback_records_;
};

} // namespace homestore