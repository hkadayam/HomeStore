#include "homedb/wal/local_journal.h"

#include "homestore/base/homestore_assert.h"
#include "homestore/logstore/log_store_mgr.h"
#include "homestore/managers.h"

namespace homedb {

static homestore::LogStoreOptions default_opts() {
    return homestore::LogStoreOptions{.append_mode = true, .auto_truncate = false};
}

Async< shared< LocalJournal > > LocalJournal::create() {
    auto log_store = co_await homestore::log_store_mgr().create_log_store(default_opts());
    HS_REL_ASSERT(log_store, "LocalJournal::create: create_log_store returned null");
    auto j = shared< LocalJournal >{new LocalJournal{log_store}};
    homestore::log_store_mgr().open_log_store(
        log_store->store_id(), default_opts(),
        [w = std::weak_ptr< LocalJournal >{j}](homestore::lsn_t lsn,
                                               sisl::IoBufView const& data) -> Async< void > {
            if (auto self = w.lock()) {
                co_await self->on_replay(lsn, data);
            }
            co_return;
        });
    co_return j;
}

shared< LocalJournal > LocalJournal::load(homestore::logstore_id_t store_id) {
    auto log_store = homestore::log_store_mgr().get_log_store(store_id);
    HS_REL_ASSERT(log_store, "LocalJournal::load: log_store id {} not found", store_id);
    auto j = shared< LocalJournal >{new LocalJournal{log_store}};
    homestore::log_store_mgr().open_log_store(
        store_id, default_opts(),
        [w = std::weak_ptr< LocalJournal >{j}](homestore::lsn_t lsn,
                                               sisl::IoBufView const& data) -> Async< void > {
            if (auto self = w.lock()) {
                co_await self->on_replay(lsn, data);
            }
            co_return;
        });
    return j;
}

LocalJournal::LocalJournal(shared< homestore::LogStore > log_store) : log_store_{std::move(log_store)} {}

void LocalJournal::set_sink(CommitSink* sink) { sink_.store(sink, std::memory_order_release); }

uint32_t LocalJournal::journal_id() const { return s_cast< uint32_t >(log_store_->store_id()); }

uint64_t LocalJournal::last_committed_lsn() const { return to_u64(log_store_->flushed_upto()); }

Async< void > LocalJournal::advance_truncate_upto(uint64_t lsn) {
    co_await log_store_->truncate(to_i64(lsn));
    co_return;
}

Async< Result< uint64_t > > LocalJournal::write(homestore::LogBlob const& record_parts) {
    homestore::lsn_t const lsn = co_await log_store_->append_and_flush(record_parts);
    if (auto* sink = sink_.load(std::memory_order_acquire); sink != nullptr) {
        co_await sink->on_commit(to_u64(lsn), record_parts);
    }
    co_return Result< uint64_t >{to_u64(lsn)};
}

Async< void > LocalJournal::on_replay(homestore::lsn_t lsn, sisl::IoBufView const& data) {
    auto* sink = sink_.load(std::memory_order_acquire);
    if (sink == nullptr) {
        co_return;
    }
    homestore::LogBlob one_part;
    one_part.append(sisl::IoBufSpan{data.cbytes(), data.size(), /*is_aligned=*/false});
    co_await sink->on_commit(to_u64(lsn), one_part);
    co_return;
}

} // namespace homedb
