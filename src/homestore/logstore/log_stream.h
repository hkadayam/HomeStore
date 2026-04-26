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
#include <type_traits>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>
#include <sisl/fds/buffer.h>
#include <sisl/fds/stream_tracker.h>

#include <homestore/crc.h>

#include "homestore/base/homestore_decl.h"
#include "blob/append_byte_stream.h"

namespace homestore {

class LogStore;
class MetaClient;
class VirtualDev;

// ─────────────────────────────────────────────────────────────────────────────
// Type aliases
// ─────────────────────────────────────────────────────────────────────────────
using logid_t = int64_t;
using logstore_id_t = uint32_t;
using lsn_t = int64_t;

/// Resolves a store_id to its owning LogStore during recovery.  Provided by LogStoreManager (which knows the
/// opened-store map) and passed to LogStream::recover.  Returning nullptr signals an orphan record (store was never
/// opened) — LogStream skips dispatch for it; the manager is responsible for orphan cleanup.
using lookup_store_fn = std::function< LogStore*(logstore_id_t) >;

// ─────────────────────────────────────────────────────────────────────────────
// stream_key
//
// Identifies one record within a LogStream and carries the two byte offsets a LogStore needs to drive read /
// truncate against it:
//   • record_stream_offset — byte offset of the record's log_record_header (used by read()).
//   • group_stream_offset  — byte offset of the start of the LogGroup containing the record (used by truncate(): we
//                             must truncate at the group head to avoid dropping later records flushed in the same
//                             group that aren't yet safe).
// LogStore caches stream_key in its per-record map; values are produced by LogStream via on_write_completion (for
// freshly-flushed records) and on_log_found (for recovered records).
// ─────────────────────────────────────────────────────────────────────────────
struct stream_key {
    logid_t log_id{-1};
    uint64_t record_stream_offset{0};
    uint64_t group_stream_offset{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// On-disk LogGroup framing
//
//   ┌──────────────────────────────────────────────────────────────────────┐
//   │ log_group_header  { magic, n_records, group_size }            12 B   │
//   ├──────────────────────────────────────────────────────────────────────┤
//   │ log_record_header[0]  { log_id, store_id, store_lsn, size }   24 B   │
//   │ data[0]                                                       size B │
//   │ log_record_header[1]                                          24 B   │
//   │ data[1]                                                       ...    │
//   │ ...                                                                  │
//   ├──────────────────────────────────────────────────────────────────────┤
//   │ log_group_footer  { prev_crc, cur_crc }                       8 B    │
//   └──────────────────────────────────────────────────────────────────────┘
//
// Overhead per group: 20 + 24*N bytes (header 12 + footer 8 + 24 per record header).
//
// Data follows its own record header directly; recovery walks groups sequentially using `size` in each record
// header, no offset table.  Padding to block boundary is handled transparently by the parent AppendByteStream.
//
// CRC chain: footer.cur_crc covers everything from log_group_header through the last data byte (i.e. the
// group_size minus the footer size).  footer.prev_crc must match the previous group's cur_crc; this catches
// out-of-order or truncated writes during recovery.
// ─────────────────────────────────────────────────────────────────────────────
constexpr uint32_t LOG_GROUP_MAGIC = 0xF00DCAFE;

#pragma pack(1)
struct log_group_header {
    uint32_t magic;
    uint32_t n_records;
    uint32_t group_size; // total bytes from header through footer (inclusive)
};

struct log_record_header {
    logid_t log_id;
    logstore_id_t store_id;
    lsn_t store_lsn;
    uint32_t size;
};

struct log_group_footer {
    crc32_t prev_crc;
    crc32_t cur_crc;
};
#pragma pack()

static_assert(sizeof(log_group_header) == 12, "log_group_header must be 12 bytes on disk");
static_assert(sizeof(log_record_header) == 24, "log_record_header must be 24 bytes on disk");
static_assert(sizeof(log_group_footer) == 8, "log_group_footer must be 8 bytes on disk");

// ─────────────────────────────────────────────────────────────────────────────
// In-memory: LogRecord (StreamTracker entry)
//
// One per appended record awaiting flush.  Holds a shallow IoBlob view of the caller's data buffer — the caller is
// responsible for keeping the buffer alive until flush() returns.  Carries the owning LogStore* so on flush
// completion we can dispatch on_write_completion directly without a store_id → store map lookup; store_id itself is
// fetched via store->store_id() when filling the on-disk record header
// Trivially copyable for StreamTracker.
// ─────────────────────────────────────────────────────────────────────────────
struct LogRecord {
    sisl::IoBlob data{};
    LogStore* store{nullptr};
    lsn_t lsn{0};
};
static_assert(std::is_trivially_copyable_v< LogRecord >, "StreamTracker requires trivially copyable T");

// ─────────────────────────────────────────────────────────────────────────────
// LogStream : AppendByteStream
//
// Append-only log of variable-size records, framed as LogGroups on disk.  Many concurrent appenders feed a
// StreamTracker<LogRecord, false, false> (no completion bits).
// flush() drains the tracker into one or more LogGroups, emplaces them into the parent's flush buffer
// (zero-extra-memcpy via emplace), then calls AppendByteStream::flush() once to push to disk.
//
// Recovery: the per-stream sb persists head_offset, chunk_size, and chunk_ids (same as AppendByteStream).
// tail_offset on disk is best-effort — the authoritative tail is rediscovered by walking the CRC chain forward from
// head_offset until the chain breaks (magic mismatch / CRC mismatch / zero block).  This lets LogStream skip a sb
// write on every flush; only chunk-list changes and explicit truncate calls persist the sb.
// ─────────────────────────────────────────────────────────────────────────────
class LogStream : public AppendByteStream {
public:
    static folly::coro::Task< shared< LogStream > > create(uint64_t stream_id, MetaClient& meta_client,
                                                           const std::string& dev_name,
                                                           const shared< VirtualDev >& vdev, uint64_t chunk_size);

    /// Load from a persisted LogStream sb MetaBlk + payload.  Reconstructs head_offset, chunk_size, and the chunk
    /// list — but does NOT walk the CRC chain.  Manager calls recover() separately, after every LogStore has been
    /// opened, so on_log_found dispatch finds its target.
    static folly::coro::Task< shared< LogStream > > load(uint64_t stream_id, MetaClient& meta_client,
                                                         const std::string& dev_name, const shared< VirtualDev >& vdev,
                                                         MetaBlk&& sb, sisl::ByteView sb_payload);

    /// Walk the on-disk LogGroup chain forward from head_offset, validating magic + CRC chain at each step.  For
    /// every recovered record, looks up its owning store via `lookup` and (if non-null) dispatches
    /// store->on_log_found(lsn, log_id, stream_offset, data_view).  Sets tail_offset_, last_crc_, log_id_, and
    /// last_flush_idx_ to the post-recovery state.  Stops at the first invalid group; tail_offset_ lands at the
    /// boundary between durable and missing data.  Called by LogStoreManager::recover().
    folly::coro::Task< void > recover(lookup_store_fn lookup);

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;
    LogStream(LogStream&&) = delete;
    LogStream& operator=(LogStream&&) = delete;
    ~LogStream() override = default;

    /// Append one record on behalf of a LogStore.  Synchronous, no I/O — record is stashed in the tracker for the
    /// next flush().  Returns the assigned monotonic log_id.  The store pointer is cached in the LogRecord and used
    /// during flush completion to fire store->on_write_completion directly (no demux).  Caller's data buffer must
    /// outlive the next flush().
    logid_t append(LogStore* store, lsn_t lsn, const sisl::IoBlob& data);

    /// Drain the tracker into 1+ LogGroups (loop while more contiguous-active records appear, capped at
    /// max_flush_loops), emplace each group into the parent buffer with CRC chaining, then call
    /// AppendByteStream::flush() once to push to disk.  Serialised by an internal coro mutex.
    folly::coro::Task< void > flush();

    /// Read the data bytes of the record identified by `key`.  Reads the log_record_header at
    /// key.record_stream_offset, validates `log_id` matches, then returns the data slice.  Returns an empty buffer
    /// on stale key or read error.
    folly::coro::Task< sisl::ByteView > read(const stream_key& key);

    /// Advance the stream's head to key.group_stream_offset (must be a LogGroup boundary — only stream_keys handed
    /// out by on_write_completion / on_log_found satisfy this) and persist the sb.  Releases any chunks fully before
    /// the new head.  Inherited semantics: head==tail keeps one chunk as anchor and resets positions to 0.
    folly::coro::Task< void > truncate(const stream_key& key) {
        return AppendByteStream::truncate(key.group_stream_offset);
    }

    /// MetaBlk-name component used by StreamBase::init_chunk_mblk and BlobDevManager parsing.
    std::string_view stream_type_name() const override { return "logstream"; }

    /// Per-stream sb MetaBlk name: "<dev>_logstream_sb_<stream_id>".
    static std::string sb_mblk_name(const std::string& dev, uint64_t stream_id);

    // ── Accessors (mostly for tests/diagnostics) ─────────────────────────────
    logid_t next_log_id() const { return log_id_.load(std::memory_order_acquire); }

    /// Exposed for LogStore-level coordination: LogStore::truncate / rollback acquire this so they serialize
    /// against flush() (and therefore against on_write_completion which fires from inside flush).  Append path
    /// does not touch this lock, so it remains lock-free.
    folly::coro::Mutex& flush_lock() { return flush_mtx_; }

protected:
    /// LogStream rediscovers tail via CRC walk on recovery, so per-flush sb persistence is unnecessary.
    /// Chunk-list changes still persist via the inherited init/remove_chunk_mblk overrides; explicit truncate
    /// persists the sb directly.
    folly::coro::Task< void > persist_flush_metadata() override;

private:
    LogStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name, const shared< VirtualDev >& vdev,
              uint64_t chunk_size);

    /// Build a single LogGroup covering log_ids [from_idx, upto_idx] (inclusive) and emplace it into the parent's
    /// flush buffer.  Updates last_crc_.  Returns the stream byte offset where the log_group_header lands.
    uint64_t build_and_emplace_group(logid_t from_idx, logid_t upto_idx);

    static constexpr int max_flush_loops = 4;

    std::unique_ptr< sisl::StreamTracker< LogRecord, /*AutoTruncate=*/false, /*TrackCompletion=*/false > > log_records_;
    std::atomic< logid_t > log_id_{0};
    std::atomic< int64_t > pending_flush_size_{0};

    folly::coro::Mutex flush_mtx_;

    logid_t last_flush_idx_{-1};
    crc32_t last_crc_{hs_init_crc_32};
};

} // namespace homestore