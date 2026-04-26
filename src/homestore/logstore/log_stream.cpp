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
#include <stdexcept>

#include <fmt/format.h>
#include <folly/small_vector.h>

#include "logstore/log_stream.h"
#include "logstore/log_store.h"
#include "common/defs.h"
#include "device/chunk.h"
#include "device/virtual_dev.h"
#include "managers.h"
#include "meta/meta_client.h"

namespace homestore {

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

folly::coro::Task< shared< LogStream > > LogStream::create(uint64_t stream_id, MetaClient& meta_client,
                                                           const std::string& dev_name,
                                                           const shared< VirtualDev >& vdev, uint64_t chunk_size) {
    auto stream = shared< LogStream >{new LogStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_size}};

    // Allocate the per-stream sb MetaBlk and persist initial empty state.
    stream->sb_mblk_ = co_await meta_client.create_meta_blk(sb_mblk_name(dev_name, stream_id), std::nullopt);
    co_await stream->persist_stream_sb();

    // Pre-allocate one chunk so the first append has somewhere to land.
    co_await stream->expand_to(0);
    co_return stream;
}

folly::coro::Task< shared< LogStream > > LogStream::load(uint64_t stream_id, MetaClient& meta_client,
                                                         const std::string& dev_name, const shared< VirtualDev >& vdev,
                                                         MetaBlk&& sb, sisl::ByteView sb_payload) {
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

    auto stream = shared< LogStream >{new LogStream{stream_id, meta_client, std::string{dev_name}, vdev, chunk_sz}};
    stream->sb_mblk_ = std::move(sb);
    stream->head_offset_ = recovered_head;
    stream->offset_in_first_chunk_ = recovered_head % chunk_sz;
    // tail_offset_, last_crc_, log_id_, last_flush_idx_ are all populated by recover() — called separately by the
    // manager once every LogStore has been opened so on_log_found dispatch can find its target.

    std::vector< shared< Chunk > > chunks;
    chunks.reserve(chunk_ids.size());
    for (auto cid : chunk_ids) {
        if (auto c = vdev->get_chunk(cid))
            chunks.push_back(std::move(c));
    }
    stream->install_chunks(std::move(chunks));

    co_return stream;
}

logid_t LogStream::append(LogStore* store, lsn_t lsn, const sisl::IoBlob& data) {
    const logid_t idx = log_id_.fetch_add(1, std::memory_order_acq_rel);
    pending_flush_size_.fetch_add(data.size(), std::memory_order_relaxed);
    log_records_->create(idx, LogRecord{data, store, lsn});
    return idx;
}

