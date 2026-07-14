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

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

#include <chrono>

#include <fmt/format.h>
#include "common/async.h"
#include <folly/small_vector.h>
#include "sisl/fds/utils.h" // Clock, get_elapsed_time_us

#include "homestore/logstore/log_stream.h"
#include "homestore/base/homestore_config.h" // HS_DYNAMIC_CONFIG
#include "common/defs.h"
#include "homestore/device/chunk.h"
#include "homestore/device/virtual_dev.h"
#include "iomanager/iomanager.h" // iomanager::spawn_detached for size-triggered auto-flush
#include "homestore/managers.h"
#include "homestore/meta/meta_client.h"

namespace homestore {

// Per-stream log helper.  The "sid" tag lets a busy multi-stream log be filtered to one stream's lifecycle.
#define LSTREAM_LOG(level, msg, ...) HS_SUBMOD_LOG(level, logstream, , "sid", stream_id(), msg, ##__VA_ARGS__)

// Generate a fresh non-zero 32-bit chain seed.  Non-zero so it's distinguishable from the default-initialized
// AppendByteStreamSb::chain_seed{0} on the recovery path of a stream that was never created with this code.
static uint32_t fresh_chain_seed() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution< uint32_t > dist{1, std::numeric_limits< uint32_t >::max()};
    return dist(rng);
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction / factories
// ─────────────────────────────────────────────────────────────────────────────

LogStream::LogStream(uint64_t stream_id, MetaClient& meta_client, std::string dev_name,
                     const shared< VirtualDev >& vdev, uint64_t chunk_size) :
        AppendByteStream{stream_id, meta_client, std::move(dev_name), vdev, chunk_size, /*concurrent_safe=*/false},
        log_records_{std::make_unique< sisl::StreamTracker< LogRecord, false, false > >("LogStream", -1)} {
}

std::string LogStream::sb_mblk_name(const std::string& dev, uint64_t stream_id) {
    return fmt::format("{}_logstream_sb_{}", dev, stream_id);
}

Async< shared< LogStream > > LogStream::create(uint64_t stream_id, MetaClient& meta_client, const std::string& dev_name,
                                               const shared< VirtualDev >& vdev, uint64_t chunk_size) {
    LOGINFOMOD(logstream, "create: sid={} dev={} chunk_size={}", stream_id, dev_name, chunk_size);
    auto stream = shared< LogStream >{new LogStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size}};

    // Seed the chain at a non-zero random value so recovery can distinguish stale on-disk groups (recycled chunk,
    // pre-truncate-all data) from this stream's epoch.  Set BEFORE persist_stream_sb so the initial sb carries it.
    stream->chain_seed_ = fresh_chain_seed();
    stream->last_crc_ = stream->chain_seed_;

    // Allocate the per-stream sb MetaBlk and persist initial empty state.
    stream->sb_mblk_ = co_await meta_client.create_meta_blk(sb_mblk_name(dev_name, stream_id), std::nullopt);
    co_await stream->persist_stream_sb();

    // Pre-allocate one chunk so the first append has somewhere to land.
    co_await stream->expand_to(0);
    stream->start_flush_timer();
    LOGTRACEMOD(logstream, "create: sid={} ready, num_chunks={}", stream_id, stream->num_chunks());
    co_return stream;
}

