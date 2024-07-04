/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
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
#include <iterator>
#include <string>

#include <fmt/format.h>
#include <iomgr/iomgr.hpp>
#include <sisl/utility/thread_factory.hpp>

#include <homestore/homestore.hpp>
#include <homestore/logstore_service.hpp>
#include "common/homestore_assert.hpp"
#include "log_dev.hpp"

namespace homestore {
SISL_LOGGING_DECL(logstore)

#define THIS_LOGSTORE_LOG(level, msg, ...) HS_SUBMOD_LOG(level, logstore, , "log_store", m_fq_name, msg, __VA_ARGS__)
#define THIS_LOGSTORE_PERIODIC_LOG(level, msg, ...)                                                                    \
    HS_PERIODIC_DETAILED_LOG(level, logstore, "log_store", m_fq_name, , , msg, __VA_ARGS__)

HomeLogStore::HomeLogStore(std::shared_ptr< LogDev > logdev, logstore_id_t id, bool append_mode,
                           logstore_seq_num_t start_lsn) :
        m_store_id{id},
        m_logdev{logdev},
        m_records{"HomeLogStoreRecords", start_lsn - 1},
        m_append_mode{append_mode},
        m_start_lsn{start_lsn},
        m_next_lsn{start_lsn},
        m_tail_lsn{start_lsn - 1},
        m_fq_name{fmt::format("{} log_dev={}", id, logdev->get_id())},
        m_metrics{logstore_service().metrics()} {}

void HomeLogStore::write_async(logstore_req* req, const log_req_comp_cb_t& cb) {
    HS_LOG_ASSERT((cb || m_comp_cb), "Expected either cb is not null or default cb registered");
    req->cb = (cb ? cb : m_comp_cb);
    req->start_time = Clock::now();

#ifndef NDEBUG
    if (req->seq_num < start_lsn()) {
        THIS_LOGSTORE_LOG(ERROR, "Assert: Writing lsn={} lesser than start_lsn={}", req->seq_num, start_lsn());
        HS_DBG_ASSERT(0, "Assertion");
    }
#endif

    m_records.create(req->seq_num);
    COUNTER_INCREMENT(m_metrics, logstore_append_count, 1);
    HISTOGRAM_OBSERVE(m_metrics, logstore_record_size, req->data.size());
    m_logdev->append_async(m_store_id, req->seq_num, req->data, static_cast< void* >(req), req->flush_wait);
}

void HomeLogStore::write_async(logstore_seq_num_t seq_num, const sisl::io_blob& b, void* cookie,
                               const log_write_comp_cb_t& cb) {
    // Form an internal request and issue the write
    auto* req = logstore_req::make(this, seq_num, b, true /* is_write_req */);
    req->cookie = cookie;

    write_async(req, [cb](logstore_req* req, logdev_key written_lkey) {
        if (cb) { cb(req->seq_num, req->data, written_lkey, req->cookie); }
        logstore_req::free(req);
    });
}

logstore_seq_num_t HomeLogStore::append_async(const sisl::io_blob& b, void* cookie, const log_write_comp_cb_t& cb) {
    HS_DBG_ASSERT_EQ(m_append_mode, true, "append_async can be called only on append only mode");
    const auto seq_num = m_next_lsn.fetch_add(1, std::memory_order_acq_rel);
    write_async(seq_num, b, cookie, cb);
    return seq_num;
}

bool HomeLogStore::write_and_flush(logstore_seq_num_t seq_num, const sisl::io_blob& b) {
    HS_LOG_ASSERT(iomanager.can_run_blocking_io(),
                  "Write and flush is a blocking IO, which can't run in this thread, please reschedule to a fiber");

    this->write_async(seq_num, b, nullptr /* cookie */, nullptr /* cb */);
    m_logdev->flush();
}

log_buffer HomeLogStore::read_sync(logstore_seq_num_t seq_num) {
    HS_LOG_ASSERT(iomanager.can_run_blocking_io(),
                  "Read sync is a blocking IO, which can't run in this thread, reschedule to a fiber");

    // If seq_num has not been flushed yet, but issued, then we flush them before reading
    auto const s = m_records.status(seq_num);
    if (s.is_out_of_range || s.is_hole) {
        // THIS_LOGSTORE_LOG(ERROR, "ld_key not valid {}", seq_num);
        throw std::out_of_range("key not valid");
    } else if (!s.is_completed) {
        THIS_LOGSTORE_LOG(TRACE, "Reading lsn={}:{} before flushed, doing flush first", m_store_id, seq_num);
        m_logdev->flush();
    }

    const auto record = m_records.at(seq_num);
    const logdev_key ld_key = record.m_dev_key;
    if (!ld_key.is_valid()) {
        THIS_LOGSTORE_LOG(ERROR, "ld_key not valid {}", seq_num);
        throw std::out_of_range("key not valid");
    }

    const auto start_time = Clock::now();
    // THIS_LOGSTORE_LOG(TRACE, "Reading lsn={}:{} mapped to logdev_key=[idx={} dev_offset={}]", m_store_id, seq_num,
    //                   ld_key.idx, ld_key.dev_offset);
    COUNTER_INCREMENT(m_metrics, logstore_read_count, 1);
    serialized_log_record header;
    const auto b = m_logdev->read(ld_key, header);
    HISTOGRAM_OBSERVE(m_metrics, logstore_read_latency, get_elapsed_time_us(start_time));
    return b;
}
#if 0
void HomeLogStore::read_async(logstore_req* req, const log_found_cb_t& cb) {
    HS_LOG_ASSERT( ((cb != nullptr) || (m_comp_cb != nullptr)),
              "Expected either cb is not null or default cb registered");
    auto record = m_records.at(req->seq_num);
    logdev_key ld_key = record.m_dev_key;
    req->cb = cb;
    m_logdev->read_async(ld_key, (void*)req);
}

void HomeLogStore::read_async(logstore_seq_num_t seq_num, void* cookie, const log_found_cb_t& cb) {
    auto record = m_records.at(seq_num);
    logdev_key ld_key = record.m_dev_key;
    sisl::io_blob b;
    auto* req = logstore_req::make(this, seq_num, &b, false /* not write */);
    read_async(req, [cookie, cb](logstore_seq_num_t seq_num, log_buffer log_buf, void* cookie) {
            cb(seq, log_buf, cookie);
            logstore_req::free(req);
            });
}
#endif

void HomeLogStore::on_write_completion(logstore_req* req, const logdev_key& ld_key) {
    // Logstore supports out-of-order lsn writes, in that case we need to mark the truncation key for this lsn as the
    // one which is being written by the higher lsn. This is to ensure that we don't truncate higher lsn's logdev_key
    // when we truncate the lower lsns.
    //
    // TODO: In case of out-of-order lsns, it needs to read the records of the tail_lsn and get their truncation key.
    // This involves a read lock and an atomic operation. We can optimize this in case if the ld_key is updated for the
    // same batch.
    logdev_key trunc_key;
    if (m_tail_lsn < req->seq_num) {
        m_tail_lsn = req->seq_num;
        trunc_key = ld_key;
    } else {
        trunc_key = m_records.at(m_tail_lsn).m_trunc_key;
    }

    // Upon completion, create the mapping between seq_num and log dev key
    m_records.update(req->seq_num, [&ld_key, &trunc_key](logstore_record& rec) -> bool {
        rec.m_dev_key = ld_key;
        rec.m_trunc_key = trunc_key;
        return true;
    });

    THIS_LOGSTORE_LOG(TRACE, "Completed write of lsn={} logdev_key={} tail_lsn={} trunc_key={}", req->seq_num, ld_key,
                      m_tail_lsn, trunc_key);
    HISTOGRAM_OBSERVE(m_metrics, logstore_append_latency, get_elapsed_time_us(req->start_time));
    (req->cb) ? req->cb(req, ld_key) : m_comp_cb(req, ld_key);
}

void HomeLogStore::on_read_completion(logstore_req* req, const logdev_key& ld_key) {
    (req->cb) ? req->cb(req, ld_key) : m_comp_cb(req, ld_key);
}

void HomeLogStore::on_log_found(logstore_seq_num_t seq_num, const logdev_key& ld_key, const logdev_key& flush_ld_key,
                                log_buffer buf) {
    THIS_LOGSTORE_LOG(DEBUG, "Found a log lsn={} logdev_key={} tail_lsn={}", seq_num, ld_key, m_tail_lsn);

    logdev_key trunc_key;
    if (m_tail_lsn < seq_num) {
        m_tail_lsn = seq_num;
        trunc_key = flush_ld_key;
    } else {
        trunc_key = m_records.at(m_tail_lsn).m_trunc_key;
    }

    // Create the mapping between seq_num and log dev key
    m_records.create_and_complete(seq_num, ld_key, trunc_key);
    atomic_update_max(m_next_lsn, seq_num + 1, std::memory_order_acq_rel);

    if (m_found_cb != nullptr) { m_found_cb(seq_num, buf, nullptr); }
}

void HomeLogStore::truncate(logstore_seq_num_t upto_lsn, bool in_memory_truncate_only) {
#ifndef NDEBUG
    auto cs = get_contiguous_completed_seq_num(0);
    if (upto_lsn > cs) {
        THIS_LOGSTORE_LOG(WARN,
                          "Truncation issued on seq_num={} outside of contiguous completions={}, "
                          "still proceeding to truncate",
                          upto_lsn, cs);
    }

#endif

    if (upto_lsn > m_tail_lsn) {
        THIS_LOGSTORE_LOG(WARN,
                          "Truncation issued on lsn={} which is greater than tail_lsn={}, truncating upto tail_lsn",
                          upto_lsn, m_tail_lsn);
        m_trunc_ld_key = m_records.at(m_tail_lsn).m_trunc_key;
    } else {
        m_trunc_ld_key = m_records.at(upto_lsn).m_trunc_key;
    }
    m_records.truncate(upto_lsn);
    m_start_lsn.store(upto_lsn + 1);
    if (!in_memory_truncate_only) { m_logdev->truncate(); }
}

std::tuple< logstore_seq_num, logdev_key, logstore_seq_num > HomeLogStore::truncate_info() const {
    auto const trunc_lsn = m_start_lsn.load(std::memory_order_relaxed) - 1;
    return std::make_tuple(trunc_lsn, m_trunc_ld_key, m_tail_lsn);
}

void HomeLogStore::fill_gap(logstore_seq_num_t seq_num) {
    HS_DBG_ASSERT_EQ(m_records.status(seq_num).is_hole, true, "Attempted to fill gap lsn={} which has valid data",
                     seq_num);

    logdev_key empty_ld_key;
    m_records.create_and_complete(seq_num, empty_ld_key, empty_ld_key);
}

nlohmann::json HomeLogStore::dump_log_store(const log_dump_req& dump_req) {
    nlohmann::json json_dump{}; // create root object
    json_dump["store_id"] = this->m_store_id;

    int64_t start_idx = std::max(dump_req.start_seq_num, start_lsn());

    // must use move operator= operation instead of move copy constructor
    nlohmann::json json_records = nlohmann::json::array();
    m_records.foreach_all_completed(
        start_idx, [this, &dump_req, &json_records](int64_t, homestore::logstore_record const& rec) -> bool {
            nlohmann::json json_val = nlohmann::json::object();
            serialized_log_record record_header;

            const auto log_buffer = m_logdev->read(rec.m_dev_key, record_header);
            try {
                json_val["size"] = uint32_cast(record_header.size);
                json_val["offset"] = uint32_cast(record_header.offset);
                json_val["is_inlined"] = uint32_cast(record_header.get_inlined());
                json_val["lsn"] = uint64_cast(record_header.store_seq_num);
                json_val["store_id"] = s_cast< logstore_id_t >(record_header.store_id);
            } catch (const std::exception& ex) { THIS_LOGSTORE_LOG(ERROR, "Exception in json dump- {}", ex.what()); }

            if (dump_req.verbosity_level == homestore::log_dump_verbosity::CONTENT) {
                const uint8_t* b = log_buffer.bytes();
                const std::vector< uint8_t > bv(b, b + log_buffer.size());
                auto content = nlohmann::json::binary_t(bv);
                json_val["content"] = std::move(content);
            }
            json_records.emplace_back(std::move(json_val));
            return true;
        });

    json_dump["log_records"] = std::move(json_records);
    return json_dump;
}

void HomeLogStore::foreach (int64_t start_idx, const std::function< bool(logstore_seq_num_t, log_buffer) >& cb) {
    m_records.foreach_all_completed(start_idx, [&](int64_t cur_idx, homestore::logstore_record& record) -> bool {
        // do a sync read
        serialized_log_record header;
        auto log_buf = m_logdev->read(record.m_dev_key, header);
        return cb(cur_idx, log_buf);
    });
}

logstore_seq_num_t HomeLogStore::get_contiguous_issued_seq_num(logstore_seq_num_t from) const {
    return (logstore_seq_num_t)m_records.active_upto(from + 1);
}

logstore_seq_num_t HomeLogStore::get_contiguous_completed_seq_num(logstore_seq_num_t from) const {
    return (logstore_seq_num_t)m_records.completed_upto(from + 1);
}

void HomeLogStore::flush(logstore_seq_num_t upto_lsn) {
    if (!m_logdev->allow_explicit_flush()) {
        HS_LOG_ASSERT(false, WARN,
                      "Explicit flush is turned off or calling flush on wrong thread for this logdev, ignoring flush");
        return;
    }

    if (upto_lsn == invalid_lsn()) { upto_lsn = m_records.active_upto(); }

    // if we have flushed already, we are done, else issue a flush
    if (!m_records.status(upto_lsn).is_completed) { m_logdev->flush(); }
}

bool HomeLogStore::rollback(logstore_seq_num_t to_lsn) {
    // Validate if the lsn to which it is rolledback to is not truncated.
    auto ret = m_records.status(to_lsn + 1);
    if (ret.is_out_of_range) {
        HS_LOG_ASSERT(false, "Attempted to rollback to {} which is already truncated", to_lsn);
        return false;
    }

    bool do_flush{false};
    do {
        {
            auto lg = m_logdev->flush_guard();
            if (m_tail_lsn + 1 < m_next_lsn.load()) {
                // We should flush any outstanding writes before we proceed with rollback
                THIS_LOGSTORE_LOG(INFO,
                                  "Rollback is issued while while there are some oustanding writes, tail_lsn={}, "
                                  "next_lsn={}, will flush and retry rollback",
                                  m_tail_lsn, m_next_lsn.load());
                do_flush = true;
            } else {
                do_flush = false;

                logid_range_t logid_range =
                    std::make_pair(m_records.at(to_lsn + 1).m_dev_key.idx,
                                   m_records.at(m_tail_lsn).m_dev_key.idx); // Get the logid range to rollback

                // Update the next_lsn and tail lsn back to to_lsn and also rollback all stream records and now on, we
                // can't access any lsns beyond to_lsn
                m_next_lsn.store(to_lsn + 1, std::memory_order_release); // Rollback the next append lsn
                m_tail_lsn = to_lsn;
                m_records.rollback(to_lsn);

                // Rollback the log_ids in the range, for this log store (which persists this info in its superblk)
                m_logdev->rollback(m_store_id, logid_range);
            }
        }
        if (do_flush) { m_logdev->flush(); }
    } while (do_flush);

    return true;
}

nlohmann::json HomeLogStore::get_status(int verbosity) const {
    nlohmann::json js;
    js["append_mode"] = m_append_mode;
    js["start_lsn"] = start_lsn();
    js["next_lsn"] = m_next_lsn.load(std::memory_order_relaxed);
    js["tail_lsn"] = m_tail_lsn;
    js["logstore_records"] = m_records.get_status(verbosity);
    js["logstore_sb_first_lsn"] = m_logdev->log_dev_meta().store_superblk(m_store_id).m_first_seq_num;
    return js;
}

logstore_superblk logstore_superblk::default_value() { return logstore_superblk{-1}; }
void logstore_superblk::init(logstore_superblk& meta) { meta.m_first_seq_num = 0; }
void logstore_superblk::clear(logstore_superblk& meta) { meta.m_first_seq_num = -1; }
bool logstore_superblk::is_valid(const logstore_superblk& meta) { return meta.m_first_seq_num >= 0; }

} // namespace homestore
