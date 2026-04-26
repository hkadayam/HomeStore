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

#include "logstore/log_store.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <fmt/format.h>
#include <folly/coro/Sleep.h>

#include "common/defs.h"
#include "meta/meta_client.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / factories
// ─────────────────────────────────────────────────────────────────────────────

LogStore::LogStore(shared< LogStream > stream, MetaBlkWrapper&& mb) :
        stream_{std::move(stream)}, meta_blk_{std::move(mb)} {
    sisl::ByteView sb_payload = meta_blk_.read();
    if (sb_payload.size() < sizeof(LogStoreSb)) {
        throw std::runtime_error(fmt::format("LogStore::load: sb payload too small on {}", dev_name));
    }

    const auto* sb = r_cast< const LogStoreSb* >(sb_payload.bytes());
    store_id_ = sb->store_id;
    append_mode_ = (sb->append_mode != 0);
    start_lsn_ = sb->start_lsn;

    std::vector< logid_range > ranges;
    ranges.reserve(s->n_rollback_ranges);
    for (uint32_t i = 0; i < s->n_rollback_ranges; ++i) {
        ranges.push_back(s->rollback_ranges()[i]);
    }
    records_ = StreamTracker{"LogStore", start_lsn - 1};
    rollback_ranges_ = std::move(ranges);
}

std::string LogStore::sb_mblk_name(const std::string& dev, logstore_id_t sid) {
    return fmt::format("{}_logstore_sb_{}", dev, sid);
}

folly::coro::Task< shared< LogStore > > LogStore::create(logstore_id_t sid, MetaClient& meta_client,
                                                         const std::string& dev_name, shared< LogStream > stream,
                                                         bool is_append_mode) {
    auto mb = MetaBlkWrapper::create(meta_client, fmt::format("LogStore_{}", sid));
    LogStoreSb sb;
    sb.store_id = sid;
    sb.append_mode = is_append_mode;
    sb.start_lsn = 0;
    sb.n_rollback_ranges = 0;

    co_await mb.write(to_u8ptr(&sb), sizeof(sb));
    co_return std::make_shared< LogStore >(std::move(stream), std::move(mb));
}

folly::coro::Task< shared< LogStore > > LogStore::load(shared< LogStream > stream, MetaBlkWrapper&& mb) {
    co_return std::make_shared< LogStore >(std::move(stream), std::move(mb));
}