Async< shared< LogStream > > LogStream::load(uint64_t stream_id, MetaClient& meta_client, const std::string& dev_name,
                                             const shared< VirtualDev >& vdev, MetaBlk&& sb,
                                             sisl::IoBufView sb_payload) {
    if (sb_payload.size() < sizeof(AppendByteStreamSb)) {
        throw std::runtime_error(
            fmt::format("LogStream::load: sb payload too small for stream {} on {}", stream_id, dev_name));
    }
    const auto* s = r_cast< const AppendByteStreamSb* >(sb_payload.bytes());
    if (s->chunk_size == 0) {
        throw std::runtime_error(
            fmt::format("LogStream::load: sb has chunk_size=0 for stream {} on {}", stream_id, dev_name));
    }
    const uint64_t chunk_sz = s->chunk_size;
    const uint64_t recovered_head = s->head_offset;
    std::vector< uint32_t > chunk_ids;
    chunk_ids.reserve(s->n_chunks);
    for (uint32_t i = 0; i < s->n_chunks; ++i) {
        chunk_ids.push_back(s->chunk_ids()[i]);
    }

    LOGINFOMOD(logstream, "load: sid={} dev={} chunk_size={} head_offset={} n_chunks={} chain_seed={:#x}", stream_id,
               dev_name, chunk_sz, recovered_head, s->n_chunks, s->chain_seed);
    auto stream = shared< LogStream >{new LogStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz}};
    stream->sb_mblk_ = std::move(sb);
    stream->head_offset_ = recovered_head;
    stream->offset_in_first_chunk_ = recovered_head % chunk_sz;
    stream->chain_seed_ = s->chain_seed;
    stream->last_crc_ = s->chain_seed; // root the chain at the persisted seed; recover() will advance it
    // tail_offset_, log_id_, last_flush_idx_ are all populated by recover() — called separately by the
    // manager once every LogStore has been opened so on_log_found dispatch can find its target.

    std::vector< shared< Chunk > > chunks;
    chunks.reserve(chunk_ids.size());
    for (auto cid : chunk_ids) {
        if (auto c = vdev->get_chunk(cid))
            chunks.push_back(std::move(c));
    }
    stream->install_chunks(std::move(chunks));

    stream->start_flush_timer();
    LOGTRACEMOD(logstream, "load: sid={} installed {} chunks", stream_id, stream->num_chunks());
    co_return stream;
}

logid_t LogStream::append(LogStreamClient* client, lsn_t lsn, const LogBlob& data) {
    const logid_t idx = log_id_.fetch_add(1, std::memory_order_acq_rel);
    const auto sz = to_i64(data.size());
    const int64_t prev = pending_flush_size_.fetch_add(sz, std::memory_order_relaxed);
    log_records_->create(idx, LogRecord{data, client, lsn});

    // Size-based auto-flush: only the appender that takes pending_flush_size_ from below threshold to >=
    // threshold spawns the detached flush.  Subsequent appenders that observe an already-above-threshold value
    // skip the spawn — flush() decrements the counter on success, naturally re-arming the next crossing.
    const int64_t threshold = to_i64(HS_DYNAMIC_CONFIG(logstore.flush_threshold_size));
    if (prev < threshold && (prev + sz) >= threshold) {
        iomanager::spawn_detached(iomanager::ReactorTarget::any(),
                                  [self = shared_from_this()]() -> Async< void > { co_await self->flush(); });
    }
    return idx;
}

Async< void > LogStream::flush() {
    auto lock = co_await flush_mtx_.co_scoped_lock();

    // Track each group emplaced this turn so we can construct stream_keys for completion callbacks below without
    // having to refer back into per-record state.  Inline capacity covers the common case; small_vector spills
    // to heap if HS_DYNAMIC_CONFIG(logstore.max_flush_loops) is hot-swapped above kFlushGroupsInlineCapacity.
    struct EmplacedGroup {
        logid_t from_idx;
        logid_t upto_idx;
        uint64_t group_offset;
    };
    folly::small_vector< EmplacedGroup, kFlushGroupsInlineCapacity > emplaced;

    // Build groups in a loop while contiguous-active records keep arriving (capped at the hotswap config).
    const auto max_loops = HS_DYNAMIC_CONFIG(logstore.max_flush_loops);
    uint32_t loops = 0;
    while (loops++ < max_loops) {
        const logid_t from = last_flush_idx_ + 1;
        const logid_t upto = log_records_->active_upto(last_flush_idx_ + 1);
        if (from > upto) {
            break;
        }
        const uint64_t goff = build_and_emplace_group(from, upto);
        emplaced.push_back(EmplacedGroup{from, upto, goff});
        last_flush_idx_ = upto;
    }

    if (emplaced.empty()) {
        co_return; // nothing newly emplaced
    }

    LSTREAM_LOG(TRACE, "flush: emplaced {} group(s), tail_off={}", emplaced.size(), tail_offset());

    // Single underlying write of everything emplaced above.  Throws on failure; the tracker is left populated and
    // completion firing is skipped — a flush-write failure is fatal at this layer.
    co_await AppendByteStream::flush();

    // Fire per-record on_write_completion for every record in every group we just made durable.  Walk groups in
    // emplace order, advancing record_stream_offset as we go (group_offset → +log_group_header → +per-record).
    // While walking, also accumulate the total flushed payload bytes so we can decrement pending_flush_size_
    // below — a single fetch_sub re-arms the size-threshold crossing in append() for the next flush cycle.
    int64_t flushed_bytes = 0;
    for (auto& g : emplaced) {
        uint64_t rec_off = g.group_offset + sizeof(log_group_header);
        for (logid_t i = g.from_idx; i <= g.upto_idx; ++i) {
            auto rec = log_records_->at(i);
            rec.client->on_write_completion(rec.lsn, stream_key{i, rec_off, g.group_offset});
            rec_off += sizeof(log_record_header) + rec.data.size();
            flushed_bytes += to_i64(rec.data.size());
        }
    }
    pending_flush_size_.fetch_sub(flushed_bytes, std::memory_order_relaxed);

    // Stamp the flush time AFTER the write succeeded — the auto-flush timer reads this to gate the
    // max_time_between_flush_us check, so we never want to advance it for an aborted flush.
    last_flush_time_.store(Clock::now(), std::memory_order_relaxed);
    log_records_->truncate(last_flush_idx_);
}

