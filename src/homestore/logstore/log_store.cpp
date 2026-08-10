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

#include "homestore/logstore/log_store.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <fmt/format.h>
#include "common/async.h"

#include "homestore/base/crash_simulator.h" // crash_if_flip_fired
#include "homestore/base/homestore_assert.h"
#include "homestore/base/hs_runtime_config.h" // HS_RUNTIME_CONFIG
#include "common/defs.h"
#include "homestore/meta/meta_client.h"

namespace homestore {

// Per-store log helper — emits to the `logstore` module with `sid=<store_id_>` tag so a single store's log lines
// can be filtered out of a busy log.
#define THIS_LOGSTORE_LOG(level, msg, ...) HS_SUBMOD_LOG(level, logstore, , "sid", store_id_, msg, ##__VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Construction / factories
// ─────────────────────────────────────────────────────────────────────────────

LogStore::LogStore(shared< LogStream > stream, MetaBlkWrapper&& mb, logstore_id_t sid, LogStoreOptions const& options,
                   lsn_t head_lsn, lsn_t checkpt_lsn, std::vector< rollback_record > rollback_records) :
        store_id_{sid},
        stream_{std::move(stream)},
        meta_blk_{std::move(mb)},
        options_{options},
        head_lsn_{head_lsn},
        tail_lsn_{head_lsn - 1},
        checkpt_lsn_{checkpt_lsn},
        pending_checkpt_lsn_{checkpt_lsn},
        records_{"LogStore", head_lsn - 1},
        rollback_records_{std::move(rollback_records)} {
}

Async< shared< LogStore > > LogStore::create(logstore_id_t sid, shared< MetaClient > meta_client,
                                             shared< LogStream > stream, LogStoreOptions const& options) {
    LOGINFO("Creating LogStore sid={} append_mode={} auto_truncate={} preserve_log_count={} on stream_id={}", sid,
            options.append_mode, options.auto_truncate, options.preserve_log_count, stream->stream_id());
    auto mb = co_await MetaBlkWrapper::create(std::move(meta_client), fmt::format("LogStore_{}", sid),
                                              std::optional< size_t >{sizeof(LogStoreSb)});
    LogStoreSb sb{};
    sb.store_id = sid;
    sb.append_mode = options.append_mode ? 1 : 0;
    sb.head_lsn = 0;
    sb.checkpt_lsn = -1;
    sb.n_rollback_records = 0;
    co_await mb.write(to_u8ptr(&sb), sizeof(sb));

    co_return std::make_shared< LogStore >(std::move(stream), std::move(mb), sid, options,
                                           /*head_lsn=*/0, /*checkpt_lsn=*/-1, std::vector< rollback_record >{});
}

Async< shared< LogStore > > LogStore::load(shared< LogStream > stream, MetaBlkWrapper&& mb) {
    sisl::IoBufView sb_payload = co_await mb.read();
    if (sb_payload.size() < sizeof(LogStoreSb)) {
        throw std::runtime_error(fmt::format("LogStore::load: sb payload too small ({} bytes)", sb_payload.size()));
    }
    const auto* sb = r_cast< const LogStoreSb* >(sb_payload.bytes());
    const logstore_id_t sid = sb->store_id;

    // Generating auto options, this will be actual user driven options once logstore is opened by the caller.
    LogStoreOptions options{};
    options.append_mode = (sb->append_mode != 0);

    const lsn_t head_lsn = sb->head_lsn;
    const lsn_t checkpt_lsn = sb->checkpt_lsn;
    std::vector< rollback_record > records;
    records.reserve(sb->n_rollback_records);
    for (uint32_t i = 0; i < sb->n_rollback_records; ++i) {
        records.push_back(sb->rollback_records()[i]);
    }
    LOGINFO("Loaded LogStore sid={} append_mode={} head_lsn={} checkpt_lsn={} rollback_records={}", sid,
            options.append_mode, head_lsn, checkpt_lsn, records.size());
    co_return std::make_shared< LogStore >(std::move(stream), std::move(mb), sid, options, head_lsn, checkpt_lsn,
                                           std::move(records));
}

void LogStore::open(LogStoreOptions const& options, log_replay_cb handler, log_commit_watermark_cb watermark_cb) {
    // append_mode is durable — was pulled from SB at load() time into options_.  Caller must match; a
    // mismatch means their reopen would silently mis-interpret every existing record.
    HS_REL_ASSERT_EQ(options_.append_mode, options.append_mode,
                     "LogStore::open sid={} append_mode mismatch: existing={} options={}", store_id_,
                     options_.append_mode, options.append_mode);
    options_ = options;
    handler_ = std::move(handler);
    watermark_cb_ = std::move(watermark_cb);
    THIS_LOGSTORE_LOG(INFO,
                      "Opened append_mode={} auto_truncate={} preserve_log_count={} head_lsn={} handler={} "
                      "watermark_cb={}",
                      options_.append_mode, options_.auto_truncate, options_.preserve_log_count,
                      head_lsn_.load(std::memory_order_relaxed), handler_ ? "set" : "none",
                      watermark_cb_ ? "set" : "none");
}

// ─────────────────────────────────────────────────────────────────────────────
// Append-mode API
// ─────────────────────────────────────────────────────────────────────────────

lsn_t LogStore::quick_append(const LogBlob& data) {
    const lsn_t lsn = tail_lsn_.fetch_add(1, std::memory_order_acq_rel) + 1;
    THIS_LOGSTORE_LOG(TRACE, "quick_append lsn={} size={}", lsn, data.size());
    stream_->append(this, lsn, data);
    return lsn;
}

Async< lsn_t > LogStore::append_and_flush(const LogBlob& data) {
    const lsn_t lsn = quick_append(data);

    // Wait for a brief time to allow coalescing multiple writes.
    co_await folly::coro::sleep(std::chrono::microseconds{HS_RUNTIME_CONFIG(logstore.flush_coalesce_wait_us)});
    do {
        if (records_.status(lsn).is_active) {
            co_return lsn;
        }

        // Flush and post flush check again the status to ensure it is completed.
        co_await stream_->flush();

        // Note: Post Flush check of status is needed because between the append_and_flush() call, there might be
        // another quick_append() call, which could have gotten a lsn lesser and the current flush might not have
        // flushed for sure. Hence we should check the status again, paying the additional atomic check
    } while (true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-append-mode API
// ─────────────────────────────────────────────────────────────────────────────

void LogStore::quick_write(lsn_t lsn, const LogBlob& data) {
    HS_REL_ASSERT(!options_.append_mode, "quick_write on append-mode LogStore (store_id={})", store_id_);
    THIS_LOGSTORE_LOG(TRACE, "quick_write lsn={} size={}", lsn, data.size());
    stream_->append(this, lsn, data);
}

Async< void > LogStore::write_and_flush(lsn_t lsn, const LogBlob& data) {
    quick_write(lsn, data);

    // Wait for a brief time to allow coalescing multiple writes.
    co_await folly::coro::sleep(std::chrono::microseconds{HS_RUNTIME_CONFIG(logstore.flush_coalesce_wait_us)});
    do {
        if (records_.status(lsn).is_active) {
            co_return;
        }

        // Flush and post flush check again the status to ensure it is completed.
        co_await stream_->flush();
    } while (true);
}

void LogStore::fill_gap(lsn_t lsn) {
    HS_REL_ASSERT(!options_.append_mode, "fill_gap on append-mode LogStore (store_id={})", store_id_);

    // Empty record with all-zero fields — non-append-mode caller signals "intentional hole at this lsn so
    // flushed_upto() can advance past it."  Bumps tail_lsn_ if this gap is past the current max.
    records_.create(lsn, LogStoreRecord{});
    lsn_t cur = tail_lsn_.load(std::memory_order_relaxed);
    while (cur < lsn && !tail_lsn_.compare_exchange_weak(cur, lsn, std::memory_order_release)) {}
    THIS_LOGSTORE_LOG(TRACE, "fill_gap lsn={}", lsn);
}

// ─────────────────────────────────────────────────────────────────────────────
// Read / flush / truncate / rollback
// ─────────────────────────────────────────────────────────────────────────────

Async< sisl::IoBufView > LogStore::read(lsn_t lsn) {
    auto exp = records_.try_at(lsn);
    if (!exp) {
        if (exp.error() == sisl::StreamTrackerError::OutOfRange) {
            co_return sisl::IoBufView{};
        }
        // NotActive: may be in-flight, flush and retry once.
        co_await stream_->flush();
        exp = records_.try_at(lsn);
        if (!exp) {
            co_return sisl::IoBufView{};
        }
    }
    auto rec = exp.value();
    co_return co_await stream_->read(stream_key{rec.log_id, rec.record_stream_offset, 0});
}

Async< void > LogStore::flush() {
    co_await stream_->flush();
}

Async< void > LogStore::truncate(lsn_t upto_lsn, bool in_memory_only) {
    auto lock = co_await stream_->flush_lock().co_scoped_lock();
    const lsn_t s = head_lsn_.load(std::memory_order_acquire);
    const lsn_t t = tail_lsn_.load(std::memory_order_acquire);

    // Clamp the caller's ask.  Two internal ceilings:
    //   - checkpt_lsn_: never truncate past what a completed CP durably captured.
    //   - tail_lsn - preserve_log_count: leave at least this many entries at the tail for peer catch-up.
    //     Signed subtraction — underflows below head_lsn when tail hasn't grown past preserve_log_count,
    //     naturally becoming a no-op via the head check below.
    lsn_t const checkpt = checkpt_lsn_.load(std::memory_order_acquire);
    lsn_t target = std::min({upto_lsn, checkpt, t - s_cast< lsn_t >(options_.preserve_log_count)});
    if (target < s) {
        THIS_LOGSTORE_LOG(DEBUG, "truncate(upto={} clamped={}) below head_lsn={}, no-op", upto_lsn, target, s);
        co_return;
    }
    if (target > t) {
        target = t;
    }
    records_.truncate(target);
    head_lsn_.store(target + 1, std::memory_order_release);
    THIS_LOGSTORE_LOG(INFO, "Truncated upto_lsn={} clamped={} new head_lsn={} in_memory_only={}", upto_lsn, target,
                      target + 1, in_memory_only);
    if (!in_memory_only) {
        co_await persist_sb();
    }
}

void LogStore::on_switchover_cp() {
    // Capture the pending checkpoint watermark: the consumer's durable watermark when a watermark_cb is
    // registered (its state derived from entries up to that lsn is registered in the CP being sealed), else
    // the current tail_lsn (the log content itself is the consumer state).  The next successful
    // cp_flush_persist promotes this to checkpt_lsn_ + sb->checkpt_lsn.
    lsn_t const tail = tail_lsn_.load(std::memory_order_acquire);
    lsn_t const watermark = watermark_cb_ ? watermark_cb_() : tail;
    HS_DBG_ASSERT_LE(watermark, tail, "LogStore sid={} watermark_cb returned {} above tail {}", store_id_, watermark,
                     tail);
    pending_checkpt_lsn_.store(std::min(watermark, tail), std::memory_order_release);
}

Async< void > LogStore::cp_flush_persist() {
    // Promote the pending watermark captured at switchover, then persist the sb.  No-op if nothing changed
    // since the last flush.
    lsn_t const pending = pending_checkpt_lsn_.load(std::memory_order_acquire);
    if (pending == checkpt_lsn_.load(std::memory_order_acquire)) {
        co_return;
    }
    checkpt_lsn_.store(pending, std::memory_order_release);
    auto lock = co_await stream_->flush_lock().co_scoped_lock();
    co_await persist_sb();
    THIS_LOGSTORE_LOG(TRACE, "cp_flush_persist: checkpt_lsn advanced to {}", pending);
}

Async< bool > LogStore::rollback(lsn_t to_lsn) {
    THIS_LOGSTORE_LOG(INFO, "rollback request to_lsn={} current tail_lsn={} head_lsn={}", to_lsn,
                      tail_lsn_.load(std::memory_order_relaxed), head_lsn_.load(std::memory_order_relaxed));
    // Drain in-flight via stream flush, then under flush_lock snapshot the max log_id.  Each rollback persists
    // (above_lsn=to_lsn, max_log_id=stream_->next_log_id()-1); on_log_found later suppresses records whose lsn
    // > above_lsn AND log_id ≤ max_log_id.  New writes after the rollback get fresh log_ids strictly greater
    // than max_log_id so they replay even if they land on the same lsn the rollback invalidated.  No
    // requirement that to_lsn+1 be an active slot — works for sparse non-append-mode stores.
    while (true) {
        co_await stream_->flush();
        auto lock = co_await stream_->flush_lock().co_scoped_lock();

        if (to_lsn < head_lsn_.load(std::memory_order_acquire)) {
            THIS_LOGSTORE_LOG(WARN, "rollback to_lsn={} below head_lsn — refused", to_lsn);
            co_return false;
        }
        const lsn_t cur_tail = tail_lsn_.load(std::memory_order_acquire);
        if (to_lsn >= cur_tail) {
            // Nothing to roll back (raced with another rollback or trivially in range).
            co_return true;
        }

        const logid_t max_log_id = stream_->next_log_id() - 1;
        rollback_records_.push_back({to_lsn, max_log_id});
        records_.rollback(to_lsn);
        tail_lsn_.store(to_lsn, std::memory_order_release);

        // Crash point: in-memory rollback and tail update are done, but the sb (which persists rollback_records_
        // and the new tail_lsn) has NOT been written.  Recovery must NOT observe the rollback — replay must
        // deliver the rolled-back records normally (they were never rejected on disk).
        if (crash_if_flip_fired("crash_before_logstore_rollback_commit")) {
            co_return false;
        }
        co_await persist_sb();
        THIS_LOGSTORE_LOG(INFO, "rollback complete to_lsn={} max_log_id={} new tail_lsn={}", to_lsn, max_log_id,
                          to_lsn);
        co_return true;
    }
}

lsn_t LogStore::flushed_upto() const {
    const lsn_t hint = prev_contiguous_lsn_hint_.load(std::memory_order_acquire);
    const lsn_t got = records_.active_upto(hint);
    prev_contiguous_lsn_hint_.store(got, std::memory_order_release);
    return got;
}

// ─────────────────────────────────────────────────────────────────────────────
// Callbacks from LogStream
// ─────────────────────────────────────────────────────────────────────────────

void LogStore::on_write_completion(lsn_t lsn, const stream_key& key) {
    // trunc_stream_offset for THIS lsn = the start of the group it landed in.  Out-of-order completion exception:
    // if this lsn is BELOW the current tail, the tail's record committed in a later group than this one, and we
    // must pin this lsn's trunc anchor to the tail's so a future truncate(this lsn) doesn't drop the tail's group.
    // append-mode bumps tail_lsn_ at quick_append time, so this branch only fires for non-append mode.
    uint64_t trunc_stream_offset{key.group_stream_offset};
    if (!options_.append_mode) {
        lsn_t cur_tail = tail_lsn_.load(std::memory_order_acquire);
        if (lsn > cur_tail) {
            // In-order completion: advance tail to this lsn.
            while (lsn > cur_tail && !tail_lsn_.compare_exchange_weak(cur_tail, lsn, std::memory_order_acq_rel)) {}
        } else {
            trunc_stream_offset = records_.at(cur_tail).trunc_stream_offset;
        }
    }
    records_.create(lsn, key.log_id, key.record_stream_offset, trunc_stream_offset);
    THIS_LOGSTORE_LOG(TRACE, "on_write_completion lsn={} log_id={} record_off={} trunc_off={}", lsn, key.log_id,
                      key.record_stream_offset, trunc_stream_offset);
}

Async< void > LogStore::on_log_found(lsn_t lsn, const stream_key& key, const sisl::IoBufView& data) {
    if (lsn < head_lsn_.load(std::memory_order_acquire)) {
        THIS_LOGSTORE_LOG(DEBUG, "on_log_found skip lsn={} below head_lsn", lsn);
        co_return;
    }
    if (in_rollback_range(lsn, key.log_id)) {
        THIS_LOGSTORE_LOG(DEBUG, "on_log_found skip lsn={} log_id={} in rollback range", lsn, key.log_id);
        co_return;
    }
    uint64_t trunc_stream_offset{key.group_stream_offset};
    const lsn_t cur_tail = tail_lsn_.load(std::memory_order_acquire);
    if (cur_tail < lsn) {
        tail_lsn_.store(lsn, std::memory_order_release);
    } else {
        trunc_stream_offset = records_.at(cur_tail).trunc_stream_offset;
    }
    records_.create(lsn, key.log_id, key.record_stream_offset, trunc_stream_offset);
    THIS_LOGSTORE_LOG(TRACE, "on_log_found lsn={} log_id={} record_off={} trunc_off={}", lsn, key.log_id,
                      key.record_stream_offset, trunc_stream_offset);
    // Replay floor for watermark-registered stores: entries at or below checkpt_lsn have their consumer-side
    // effects durably checkpointed, so handler delivery is skipped.  records_ was still rebuilt above — the
    // store's own index always covers the full retained log.
    if (watermark_cb_ && (lsn <= checkpt_lsn_.load(std::memory_order_acquire))) {
        THIS_LOGSTORE_LOG(DEBUG, "on_log_found lsn={} at/below checkpt_lsn={} — handler delivery skipped", lsn,
                          checkpt_lsn_.load(std::memory_order_acquire));
        co_return;
    }
    if (handler_) {
        co_await handler_(lsn, data);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors / helpers
// ─────────────────────────────────────────────────────────────────────────────

std::optional< uint64_t > LogStore::min_trunc_stream_offset() const {
    auto exp = records_.try_at(head_lsn_.load(std::memory_order_acquire));
    if (!exp) {
        return std::nullopt;
    }
    return exp.value().trunc_stream_offset;
}

bool LogStore::in_rollback_range(lsn_t lsn, logid_t log_id) const {
    for (auto const& r : rollback_records_) {
        if (lsn > r.above_lsn && log_id <= r.max_log_id) {
            return true;
        }
    }
    return false;
}

Async< void > LogStore::persist_sb() {
    const uint32_t n = to_u32(rollback_records_.size());
    const size_t sz = LogStoreSb::size_for(n);

    auto buf = sisl::make_io_buf_shared(to_u32(sz));
    auto* sb = r_cast< LogStoreSb* >(buf->bytes());
    sb->store_id = store_id_;
    sb->append_mode = options_.append_mode ? 1 : 0;
    sb->head_lsn = head_lsn_.load(std::memory_order_acquire);
    sb->checkpt_lsn = checkpt_lsn_.load(std::memory_order_acquire);
    sb->n_rollback_records = n;
    std::copy(rollback_records_.begin(), rollback_records_.end(), sb->rollback_records());
    co_await meta_blk_.write(buf->cbytes(), sz);
}

} // namespace homestore