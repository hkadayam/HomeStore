# Plan: LogStream + HomeLogStore Redesign

## Context

LogDev + JournalVirtualDev are being replaced by LogStream, a BlobDev stream type derived from
StreamBase. LogStream absorbs both LogDev's log management and JournalVirtualDev's IO role.
HomeLogStore moves from callbacks to folly coroutines. Both use StreamTracker with
CompletionTracking=false.

## Part 1: LogStream (replaces LogDev + JournalVirtualDev)

### On-Disk Format — Model C (Hybrid)

Each flush writes a group of N records as a contiguous byte sequence into the stream:

```
┌─────────────────────────────────────────────────────────────────┐
│ LogGroupHeader: { magic(4B), n_records(4B), group_size(4B) }    │  12B
├─────────────────────────────────────────────────────────────────┤
│ LogRecordHeader[0]: { log_id(8B), store_id(4B),                 │
│                       store_lsn(8B), size(4B) }                 │  24B
│ data[0]: [ size bytes ]                                         │
├─────────────────────────────────────────────────────────────────┤
│ LogRecordHeader[1]: { ... }                                     │  24B
│ data[1]: [ size bytes ]                                         │
├─────────────────────────────────────────────────────────────────┤
│ ...  (N records total)                                          │
├─────────────────────────────────────────────────────────────────┤
│ LogGroupFooter: { prev_crc(4B), cur_crc(4B) }                  │  8B
└─────────────────────────────────────────────────────────────────┘
```

**Overhead per flush**: 20 + 24N bytes (header 12B + N * record_header 24B + footer 8B).

**No inline/OOB distinction**: each record's data follows its header directly. No offset
arithmetic, no separate inline/OOB areas.

**No per-group block padding**: the stream is continuous. BlobDev retains the residual partial
page from the previous flush. The next flush coalesces onto it. All writes to disk are
NVMe-page-aligned (4KB) — stream layer handles this transparently. Padding waste is only at the
stream tail, not per flush.

**CRC chain**: footer carries prev_crc + cur_crc. cur_crc covers everything from LogGroupHeader
through the last data byte (before footer). Validated group-to-group during recovery. Zeroed
chunks + CRC validation detects end of valid stream.

**Recovery scan**: read from starting_offset. For each position: validate LogGroupHeader magic,
read group_size bytes, validate CRC chain in footer. Parse N records within the group. When magic
fails or CRC doesn't match → end of valid stream.

### stream_key (replaces logdev_key)

```cpp
struct stream_key {
    logid_t log_id;       // Monotonic log ID across all logstores in this stream
    off_t stream_offset;  // Byte offset within the stream where this record's header starts
};
```

Logstore holds stream_keys for reads and truncation bookkeeping. log_id provides validation
and lookup beyond just the offset.

**Read path**: given stream_key, round down stream_offset to page boundary, read enclosing
page(s), extract LogRecordHeader + data starting at (stream_offset - aligned_offset). Same
pattern LogDev already uses today (log_dev.cpp:317-322). If record straddles a page boundary,
read two pages — identical to current handling.

### LogStream Class

Derived from StreamBase. Manages chunks, handles IO.