void LogStream::start_flush_timer() {
    flush_timer_.start(iomanager::ReactorTarget::any(),
                       std::chrono::microseconds(HS_DYNAMIC_CONFIG(logstore.flush_timer_frequency_us)),
                       iomanager::TimerKind::Recurring, [this]() -> Async< void > {
                           // Cheap pre-check: skip if no pending bytes.  pending_flush_size_ is decremented by
                           // flush() on success, so >0 means records haven't reached disk yet.
                           if (pending_flush_size_.load(std::memory_order_acquire) <= 0) {
                               co_return;
                           }
                           if (get_elapsed_time_us(last_flush_time_.load(std::memory_order_relaxed)) >=
                               HS_DYNAMIC_CONFIG(logstore.max_time_between_flush_us)) {
                               co_await flush();
                           }
                       });
}

Async< void > LogStream::stop() {
    co_await flush_timer_.stop();
}

Async< void > LogStream::truncate(const stream_key& key) {
    co_await AppendByteStream::truncate(key.group_stream_offset);

    // Truncate-all path: AppendByteStream collapses head==tail to (0,0).  Bump chain_seed_ so any stale on-disk
    // groups left in the anchor chunk fail recovery's first-group prev_crc check.  Reset chain state and persist
    // the sb again to land the new seed on disk.
    if (head_offset_ == 0 && tail_offset_ == 0) {
        const uint32_t old_seed = chain_seed_;
        chain_seed_ = fresh_chain_seed();
        last_crc_ = chain_seed_;
        last_flush_idx_ = -1;
        log_id_.store(0, std::memory_order_relaxed);

        LSTREAM_LOG(INFO, "truncate-all: bumped chain_seed {:#x} -> {:#x}", old_seed, chain_seed_);
        co_await persist_stream_sb();
    }
}

uint64_t LogStream::build_and_emplace_group(logid_t from_idx, logid_t upto_idx) {
    // Compute the group's on-disk size.
    uint32_t group_size = sizeof(log_group_header) + sizeof(log_group_footer);
    for (logid_t i = from_idx; i <= upto_idx; ++i) {
        group_size += sizeof(log_record_header) + log_records_->at(i).data.size();
    }

    // emplace into the parent's flush buffer; the lambda fills the bytes in place.
    const uint64_t group_offset = AppendByteStream::emplace(group_size, [&](sisl::Blob buf) {
        uint8_t* p = buf.bytes();

        // log_group_header
        auto* hdr = r_cast< log_group_header* >(p);
        hdr->magic = LOG_GROUP_MAGIC;
        hdr->n_records = to_u32(upto_idx - from_idx + 1);
        hdr->group_size = group_size;
        p += sizeof(log_group_header);

        // log_record_header[i] + data[i] for each record. The on-disk record stays contiguous; the scatter-
        // gather sits only on the in-memory side (LogBlob's parts[]). We coalesce per-record into the group
        // buffer here — one memcpy per part, same total byte count as the single-IoBufSpan path.
        for (logid_t i = from_idx; i <= upto_idx; ++i) {
            const auto rec = log_records_->at(i);
            auto* rhdr = r_cast< log_record_header* >(p);
            rhdr->log_id = i;
            rhdr->store_id = rec.client->store_id();
            rhdr->store_lsn = rec.lsn;
            rhdr->size = rec.data.size();
            p += sizeof(log_record_header);
            for (uint8_t k = 0; k < rec.data.n_parts; ++k) {
                std::memcpy(p, rec.data.parts[k].cbytes(), rec.data.parts[k].size());
                p += rec.data.parts[k].size();
            }
        }

        // log_group_footer: prev_crc from the chain so far, cur_crc covers everything written above.
        auto* footer = r_cast< log_group_footer* >(p);
        footer->prev_crc = last_crc_;
        const auto* covered_begin = r_cast< const uint8_t* >(buf.cbytes());
        const auto covered_len = to_u64(p - buf.bytes());
        const crc32_t cur = crc32_ieee(hs_init_crc_32, covered_begin, covered_len);
        footer->cur_crc = cur;
        last_crc_ = cur;
    });

    return group_offset;
}