folly::coro::Task< void > LogStream::flush() {
    auto lock = co_await flush_mtx_.co_scoped_lock();

    // Track each group emplaced this turn so we can construct stream_keys for completion callbacks below without
    // having to refer back into per-record state.  Bounded by max_flush_loops so a small_vector is plenty.
    struct EmplacedGroup {
        logid_t from_idx;
        logid_t upto_idx;
        uint64_t group_offset;
    };
    folly::small_vector< EmplacedGroup, max_flush_loops > emplaced;

    // Build groups in a loop while contiguous-active records keep arriving (capped at max_flush_loops).
    int loops = 0;
    while (loops++ < max_flush_loops) {
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

    // Single underlying write of everything emplaced above.  Throws on failure; the tracker is left populated and
    // completion firing is skipped — a flush-write failure is fatal at this layer.
    co_await AppendByteStream::flush();

    // Fire per-record on_write_completion for every record in every group we just made durable.  Walk groups in
    // emplace order, advancing record_stream_offset as we go (group_offset → +log_group_header → +per-record).
    for (auto& g : emplaced) {
        uint64_t rec_off = g.group_offset + sizeof(log_group_header);
        for (logid_t i = g.from_idx; i <= g.upto_idx; ++i) {
            auto& rec = log_records_->at(i);
            rec.store->on_write_completion(rec.lsn, stream_key{i, rec_off, g.group_offset});
            rec_off += sizeof(log_record_header) + rec.data.size();
        }
    }
    log_records_->truncate(last_flush_idx_);
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

        // log_record_header[i] + data[i] for each record
        for (logid_t i = from_idx; i <= upto_idx; ++i) {
            const auto& rec = log_records_->at(i);
            auto* rhdr = r_cast< log_record_header* >(p);
            rhdr->log_id = i;
            rhdr->store_id = rec.store->store_id();
            rhdr->store_lsn = rec.lsn;
            rhdr->size = rec.data.size();
            p += sizeof(log_record_header);
            std::memcpy(p, rec.data.cbytes(), rec.data.size());
            p += rec.data.size();
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

folly::coro::Task< sisl::ByteView > LogStream::read(const stream_key& key) {
    // Read the record header first.
    auto [ec_h, hdr_buf] = co_await AppendByteStream::read(key.record_stream_offset, sizeof(log_record_header));
    if (ec_h) {
        co_return sisl::ByteView{};
    }

    // AppendByteStream::read returns a block-aligned IOBuffer; the requested bytes start at (offset % blk).
    const uint32_t in_buf = key.record_stream_offset % block_size();
    const auto* rhdr = r_cast< const log_record_header* >(hdr_buf.cbytes() + in_buf);
    if (rhdr->log_id != key.log_id) {
        co_return sisl::ByteView{};
    }

    const uint32_t data_size = rhdr->size;
    const uint64_t data_offset = key.record_stream_offset + sizeof(log_record_header);

    auto [ec_d, data_buf] = co_await AppendByteStream::read(data_offset, data_size);
    if (ec_d) {
        co_return sisl::ByteView{};
    }

    // Wrap the IOBuffer into a shared ByteArray (no buffer copy — just a wrapper alloc) so the returned view owns
    // its underlying storage; the caller can retain or drop it as it pleases.
    auto ba = sisl::make_byte_array(std::move(data_buf));
    co_return sisl::ByteView{std::move(ba), data_offset % block_size(), data_size};
}

folly::coro::Task< void > LogStream::recover(lookup_store_fn lookup) {
    // Walk forward from head_offset_ as a chain of LogGroups.  For each candidate group: read its header, validate
    // magic and group_size, read the full group, validate prev_crc + cur_crc against the running chain, then dispatch
    // each record to its owning store via on_log_found.  Stop at the first invalid group; tail_offset_ is set to
    // where the next group would have started.

    // The first surviving group at head_offset_ may legitimately have a prev_crc that doesn't match
    // hs_init_crc_32 — truncation cuts the chain at the head boundary.  Skip the prev_crc check on the first
    // iteration; the first group's own cur_crc validates its integrity.  Subsequent groups chain-validate normally.
    crc32_t expected_prev_crc = 0;
    bool first_group = true;
    uint64_t cursor = head_offset_;

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
        const uint32_t hdr_in_buf = cursor % block_size();
        const auto* hdr = r_cast< const log_group_header* >(hdr_buf.cbytes() + hdr_in_buf);
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

        // Wrap the IOBuffer once into a shared ByteArray; per-record ByteViews below all share the ref-count, so the
        // underlying aligned buffer outlives any individual view (no extra memcpy).
        const uint32_t group_in_buf = cursor % block_size();
        auto group_ba = sisl::make_byte_array(std::move(group_buf));
        const uint8_t* group_bytes = group_ba->cbytes() + group_in_buf;
        const auto* footer = r_cast< const log_group_footer* >(group_bytes + group_size - sizeof(log_group_footer));

        // Validate CRC chain: prev_crc must match expected (skipped on first group — see expected_prev_crc init).
        // cur_crc must always match recomputed.
        if (!first_group && footer->prev_crc != expected_prev_crc) {
            tail_offset_ = prev_tail;
            break;
        }
        const uint64_t covered_len = group_size - sizeof(log_group_footer);
        const crc32_t computed = crc32_ieee(hs_init_crc_32, group_bytes, covered_len);
        if (footer->cur_crc != computed) {
            tail_offset_ = prev_tail;
            break;
        }

        // Group is valid — dispatch each record to its owning store.  cursor is the group's start offset; that's
        // also what each record's stream_key.group_stream_offset gets set to (used by LogStore for safe-truncate).
        uint32_t off_in_group = sizeof(log_group_header);
        for (uint32_t i = 0; i < n_records; ++i) {
            const auto* rhdr = r_cast< const log_record_header* >(group_bytes + off_in_group);
            const uint64_t rec_stream_offset = cursor + off_in_group;
            const uint32_t data_off_in_buf = group_in_buf + off_in_group + sizeof(log_record_header);

            if (auto* store = lookup ? lookup(rhdr->store_id) : nullptr) {
                sisl::ByteView data_view{group_ba, data_off_in_buf, rhdr->size};
                store->on_log_found(rhdr->store_lsn, stream_key{rhdr->log_id, rec_stream_offset, cursor}, data_view);
            }
            // else: orphan record — store_id was never opened; manager handles cleanup.

            if (rhdr->log_id + 1 > log_id_.load(std::memory_order_relaxed)) {
                log_id_.store(rhdr->log_id + 1, std::memory_order_relaxed);
            }
            last_flush_idx_ = std::max(last_flush_idx_, rhdr->log_id);
            off_in_group += sizeof(log_record_header) + rhdr->size;
        }

        // Advance chain.
        expected_prev_crc = footer->cur_crc;
        last_crc_ = footer->cur_crc;
        first_group = false;
        cursor += group_size;
        tail_offset_ = prev_tail; // restore; next iteration recomputes if needed
    }

    // Final tail is where the first invalid (or missing) group would have started.
    tail_offset_ = cursor;
}

folly::coro::Task< void > LogStream::persist_flush_metadata() {
    // No-op: LogStream rediscovers tail at recovery via CRC walk; the sb only needs to be written when chunks
    // change (via inherited init/remove_chunk_mblk overrides) or on truncate.  Skipping per-flush sb writes saves
    // one MetaBlk write per flush on the hot path.
    co_return;
}
} // namespace homestore