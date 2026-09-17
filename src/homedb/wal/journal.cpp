#include "homedb/wal/journal.h"

#include "homedb/wal/local_journal.h"
#include "homedb/wal/repl_journal.h"

namespace homedb {

Async< shared< Journal > > Journal::create(JournalConfig const& config) {
    if (config.replicated) {
        co_return co_await ReplJournal::create(config);
    }
    co_return co_await LocalJournal::create();
}

Async< shared< Journal > > Journal::load(JournalConfig const& config, uint32_t journal_id) {
    if (config.replicated) {
        co_return co_await ReplJournal::load(config, journal_id);
    }
    co_return LocalJournal::load(s_cast< homestore::logstore_id_t >(journal_id));
}

} // namespace homedb
