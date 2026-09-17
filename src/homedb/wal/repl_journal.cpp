#include "homedb/wal/repl_journal.h"

#include <stdexcept>

namespace homedb {

static void not_impl() { throw std::runtime_error{"ReplJournal is not yet implemented"}; }

Async< shared< ReplJournal > > ReplJournal::create(JournalConfig const& /*config*/) {
    not_impl();
    co_return shared< ReplJournal >{};
}

Async< shared< ReplJournal > > ReplJournal::load(JournalConfig const& /*config*/, uint32_t /*journal_id*/) {
    not_impl();
    co_return shared< ReplJournal >{};
}

void ReplJournal::set_sink(CommitSink* /*sink*/) { not_impl(); }

Async< Result< uint64_t > > ReplJournal::write(homestore::LogBlob const& /*record_parts*/) {
    not_impl();
    co_return folly::makeUnexpected(HomeDbError{});
}

uint64_t ReplJournal::last_committed_lsn() const {
    not_impl();
    return 0;
}

uint32_t ReplJournal::journal_id() const {
    not_impl();
    return 0;
}

Async< void > ReplJournal::advance_truncate_upto(uint64_t /*lsn*/) {
    not_impl();
    co_return;
}

} // namespace homedb