void LogStore::open(log_replay_cb handler) {
    handler_ = std::move(handler);
    is_open_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Append-mode API
// ─────────────────────────────────────────────────────────────────────────────

lsn_t LogStore::quick_append(const sisl::IoBlob& data) {
    const lsn_t lsn = tail_lsn_.fetch_add(1, std::memory_order_acq_rel) + 1;
    stream_->append(this, lsn, data);
    return lsn;
}

folly::coro::Task< lsn_t > LogStore::append_and_flush(const sisl::IoBlob& data) {
    const lsn_t lsn = quick_append(data);

    // Wait for a brief time to allow coalescing multiple writes.
    co_await folly::coro::sleep(flush_coalesce_wait_);
    if (records_.status(lsn).is_completed) {
        // Some other append, has flushed ours, so return
        co_return lsn;
    }

    // Flush and post flush we should have created lsn record
    co_await stream_->flush();
    co_return lsn;
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-append-mode API
// ─────────────────────────────────────────────────────────────────────────────

void LogStore::quick_write(lsn_t lsn, const sisl::IoBlob& data) {
    HS_REL_ASSERT(!append_mode_, "quick_write on append-mode LogStore (store_id={})", store_id_);
    stream_->append(this, lsn, data);
}

folly::coro::Task< void > LogStore::write_and_flush(lsn_t lsn, const sisl::IoBlob& data) {
    quick_write(lsn, data);
    co_await flush_upto(lsn);
}

void LogStore::fill_gap(lsn_t lsn) {
    HS_REL_ASSERT(!append_mode_, "fill_gap on append-mode LogStore (store_id={})", store_id_);
    records_.create(lsn, LogStoreRecord{});
    lsn_t cur = next_lsn_.load(std::memory_order_relaxed);
    while (cur < lsn + 1 && !next_lsn_.compare_exchange_weak(cur, lsn + 1, std::memory_order_release)) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// Read / flush / truncate / rollback
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< sisl::ByteView > LogStore::read(lsn_t lsn) {
    if (lsn < start_lsn_.load(std::memory_order_acquire) || lsn >= tail_lsn_.load(std::memory_order_acquire)) {
        co_return sisl::ByteView{};
    }
    co_await flush_upto(lsn);
    if (lsn > tail_lsn_.load(std::memory_order_acquire)) {
        co_return sisl::ByteView{};
    }
    const stream_key key = records_.at(lsn).dev_key;
    co_return co_await stream_->read(key);
}

folly::coro::Task< void > LogStore::flush() {
    co_await stream_->flush();
}

folly::coro::Task< void > LogStore::truncate(lsn_t upto_lsn, bool in_memory_only) {
    auto lock = co_await stream_->flush_lock().co_scoped_lock();
    const lsn_t s = start_lsn_.load(std::memory_order_acquire);
    if (upto_lsn < s) {
        co_return;
    }
    lsn_t t = tail_lsn_.load(std::memory_order_acquire);
    if (upto_lsn > t) {
        upto_lsn = t;
    }
    records_.truncate(upto_lsn);
    start_lsn_.store(upto_lsn + 1, std::memory_order_release);
    co_await persist_sb();
    (void)in_memory_only;
}

folly::coro::Task< bool > LogStore::rollback(lsn_t to_lsn) {
    while (true) {
        co_await stream_->flush();
        auto lock = co_await stream_->flush_lock().co_scoped_lock();

        if (to_lsn < start_lsn_.load(std::memory_order_acquire)) {
            co_return false;
        }
        const lsn_t cur_tail = tail_lsn_.load(std::memory_order_acquire);
        if (to_lsn >= cur_tail) {
            co_return true;
        }

        const logid_t cur_tail_logid = records_.at(cur_tail).dev_key.log_id;
        if (stream_->next_log_id() != cur_tail_logid + 1) {
            continue;
        }

        const logid_t from_logid = records_.at(to_lsn + 1).dev_key.log_id;
        const logid_t to_logid = cur_tail_logid;
        rollback_ranges_.emplace_back(from_logid, to_logid);
        records_.rollback(to_lsn);
        tail_lsn_.store(to_lsn, std::memory_order_release);
        next_lsn_.store(to_lsn + 1, std::memory_order_release);
        co_await persist_sb();
        co_return true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Callbacks from LogStream
// ─────────────────────────────────────────────────────────────────────────────

void LogStore::on_write_completion(lsn_t lsn, const stream_key& key) {
    uint64_t trunc_stream_offset{key.group_stream_offset};
    if (!append_mode) {
        lsn_t cur_tail = tail_lsn_.load(std::memory_order_acquire);
        if (lsn > cur_tail) {
            // lsn flushed bigger than current tail, so it is in-order, just add truncable stream offset to cur group
            while (lsn > cur_tail && !tail_lsn_.compare_exchange_weak(cur_tail, lsn, std::memory_order_acq_rel)) {}
        } else {
            trunc_stream_offset = records_.at(cur_tail).trunc_stream_offset;
        }
    }
    records_.create(lsn, key.log_id, key.record_stream_offset, trunc_stream_offset);
}

void LogStore::on_log_found(lsn_t lsn, const stream_key& key, const sisl::ByteView& data) {
    if (lsn < start_lsn_.load(std::memory_order_acquire)) {
        return;
    }
    if (in_rollback_range(key.log_id)) {
        return;
    }
    stream_key trunc_key{};
    const lsn_t cur_tail = tail_lsn_.load(std::memory_order_acquire);
    if (cur_tail < lsn) {
        tail_lsn_.store(lsn, std::memory_order_release);
        trunc_key = key;
    } else {
        trunc_key = records_.at(cur_tail).trunc_key;
    }
    records_.create(lsn, LogStoreRecord{key, trunc_key});
    lsn_t cur_next = next_lsn_.load(std::memory_order_relaxed);
    while (cur_next < lsn + 1 && !next_lsn_.compare_exchange_weak(cur_next, lsn + 1, std::memory_order_release)) {}
    if (handler_) {
        handler_(lsn, data);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors / helpers
// ─────────────────────────────────────────────────────────────────────────────

std::optional< uint64_t > LogStore::head_stream_offset() const {
    const lsn_t s = start_lsn_.load(std::memory_order_acquire);
    const lsn_t t = tail_lsn_.load(std::memory_order_acquire);
    if (s > t) {
        return std::nullopt;
    }
    return records_.at(s).trunc_key.group_stream_offset;
}

bool LogStore::in_rollback_range(logid_t log_id) const {
    for (auto const& r : rollback_ranges_) {
        if (log_id >= r.first && log_id <= r.second) {
            return true;
        }
    }
    return false;
}

folly::coro::Task< void > LogStore::flush_upto(lsn_t upto_lsn) {
    if (append_mode_) {
        if (records_.status(lsn).is_completed) {
            // Some other append, has flushed ours, so return
            co_return;
        }

        // Flush and post flush we should have created lsn record
        co_await stream_->flush();
    } else {
        comp_lsn = records_.active_upto(prev_contiguous_lsn_hint_.load(std::memory_order_relaxed));
        if (comp_lsn >= upto_lsn) {
            prev_contiguous_lsn_hint_.store(comp_lsn, std::memory_order_release);
            co_return;
        }
    }

    if (tail_lsn_.load(std::memory_order_acquire) >= upto_lsn) {
        co_return;
    }
    co_await folly::coro::sleep(flush_coalesce_wait_);
    if (tail_lsn_.load(std::memory_order_acquire) >= upto_lsn) {
        co_return;
    }
    co_await stream_->flush();
    for (int i = 0; i < max_flush_retries && tail_lsn_.load(std::memory_order_acquire) < upto_lsn; ++i) {
        co_await folly::coro::sleep(flush_retry_wait_);
    }
}

folly::coro::Task< void > LogStore::persist_sb() {
    const uint32_t n = to_u32(rollback_ranges_.size());
    const size_t bytes = LogStoreSb::size_for(n);
    auto buf = sisl::make_byte_array(to_u32(bytes));
    auto* sb = r_cast< LogStoreSb* >(buf->bytes());
    sb->store_id = store_id_;
    sb->append_mode = append_mode_ ? 1 : 0;
    sb->start_lsn = start_lsn_.load(std::memory_order_acquire);
    sb->n_rollback_ranges = n;
    if (n > 0) {
        std::memcpy(sb->rollback_ranges(), rollback_ranges_.data(), n * sizeof(logid_range));
    }
    co_await meta_client_.write_meta_blk(sb_mblk_, buf);
}

} // namespace homestore