Async< sisl::IoBufView > LogStream::read(const stream_key& key) {
    // Read the record header first.
    auto [ec_h, hdr_buf] = co_await AppendByteStream::read(key.record_stream_offset, sizeof(log_record_header));
    if (ec_h) {
        co_return sisl::IoBufView{};
    }

    // AppendByteStream::read returns a IoBufView already sliced to start exactly at byte_offset — index from 0.
    const auto* rhdr = r_cast< const log_record_header* >(hdr_buf.bytes());
    if (rhdr->log_id != key.log_id) {
        co_return sisl::IoBufView{};
    }

    const uint32_t data_size = rhdr->size;
    const uint64_t data_offset = key.record_stream_offset + sizeof(log_record_header);

    auto [ec_d, data_buf] = co_await AppendByteStream::read(data_offset, data_size);
    if (ec_d) {
        co_return sisl::IoBufView{};
    }

    // data_buf is already a IoBufView sliced to start at data_offset with size == data_size — return as-is.
    co_return data_buf;
}

Async< void > LogStream::recover(lookup_store_fn lookup) {
    LSTREAM_LOG(INFO, "recover: walking chain from head_offset={}", head_offset_);
    // Walk forward from head_offset_ as a chain of LogGroups.  For each candidate group: read its header, validate
    // magic and group_size, read the full group, validate prev_crc + cur_crc against the running chain, then dispatch
    // each record to its owning store via on_log_found.  Stop at the first invalid group; tail_offset_ is set to
    // where the next group would have started.

    // First-group prev_crc handling depends on where head is:
    //   • head == 0 (fresh stream, or after truncate-all): expect prev_crc == chain_seed_.  Stale data from a
    //     prior epoch / recycled chunk has a different seed and fails immediately.
    //   • head != 0 (after partial truncate): the new first group's prev_crc was set at flush time to the prior
    //     group's cur_crc, which we no longer have.  Skip the check on iteration 0; subsequent groups still
    //     chain-validate.
    bool skip_prev_crc_check = (head_offset_ != 0);
    crc32_t expected_prev_crc = chain_seed_;
    uint64_t cursor = head_offset_;
    uint32_t groups_recovered = 0;
    uint32_t records_recovered = 0;

    while (true) {
        // AppendByteStream::read clamps to tail_offset_; set tail optimistically to "end of chunks" so reads can
        // probe ahead.  Restored to prev_tail before any early-exit.
        const uint64_t prev_tail = tail_offset_;
        const uint64_t first_chunk_abs = head_offset_ - offset_in_first_chunk_;
        tail_offset_ = first_chunk_abs + num_chunks() * chunk_size();

        if (cursor + sizeof(log_group_header) > tail_offset_) {
            tail_offset_ = prev_tail;
            break;
        }

        auto [ec_h, hdr_buf] = co_await AppendByteStream::read(cursor, sizeof(log_group_header));
        if (ec_h) {
            tail_offset_ = prev_tail;
            break;
        }
        // AppendByteStream::read returns a IoBufView pre-sliced to start at byte_offset — index from 0.
        const auto* hdr = r_cast< const log_group_header* >(hdr_buf.bytes());
        if (hdr->magic != LOG_GROUP_MAGIC || hdr->group_size < sizeof(log_group_header) + sizeof(log_group_footer)) {
            tail_offset_ = prev_tail;
            break;
        }
        if (cursor + hdr->group_size > tail_offset_) {
            tail_offset_ = prev_tail;
            break;
        }

        // Read the whole group (header + records + footer) for CRC validation and record dispatch.
        const uint32_t group_size = hdr->group_size;
        const uint32_t n_records = hdr->n_records;
        auto [ec_g, group_buf] = co_await AppendByteStream::read(cursor, group_size);
        if (ec_g) {
            tail_offset_ = prev_tail;
            break;
        }

        // group_buf is a IoBufView already sliced to the group's start; per-record sub-views below share its
        // underlying refcount via the IoBufView(IoBufView, offset, size) ctor (no extra memcpy).
        const uint8_t* group_bytes = group_buf.bytes();
        const auto* footer = r_cast< const log_group_footer* >(group_bytes + group_size - sizeof(log_group_footer));

        // Validate CRC chain.  Skip prev_crc on the iteration-0 partial-truncate case (see flag init above);
        // every other iteration must chain to the prior group's cur_crc.  cur_crc is always recomputed.
        if (!skip_prev_crc_check && footer->prev_crc != expected_prev_crc) {
            LSTREAM_LOG(WARN, "recover: prev_crc mismatch at off={} (expected={:#x} got={:#x}) — chain ends here",
                        cursor, expected_prev_crc, footer->prev_crc);
            tail_offset_ = prev_tail;
            break;
        }
        skip_prev_crc_check = false;
        const uint64_t covered_len = group_size - sizeof(log_group_footer);
        const crc32_t computed = crc32_ieee(hs_init_crc_32, group_bytes, covered_len);
        if (footer->cur_crc != computed) {
            LSTREAM_LOG(ERROR,
                        "recover: cur_crc mismatch at off={} (expected={:#x} got={:#x}) — probing for torn write",
                        cursor, computed, footer->cur_crc);
            tail_offset_ = prev_tail;
            break;
        }

        // Group is valid — dispatch each record to its owning store.  cursor is the group's start offset; that's
        // also what each record's stream_key.group_stream_offset gets set to (used by LogStore for safe-truncate).
        uint32_t off_in_group = sizeof(log_group_header);
        for (uint32_t i = 0; i < n_records; ++i) {
            const auto* rhdr = r_cast< const log_record_header* >(group_bytes + off_in_group);
            const uint64_t rec_stream_offset = cursor + off_in_group;
            const uint32_t data_off_in_view = off_in_group + sizeof(log_record_header);

            if (auto* client = lookup ? lookup(rhdr->store_id) : nullptr) {
                // Sub-view sharing group_buf's underlying refcount — no copy, group's buffer outlives the view.
                sisl::IoBufView data_view{group_buf, data_off_in_view, rhdr->size};
                client->on_log_found(rhdr->store_lsn, stream_key{rhdr->log_id, rec_stream_offset, cursor}, data_view);
            }
            // else: orphan record — store_id was never opened; manager handles cleanup.

            if (rhdr->log_id + 1 > log_id_.load(std::memory_order_relaxed)) {
                log_id_.store(rhdr->log_id + 1, std::memory_order_relaxed);
            }
            last_flush_idx_ = std::max(last_flush_idx_, rhdr->log_id);
            off_in_group += sizeof(log_record_header) + rhdr->size;
            ++records_recovered;
        }
        ++groups_recovered;

        // Advance chain.
        expected_prev_crc = footer->cur_crc;
        last_crc_ = footer->cur_crc;
        cursor += group_size;
        tail_offset_ = prev_tail; // restore; next iteration recomputes if needed
    }

    // Final tail is where the first invalid (or missing) group would have started.  Use AppendByteStream's
    // resume_writes_at so flush_buf_ is primed with any partial-tail-block bytes (the next flush would otherwise
    // clobber them by writing a fresh block at offset 0 within the partial block).
    co_await resume_writes_at(cursor);
    LSTREAM_LOG(INFO, "recover: done — {} groups, {} records, tail_offset={} log_id={}", groups_recovered,
                records_recovered, tail_offset_, log_id_.load());

    // Torn-write detection: only meaningful if we walked at least one group successfully — i.e. there was a
    // live chain that then broke.  When groups_recovered == 0 the break is at the very first probe (head ==
    // current cursor), which is the post-truncate-all leftover-data case (chain_seed rejected stale group #0);
    // any downstream "valid" group is stale-from-prior-epoch, not a torn middle write.
    if (groups_recovered > 0) {
        if (auto found = co_await probe_for_torn_write(cursor)) {
            LSTREAM_LOG(ERROR, "recover: torn middle write detected — chain break at off={}, valid group at off={}",
                        cursor, *found);
            throw std::runtime_error(fmt::format(
                "LogStream::recover: torn middle write detected — chain break at stream offset {}, but a valid "
                "LogGroup exists at offset {} (within {} block lookahead). Refusing to silently truncate; this "
                "stream needs manual investigation.",
                cursor, *found, HS_DYNAMIC_CONFIG(logstore.recovery_max_blks_read_for_additional_check)));
        }
    }
}