```cpp
class LogStream : public StreamBase {
public:
    // Append a record — fast synchronous call. Stores in StreamTracker, returns log_id.
    // Does NOT write to disk. Data is written on flush.
    logid_t append(logstore_id_t store_id, logstore_seq_num_t seq_num, const sisl::IoBlob& data);

    // Flush all pending records to disk. Internal coro mutex, never exposed.
    folly::coro::Task<void> flush();

    // Read a record by stream_key
    log_buffer read(const stream_key& key);

    // Truncate the stream up to a given log_id
    folly::coro::Task<uint64_t> truncate();

    // Rollback log_id range for a store (persisted in logstore_superblk, not stream-level)
    void rollback(logstore_id_t store_id, logid_range_t id_range);

    // Logstore management
    shared<HomeLogStore> create_new_log_store(bool append_mode = false);
    folly::Future<shared<HomeLogStore>> open_log_store(logstore_id_t store_id, bool append_mode,
                                                        log_found_cb_t cb = nullptr,
                                                        log_replay_done_cb_t replay_cb = nullptr);
    void remove_log_store(logstore_id_t store_id);

    // Start/stop/recovery
    void start(bool format);
    folly::coro::Task<void> stop();

private:
    // Flush internals — all under coro mutex
    folly::coro::Task<bool> flush_impl();
    LogGroup* prepare_flush(int32_t estimated_records);
    void on_flush_completion(LogGroup* lg);

    // Recovery
    void do_load(off_t offset);
    void on_log_store_found(logstore_id_t store_id, const logstore_superblk& sb);
    void on_logfound(logstore_id_t id, logstore_seq_num_t seq_num, stream_key key,
                     stream_key flush_key, log_buffer buf, uint32_t nremaining);
    void handle_unopened_log_stores(bool format);

    // StreamTracker with CompletionTracking=false — only tracks active records for flush
    std::unique_ptr<sisl::StreamTracker<log_record, false, false>> m_log_records;
    std::atomic<logid_t> m_log_idx{0};
    std::atomic<int64_t> m_pending_flush_size{0};

    // Coro mutex replaces flush_mtx — never exposed
    folly::coro::Mutex m_flush_mtx;

    // Store management (same as today's LogDev)
    folly::SharedMutexWritePriority m_store_map_mtx;
    std::unordered_map<logstore_id_t, logstore_info> m_id_logstore_map;
    std::unordered_map<logstore_id_t, uint64_t> m_unopened_store_io;
    std::unordered_set<logstore_id_t> m_unopened_store_id;
    std::multimap<logid_t, logstore_id_t> m_garbage_store_ids;

    // Flush state
    logid_t m_last_flush_idx{-1};
    stream_key m_last_flush_key{0, 0};
    logid_t m_last_truncate_idx{-1};
    crc32_t m_last_crc{INVALID_CRC32_VALUE};

    // Metadata
    LogStreamMetadata m_metadata;  // logstream_superblk (starting_offset only)
};
```

### append() — hot path (synchronous)

```cpp
logid_t LogStream::append(logstore_id_t store_id, logstore_seq_num_t seq_num,
                           const sisl::IoBlob& data) {
    auto const idx = m_log_idx.fetch_add(1, std::memory_order_acq_rel);
    m_pending_flush_size.fetch_add(data.size(), std::memory_order_relaxed);
    m_log_records->create(idx, store_id, seq_num, data);
    return idx;
}
```

No cb_context parameter — logstore_req is eliminated.

### flush() — coro, internal mutex

```cpp
folly::coro::Task<void> LogStream::flush() {
    auto lock = co_await m_flush_mtx.co_scoped_lock();
    co_await flush_impl();
}
```

No `flush_under_guard()` exposed. No `flush_guard()` exposed. Callers just `co_await flush()`.

### on_flush_completion — calls logstore directly

```cpp
void LogStream::on_flush_completion(LogGroup* lg) {
    auto from_idx = lg->m_flush_log_idx_from;
    auto upto_idx = lg->m_flush_log_idx_upto;
    auto dev_offset = lg->m_log_dev_offset;

    for (auto idx = from_idx; idx <= upto_idx; ++idx) {
        auto& record = m_log_records->at(idx);
        folly::SharedMutexWritePriority::ReadHolder holder(m_store_map_mtx);
        auto it = m_id_logstore_map.find(record.store_id);
        if (it != m_id_logstore_map.end()) {
            it->second.log_store->on_write_completion(
                record.seq_num,
                stream_key{idx, /* offset computed from group layout */},
                stream_key{from_idx, dev_offset});
        }
    }

    m_log_records->truncate(upto_idx);
    m_last_flush_idx = upto_idx;
    m_last_flush_key = stream_key{from_idx, dev_offset};
}
```

No `run_on_forget` callback dispatch. No logstore_req. No completion bits on StreamTracker
(CompletionTracking=false, so `complete()` is a no-op and removed).

### Superblock Changes

**logstream_superblk** (replaces logdev_superblk):
```cpp
struct logstream_superblk {
    uint32_t magic;
    uint32_t version;
    uint64_t starting_offset;  // Where to start recovery scan
};
```

No per-store metadata in the stream superblk. Each logstore persists its own state.

