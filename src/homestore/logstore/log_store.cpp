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
#include <folly/coro/Sleep.h>

#include "homestore/base/homestore_assert.h"
#include "homestore/base/homestore_config.h" // HS_DYNAMIC_CONFIG
#include "common/defs.h"
#include "homestore/meta/meta_client.h"

namespace homestore {

// Per-store log helper — emits to the `logstore` module with `sid=<store_id_>` tag so a single store's log lines
// can be filtered out of a busy log.
#define THIS_LOGSTORE_LOG(level, msg, ...) HS_SUBMOD_LOG(level, logstore, , "sid", store_id_, msg, ##__VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Construction / factories
// ─────────────────────────────────────────────────────────────────────────────

LogStore::LogStore(shared< LogStream > stream, MetaBlkWrapper&& mb, logstore_id_t sid, bool is_append_mode,
                   lsn_t head_lsn, std::vector< logid_range > rollback_ranges) :
        store_id_{sid},
        stream_{std::move(stream)},
        meta_blk_{std::move(mb)},
        append_mode_{is_append_mode},
        head_lsn_{head_lsn},
        tail_lsn_{head_lsn - 1},
        records_{"LogStore", head_lsn - 1},
        rollback_ranges_{std::move(rollback_ranges)} {
}

folly::coro::Task< shared< LogStore > > LogStore::create(logstore_id_t sid, shared< MetaClient > meta_client,
                                                         shared< LogStream > stream, bool is_append_mode) {
    LOGINFO("Creating LogStore sid={} append_mode={} on stream_id={}", sid, is_append_mode, stream->stream_id());
    auto mb = co_await MetaBlkWrapper::create(std::move(meta_client), fmt::format("LogStore_{}", sid),
                                              std::optional< size_t >{sizeof(LogStoreSb)});
    LogStoreSb sb{};
    sb.store_id = sid;
    sb.append_mode = is_append_mode ? 1 : 0;
    sb.head_lsn = 0;
    sb.n_rollback_ranges = 0;
    co_await mb.write(to_u8ptr(&sb), sizeof(sb));

    co_return std::make_shared< LogStore >(std::move(stream), std::move(mb), sid, is_append_mode,
                                           /*head_lsn=*/0, std::vector< logid_range >{});
}

folly::coro::Task< shared< LogStore > > LogStore::load(shared< LogStream > stream, MetaBlkWrapper&& mb) {
    sisl::ByteView sb_payload = co_await mb.read();
    if (sb_payload.size() < sizeof(LogStoreSb)) {
        throw std::runtime_error(fmt::format("LogStore::load: sb payload too small ({} bytes)", sb_payload.size()));
    }
    const auto* sb = r_cast< const LogStoreSb* >(sb_payload.bytes());
    const logstore_id_t sid = sb->store_id;
    const bool is_append_mode = (sb->append_mode != 0);
    const lsn_t head_lsn = sb->head_lsn;
    std::vector< logid_range > ranges;
    ranges.reserve(sb->n_rollback_ranges);
    for (uint32_t i = 0; i < sb->n_rollback_ranges; ++i) {
        ranges.push_back(sb->rollback_ranges()[i]);
    }
    LOGINFO("Loaded LogStore sid={} append_mode={} head_lsn={} rollback_ranges={}", sid, is_append_mode, head_lsn,
            ranges.size());
    co_return std::make_shared< LogStore >(std::move(stream), std::move(mb), sid, is_append_mode, head_lsn,
                                           std::move(ranges));
}

void LogStore::open(log_replay_cb handler) {
    handler_ = std::move(handler);
    THIS_LOGSTORE_LOG(INFO, "Opened append_mode={} head_lsn={} replay_handler={}", append_mode_,
                      head_lsn_.load(std::memory_order_relaxed), handler_ ? "set" : "none");
}

// ─────────────────────────────────────────────────────────────────────────────
// Append-mode API
// ─────────────────────────────────────────────────────────────────────────────

lsn_t LogStore::quick_append(const sisl::IoBlob& data) {
    const lsn_t lsn = tail_lsn_.fetch_add(1, std::memory_order_acq_rel) + 1;
    THIS_LOGSTORE_LOG(TRACE, "quick_append lsn={} size={}", lsn, data.size());
    stream_->append(this, lsn, data);
    return lsn;
}

folly::coro::Task< lsn_t > LogStore::append_and_flush(const sisl::IoBlob& data) {
    const lsn_t lsn = quick_append(data);

    // Wait for a brief time to allow coalescing multiple writes.
    co_await folly::coro::sleep(std::chrono::microseconds{HS_DYNAMIC_CONFIG(logstore.flush_coalesce_wait_us)});
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

void LogStore::quick_write(lsn_t lsn, const sisl::IoBlob& data) {
    HS_REL_ASSERT(!append_mode_, "quick_write on append-mode LogStore (store_id={})", store_id_);
    THIS_LOGSTORE_LOG(TRACE, "quick_write lsn={} size={}", lsn, data.size());
    stream_->append(this, lsn, data);
}

folly::coro::Task< void > LogStore::write_and_flush(lsn_t lsn, const sisl::IoBlob& data) {
    quick_write(lsn, data);

    // Wait for a brief time to allow coalescing multiple writes.
    co_await folly::coro::sleep(std::chrono::microseconds{HS_DYNAMIC_CONFIG(logstore.flush_coalesce_wait_us)});
    do {
        if (records_.status(lsn).is_active) {
            co_return;
        }

        // Flush and post flush check again the status to ensure it is completed.
        co_await stream_->flush();
    } while (true);
}

void LogStore::fill_gap(lsn_t lsn) {
    HS_REL_ASSERT(!append_mode_, "fill_gap on append-mode LogStore (store_id={})", store_id_);

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

folly::coro::Task< sisl::ByteView > LogStore::read(lsn_t lsn) {
    auto exp = records_.try_at(lsn);
    if (!exp) {
        if (exp.error() == sisl::StreamTrackerError::OutOfRange) {
            co_return sisl::ByteView{};
        }
        // NotActive: may be in-flight, flush and retry once.
        co_await stream_->flush();
        exp = records_.try_at(lsn);
        if (!exp) {
            co_return sisl::ByteView{};
        }
    }
    auto* rec = exp.value();
    co_return co_await stream_->read(stream_key{rec->log_id, rec->record_stream_offset, 0});
}

folly::coro::Task< void > LogStore::flush() {
    co_await stream_->flush();
}

folly::coro::Task< void > LogStore::truncate(lsn_t upto_lsn, bool in_memory_only) {
    auto lock = co_await stream_->flush_lock().co_scoped_lock();
    const lsn_t s = head_lsn_.load(std::memory_order_acquire);
    if (upto_lsn < s) {
        THIS_LOGSTORE_LOG(DEBUG, "truncate(upto={}) is below head_lsn={}, no-op", upto_lsn, s);
        co_return;
    }
    lsn_t t = tail_lsn_.load(std::memory_order_acquire);
    if (upto_lsn > t) {
        upto_lsn = t;
    }
    records_.truncate(upto_lsn);
    head_lsn_.store(upto_lsn + 1, std::memory_order_release);
    THIS_LOGSTORE_LOG(INFO, "Truncated upto_lsn={} new head_lsn={} in_memory_only={}", upto_lsn, upto_lsn + 1,
                      in_memory_only);
    if (!in_memory_only) {
        co_await persist_sb();
        // TODO: notify LogStoreManager to recompute cross-store min_trunc_stream_offset and call
        //       stream_->truncate() if our store was holding back the global head.
    }
}

folly::coro::Task< bool > LogStore::rollback(lsn_t to_lsn) {
    THIS_LOGSTORE_LOG(INFO, "rollback request to_lsn={} current tail_lsn={} head_lsn={}", to_lsn,
                      tail_lsn_.load(std::memory_order_relaxed), head_lsn_.load(std::memory_order_relaxed));
    // Drain in-flight via stream flush, then under flush_lock recompute the log_id range to invalidate.  If a
    // concurrent append slipped in between drain and lock-acquire (next_log_id has advanced past our snapshot
    // tail's log_id), retry the drain.
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

        const logid_t cur_tail_logid = records_.at(cur_tail).log_id;
        if (stream_->next_log_id() != cur_tail_logid + 1) {
            // A racing append assigned a log_id past our snapshot — drain again.
            continue;
        }

        // Range of log_ids that are now invalidated for this store.  Persisted in rollback_ranges_; recovery's
        // on_log_found filters records whose log_id falls inside.
        const logid_t from_logid = records_.at(to_lsn + 1).log_id;
        const logid_t to_logid = cur_tail_logid;
        rollback_ranges_.emplace_back(from_logid, to_logid);
        records_.rollback(to_lsn);
        tail_lsn_.store(to_lsn, std::memory_order_release);
        co_await persist_sb();
        THIS_LOGSTORE_LOG(INFO, "rollback complete to_lsn={} log_id range=[{},{}] new tail_lsn={}", to_lsn, from_logid,
                          to_logid, to_lsn);
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
    if (!append_mode_) {
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

void LogStore::on_log_found(lsn_t lsn, const stream_key& key, const sisl::ByteView& data) {
    if (lsn < head_lsn_.load(std::memory_order_acquire)) {
        THIS_LOGSTORE_LOG(DEBUG, "on_log_found skip lsn={} below head_lsn", lsn);
        return;
    }
    if (in_rollback_range(key.log_id)) {
        THIS_LOGSTORE_LOG(DEBUG, "on_log_found skip lsn={} log_id={} in rollback range", lsn, key.log_id);
        return;
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
    if (handler_) {
        handler_(lsn, data);
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
    return exp.value()->trunc_stream_offset;
}

bool LogStore::in_rollback_range(logid_t log_id) const {
    for (auto const& r : rollback_ranges_) {
        if (log_id >= r.first && log_id <= r.second) {
            return true;
        }
    }
    return false;
}

folly::coro::Task< void > LogStore::persist_sb() {
    const uint32_t n = to_u32(rollback_ranges_.size());
    const size_t sz = LogStoreSb::size_for(n);

    auto buf = sisl::make_byte_array(to_u32(sz));
    auto* sb = r_cast< LogStoreSb* >(buf->bytes());
    sb->store_id = store_id_;
    sb->append_mode = append_mode_ ? 1 : 0;
    sb->head_lsn = head_lsn_.load(std::memory_order_acquire);
    sb->n_rollback_ranges = n;
    // logid_range is std::pair, not trivially copyable — copy element-wise instead of memcpy.
    std::copy(rollback_ranges_.begin(), rollback_ranges_.end(), sb->rollback_ranges());
    co_await meta_blk_.write(buf->cbytes(), sz);
}

} // namespace homestore