Async< std::optional< uint64_t > > LogStream::probe_for_torn_write(uint64_t bad_off) {
    const uint32_t max_blocks = HS_DYNAMIC_CONFIG(logstore.recovery_max_blks_read_for_additional_check);
    if (max_blocks == 0)
        co_return std::nullopt;

    // Round bad_off UP to the next strict block boundary; the partial block at bad_off itself is part of the
    // damage and is skipped.  We're looking for downstream evidence of a *different* good group.
    const uint64_t probe_start = ((bad_off + block_size()) / block_size()) * block_size();

    // Optimistically bump tail_offset_ to end-of-allocated-chunks so AppendByteStream::read can probe past
    // the recovered tail.  Restored before return regardless of outcome.
    const uint64_t prev_tail = tail_offset_;
    const uint64_t first_chunk_abs = head_offset_ - offset_in_first_chunk_;
    const uint64_t optimistic_tail = first_chunk_abs + num_chunks() * chunk_size();
    tail_offset_ = optimistic_tail;

    std::optional< uint64_t > found_off;
    for (uint32_t i = 0; i < max_blocks; ++i) {
        const uint64_t probe = probe_start + to_u64(i) * block_size();
        if (probe + sizeof(log_group_header) > optimistic_tail)
            break;

        auto [ec_h, hdr_buf] = co_await AppendByteStream::read(probe, sizeof(log_group_header));
        if (ec_h)
            continue;

        // AppendByteStream::read returns a IoBufView pre-sliced to start at byte_offset — index from 0.
        const auto* hdr = r_cast< const log_group_header* >(hdr_buf.bytes());
        if (hdr->magic != LOG_GROUP_MAGIC)
            continue;

        if (hdr->group_size < sizeof(log_group_header) + sizeof(log_group_footer))
            continue;

        if (probe + hdr->group_size > optimistic_tail)
            continue;

        // Magic + size sanity passed — read the full group and validate cur_crc.  We deliberately skip the
        // prev_crc check here: the chain across the torn region is broken by definition, so prev_crc would
        // not match expected.  cur_crc is self-contained and proves the group itself is intact.
        const uint32_t group_size = hdr->group_size;
        auto [ec_g, group_buf] = co_await AppendByteStream::read(probe, group_size);
        if (ec_g)
            continue;

        const uint8_t* group_bytes = group_buf.bytes();

        const uint64_t actual_group_size = group_size - sizeof(log_group_footer);
        const auto* footer = r_cast< const log_group_footer* >(group_bytes + actual_group_size);
        const crc32_t computed = crc32_ieee(hs_init_crc_32, group_bytes, actual_group_size);
        if (footer->cur_crc == computed) {
            found_off = probe;
            break;
        }
    }

    tail_offset_ = prev_tail;
    co_return found_off;
}

Async< void > LogStream::persist_flush_metadata() {
    // No-op: LogStream rediscovers tail at recovery via CRC walk; the sb only needs to be written when chunks
    // change (via inherited init/remove_chunk_mblk overrides) or on truncate.  Skipping per-flush sb writes saves
    // one MetaBlk write per flush on the hot path.
    co_return;
}
} // namespace homestore