**logdev_superblk**: eliminated. Fields absorbed:
- `start_dev_offset` / `key_idx` → `logstream_superblk.starting_offset`
- `num_stores` + `logstore_superblk[]` → each logstore has its own MetaBlk superblk
- `flush_mode` → LogStream constructor parameter

**rollback_superblk**: moves into logstore_superblk. Each logstore persists its own
rolled-back ranges as `stream_key` pairs (log_id + stream_offset), not LSNs. log_id is
monotonic so rollback → roll-forward → rollback sequences are unambiguous.

```cpp
struct logstore_superblk {
    logstore_seq_num_t m_first_seq_num{0};
    uint32_t n_rollback_records{0};
    // followed by: rollback_record[n_rollback_records]
    // each rollback_record: { logid_range_t idx_range } using stream-level log_ids
};
```

### Eliminated

- JournalVirtualDev — replaced by BlobDev/StreamBase
- LogGroup's inline/OOB area management — data follows record header directly
- log_group_footer's magic/version/start_log_idx/padding — slimmed to just {prev_crc, cur_crc}
- log_group_header's inline_data_offset, oob_data_offset, footer_offset, prev_grp_crc,
  cur_grp_crc, logdev_id, version — slimmed to {magic, n_records, group_size}
- serialized_log_record's offset:31 + is_inlined:1 — not needed, data is contiguous
- flush_under_guard() / flush_guard() — internal coro mutex only
- LogDevMetadata class — split into logstream_superblk + per-store superblks
- logdev_req (already #if 0'd)
- log_stream_reader — replaced by simpler sequential scanner in LogStream::do_load()

### Kept

- LogGroup class (simplified) — still accumulates records for batch write
- StreamTracker<log_record, false, false> — tracks pending records between append and flush
- Store registration/unregistration — same contract
- Unopened store handling — same contract (skip + garbage collect)
- flush_if_necessary threshold logic — size + time based triggers
- IDReserver for store IDs

### New Chunks Must Be Zeroed

Required for CRC-based end-of-stream detection during recovery. When the stream allocates a new
chunk, it must be zeroed so that the recovery scanner sees non-magic bytes and stops.

---

## Part 2: HomeLogStore Redesign

### New Public API

```cpp
// Fire-and-forget: append with auto-assigned LSN, returns immediately
logstore_seq_num_t quick_append(const sisl::IoBlob& data);

// Fire-and-forget: write at user-specified LSN, returns immediately
void quick_write(logstore_seq_num_t lsn, const sisl::IoBlob& data);

// Coroutine: append + await flush completion
folly::coro::Task<logstore_seq_num_t> append_and_flush(const sisl::IoBlob& data);

// Coroutine: write at LSN + await contiguous completion through that LSN
folly::coro::Task<void> write_and_flush(logstore_seq_num_t lsn, const sisl::IoBlob& data);

// Coroutine: await completion through target LSN, trigger flush if needed
folly::coro::Task<void> flush_upto(logstore_seq_num_t upto_lsn);

// Coroutine: flush everything appended so far
folly::coro::Task<void> flush();
```

### StreamTracker with CompletionTracking=false

LogStore uses `StreamTracker<logstore_record, false, false>`. No completion bits. Completion
is tracked via `m_tail_lsn` (atomic), updated in `on_write_completion`.

- `quick_append` / `quick_write`: call `m_records.create(lsn)` → sets active bit only
- `on_write_completion`: call `m_records.update(seq_num, processor returning false)` to store
  stream_key + trunc_key in the record. Update `m_tail_lsn` via CAS.
- `flush_upto`: checks `m_tail_lsn >= target_lsn` (for append_only, LSNs are monotonic so
  tail_lsn is the completion marker). Sleep + poll + explicit flush if needed.
- `active_upto()`: still available for contiguous-issued tracking
- `completed_upto()`: NOT used — static_assert prevents it with CompletionTracking=false

### on_write_completion (called by LogStream flush thread)

```cpp
void HomeLogStore::on_write_completion(logstore_seq_num_t seq_num,
                                        const stream_key& key,
                                        const stream_key& flush_key) {
    stream_key trunc_key;
    auto current_tail = m_tail_lsn.load(std::memory_order_acquire);
    if (current_tail < seq_num) {
        while (!m_tail_lsn.compare_exchange_weak(current_tail, seq_num,
                std::memory_order_acq_rel) && current_tail < seq_num) {}
        trunc_key = flush_key;
    } else {
        trunc_key = m_records.at(current_tail).m_trunc_key;
    }

    m_records.update(seq_num, [&key, &trunc_key](logstore_record& rec) -> bool {
        rec.m_dev_key = key;
        rec.m_trunc_key = trunc_key;
        return false;  // CompletionTracking=false, processor return value is irrelevant
    });
}
```

### flush_upto (sleep + poll)

```cpp
folly::coro::Task<void> HomeLogStore::flush_upto(logstore_seq_num_t upto_lsn) {
    // Fast path: already completed
    if (m_tail_lsn.load(std::memory_order_acquire) >= upto_lsn) co_return;

    // Sleep for coalescing window
    co_await folly::coro::sleep(m_flush_coalesce_wait);

    // Poll: check if stream's periodic/inline flush already completed our LSN
    if (m_tail_lsn.load(std::memory_order_acquire) >= upto_lsn) co_return;

    // Not yet flushed — trigger flush ourselves
    co_await m_logstream->flush();

    // Bounded retry if still not complete (out-of-order LSN gaps)
    for (int i = 0; i < max_flush_retries
         && m_tail_lsn.load(std::memory_order_acquire) < upto_lsn; ++i) {
        co_await folly::coro::sleep(m_flush_retry_wait);
    }
}
```

### Eliminated from HomeLogStore

- `logstore_req` class — heap-allocated per-append request
- `log_req_comp_cb_t` / `log_write_comp_cb_t` — callback typedefs
- `register_req_comp_cb()` / `m_comp_cb` / `get_comp_cb()`
- `write_async()` — both overloads
- `append_async(IoBlob, cookie, cb)` — callback-based
- Old blocking `write_and_flush(seq_num, IoBlob)`
- `get_contiguous_completed_seq_num()` — no completion bits

### Kept in HomeLogStore

- `read_sync()` — reads via stream_key from LogStream
- `truncate()` — in-memory truncate of StreamTracker + logdev truncation
- `fill_gap()` — creates active record with empty key (no create_and_complete since
  CompletionTracking=false; uses create() instead)
- `rollback()` — flush + rollback StreamTracker + persist rollback ranges in logstore_superblk
- `foreach()` — iterates active records (not completed, since no completion bits)
- `get_contiguous_issued_seq_num()` — active_upto wrapper
- Recovery callbacks: `log_found_cb_t`, `log_replay_done_cb_t`

### logstore_record Changes

```cpp
struct logstore_record {
    stream_key m_dev_key;    // was logdev_key
    stream_key m_trunc_key;  // was logdev_key
};
```

### Member Changes

```cpp
// Changed
std::shared_ptr<LogStream> m_logstream;  // was std::shared_ptr<LogDev> m_logdev
sisl::StreamTracker<logstore_record, false, false> m_records;  // was <logstore_record>
stream_key m_trunc_key{0, 0};  // was logdev_key

// Added
std::chrono::microseconds m_flush_coalesce_wait{50};
std::chrono::microseconds m_flush_retry_wait{10};
static constexpr int max_flush_retries = 100;

// Removed
log_req_comp_cb_t m_comp_cb;
```

---

## Implementation Order

1. Implement StreamTracker CompletionTracking=false for LogDev (already done in stream_tracker.h)
2. Define on-disk structures: LogGroupHeader (slim), LogRecordHeader, LogGroupFooter
3. Implement LogStream class on BlobDev/StreamBase
4. Implement HomeLogStore coroutine API against LogStream
5. Remove JournalVirtualDev, old LogDev, logstore_req, callback typedefs

## Verification

1. Build LogStream + HomeLogStore
2. Run logstore tests — append, flush, read, truncate, rollback
3. Recovery test — write records, restart, verify all records recovered via CRC chain walk
4. Crash recovery test — partial flush, verify records up to last valid group recovered
5. Grep for logdev_key, logstore_req, flush_under_guard, JournalVirtualDev to confirm removal
