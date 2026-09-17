#include "homedb/db_table/db.h"

#include <fmt/format.h>

#include "homestore/base/homestore_assert.h"
#include "homestore/base/homestore_decl.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"
#include "homestore/device/virtual_dev.h"
#include "homestore/homestore.h"
#include "homestore/index/btree/btree.h"
#include "homestore/index/cow_btree/cow_btree.h"
#include "homestore/index/cow_btree/cow_btree_mgr.h"
#include "homestore/index/cow_btree/cow_btree_mgr.ipp"
#include "homestore/index/btree/detail/btree_node.h"
#include "homestore/logstore/log_blob.h"
#include "homestore/managers.h"

#include "homedb/db_table/catalog.h"
#include "homedb/index/db_kv.h"
#include "homedb/index/unsharded_btree.h"
#include "homedb/wal/journal.h"
#include "homedb/wal/record.h"

namespace homedb {

// ── Boot-time defaults ───────────────────────────────────────────────────────────────────────────────────────────

static constexpr uint64_t kBlobDevInitialChunkSize = 32ull * 1024 * 1024;
static constexpr uint32_t kBlkSize = 4096;
static constexpr uint32_t kNodeSize = 4096;

static homestore::BtreeConfig make_btree_config(std::string const& name) {
    homestore::BtreeConfig cfg{};
    cfg.btree_name_ = name;
    cfg.node_size_ = kNodeSize;
    cfg.leaf_node_type_ = homestore::BtreeNodeType::VAR_OBJECT;
    cfg.int_node_type_ = homestore::BtreeNodeType::VAR_KEY;
    cfg.finalize(sizeof(homestore::NodeCore::PersistentHeader));
    return cfg;
}

static homestore::VDevParameters make_vdev_params() {
    homestore::VDevParameters p{};
    p.initial_chunk_size = kBlobDevInitialChunkSize;
    p.blk_size = kBlkSize;
    p.dev_type = homestore::HSDevType::Data;
    p.alloc_type = homestore::BlkAllocatorType::SlabCompact;
    p.chunk_sel_type = homestore::ChunkSelectorType::RoundRobin;
    return p;
}

// ── RecordCursor ─────────────────────────────────────────────────────────────────────────────────────────────────
// Walks a scatter-gather LogBlob field-by-field.  Callers know the layout (Database owns the format) so each
// take(n) matches a single field boundary — no cross-part reads.  Live records arrive with each field in its
// own part (record_hdr, entry_hdr, key, value); replay records collapse into a single part with everything
// concatenated (all take() calls slice into the same underlying buffer).

namespace {
class RecordCursor {
public:
    explicit RecordCursor(homestore::LogBlob const& parts) : parts_{parts} {}

    sisl::Blob take(uint32_t n) {
        while (part_idx_ < parts_.n_parts && part_offset_ >= parts_.parts[part_idx_].size()) {
            ++part_idx_;
            part_offset_ = 0;
        }
        HS_REL_ASSERT_LT(part_idx_, parts_.n_parts, "RecordCursor::take: ran out of parts");
        auto const& p = parts_.parts[part_idx_];
        HS_REL_ASSERT_LE(part_offset_ + n, p.size(),
                         "RecordCursor::take: field crosses part boundary at part[{}]", part_idx_);
        sisl::Blob view{p.cbytes() + part_offset_, n};
        part_offset_ += n;
        return view;
    }

    template < typename T >
    T const* peek_struct() {
        auto v = take(to_u32(sizeof(T)));
        return r_cast< T const* >(v.cbytes());
    }

private:
    homestore::LogBlob const& parts_;
    uint8_t part_idx_{0};
    uint32_t part_offset_{0};
};
} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────────────────────────────────────────

Async< shared< Database > > Database::open(JournalConfig const& config) {
    // Fresh boot → new journal, no persisted tables to reopen.
    if (homestore::hs()->is_first_time_boot()) {
        auto journal = co_await Journal::create(config);
        auto db = shared< Database >{new Database{std::move(journal)}};
        db->journal_->set_sink(db.get());
        co_return db;
    }

    // Recovery.
    auto const sbs = homestore::cow_btree_mgr().list_persisted_btrees();
    if (sbs.empty()) {
        // Loaded HomeStore with nothing persisted; treat as fresh Database.  replay is a no-op (empty log)
        // but call it to take HomeStore live.
        auto journal = co_await Journal::create(config);
        auto db = shared< Database >{new Database{std::move(journal)}};
        db->journal_->set_sink(db.get());
        co_await homestore::hs()->replay();
        co_return db;
    }

    // Every Table's CatalogEntry carries the same journal_id — take it from the first one.
    auto const* first_entry = r_cast< CatalogEntry const* >(sbs[0]->user_sb_data());
    HS_REL_ASSERT_EQ(first_entry->version, CatalogEntry::kVersion,
                     "Database::open: CatalogEntry version mismatch on first table");
    auto journal = co_await Journal::load(config, first_entry->journal_id);
    auto db = shared< Database >{new Database{std::move(journal)}};
    db->journal_->set_sink(db.get());

    for (auto const* sb : sbs) {
        auto const* entry = r_cast< CatalogEntry const* >(sb->user_sb_data());
        HS_REL_ASSERT_EQ(entry->version, CatalogEntry::kVersion,
                         "Database::open: CatalogEntry version mismatch on table id {}", entry->table_id);
        auto const name = entry->get_name();
        auto const spec = entry->spec.to_spec();

        auto blob_dev = homestore::blob_dev_mgr().get_blob_dev(name);
        HS_REL_ASSERT(blob_dev, "Database::open: BlobDev '{}' not found for table id {}", name, entry->table_id);

        auto cfg = make_btree_config(name);
        auto btree = co_await homestore::cow_btree_mgr().load_cow_btree< DbKey, DbValue >(cfg, blob_dev, *sb);
        auto index = unique< UnshardedBtree< DbKey, DbValue > >{new UnshardedBtree< DbKey, DbValue >{std::move(btree)}};
        auto table = shared< Table >{new Table{entry->table_id, name, spec, std::move(index)}};

        {
            std::unique_lock lk{db->tables_mtx_};
            db->tables_by_name_[name] = table;
            db->tables_by_id_[entry->table_id] = table.get();
            db->next_table_id_ = std::max(db->next_table_id_, s_cast< uint16_t >(entry->table_id + 1));
        }
    }

    // Every Table registered — drive WAL replay so any un-CP'd records fold back through Journal::on_replay →
    // Database::on_commit → Table::commit → persistent index.
    co_await homestore::hs()->replay();

    co_return db;
}

// ── Instance ─────────────────────────────────────────────────────────────────────────────────────────────────────

Database::Database(shared< Journal > journal) : journal_{std::move(journal)} {}

Database::~Database() {
    std::unique_lock lk{tables_mtx_};
    tables_by_id_.clear();
    tables_by_name_.clear();
}

Async< Result< shared< Table > > > Database::create_table(std::string const& name, TableSpec const& spec) {
    if (spec.partition_key_size != 0) {
        co_return folly::makeUnexpected(HomeDbError{
            ErrorKind::NotYetImplemented, "sharded btree (partition_key_size > 0) is not supported yet"});
    }
    if (spec.mvcc_enabled) {
        co_return folly::makeUnexpected(HomeDbError{ErrorKind::NotYetImplemented, "mvcc is not supported yet"});
    }

    uint16_t const table_id = [this]() {
        std::unique_lock lk{tables_mtx_};
        return next_table_id_++;
    }();

    if (get_table(name)) {
        co_return folly::makeUnexpected(
            HomeDbError{ErrorKind::AlreadyExists, fmt::format("table '{}' already exists", name)});
    }

    auto blob_dev = co_await homestore::blob_dev_mgr().create_blob_dev(std::string{name}, make_vdev_params());
    HS_REL_ASSERT(blob_dev, "Database::create_table: create_blob_dev '{}' returned null", name);

    CatalogEntry entry{};
    entry.table_id = table_id;
    entry.journal_id = journal_->journal_id();
    entry.set_name(name);
    entry.spec = TableSpecOnDisk::from(spec);
    sisl::Blob user_sb{r_cast< uint8_t const* >(&entry), to_u32(sizeof(entry))};

    auto cfg = make_btree_config(name);
    auto btree = co_await homestore::cow_btree_mgr().create_cow_btree< DbKey, DbValue >(cfg, blob_dev, user_sb);
    auto index = unique< UnshardedBtree< DbKey, DbValue > >{new UnshardedBtree< DbKey, DbValue >{std::move(btree)}};
    auto table = shared< Table >{new Table{table_id, name, spec, std::move(index)}};

    {
        std::unique_lock lk{tables_mtx_};
        tables_by_name_[name] = table;
        tables_by_id_[table_id] = table.get();
    }
    co_return table;
}

shared< Table > Database::get_table(std::string const& name) const {
    std::shared_lock lk{tables_mtx_};
    auto it = tables_by_name_.find(name);
    return (it == tables_by_name_.end()) ? shared< Table >{} : it->second;
}

std::vector< shared< Table > > Database::tables() const {
    std::vector< shared< Table > > out;
    std::shared_lock lk{tables_mtx_};
    out.reserve(tables_by_name_.size());
    for (auto const& [_, t] : tables_by_name_) {
        out.push_back(t);
    }
    return out;
}

Table* Database::table_by_id(uint16_t table_id) const {
    std::shared_lock lk{tables_mtx_};
    auto it = tables_by_id_.find(table_id);
    return (it == tables_by_id_.end()) ? nullptr : it->second;
}

// ── Write surface ────────────────────────────────────────────────────────────────────────────────────────────────

Async< Result< uint64_t > > Database::put(std::string const& table, sisl::Blob const& key, sisl::Blob const& value) {
    auto t = get_table(table);
    if (!t) {
        co_return folly::makeUnexpected(
            HomeDbError{ErrorKind::NotFound, fmt::format("table '{}' not found", table)});
    }
    if (auto r = t->spec().key_spec.validate_bytes(key.size()); !r) {
        co_return folly::makeUnexpected(r.error());
    }
    if (auto r = t->spec().value_spec.validate_bytes(value.size()); !r) {
        co_return folly::makeUnexpected(r.error());
    }
    co_return co_await write_single_entry(t->table_id(), OpType::Put, key, value);
}

Async< Result< uint64_t > > Database::remove(std::string const& table, sisl::Blob const& key) {
    auto t = get_table(table);
    if (!t) {
        co_return folly::makeUnexpected(
            HomeDbError{ErrorKind::NotFound, fmt::format("table '{}' not found", table)});
    }
    if (auto r = t->spec().key_spec.validate_bytes(key.size()); !r) {
        co_return folly::makeUnexpected(r.error());
    }
    co_return co_await write_single_entry(t->table_id(), OpType::Remove, key, sisl::Blob{});
}

Async< Result< sisl::IoBufShared > > Database::get(std::string const& table, sisl::Blob const& key) {
    auto t = get_table(table);
    if (!t) {
        co_return folly::makeUnexpected(
            HomeDbError{ErrorKind::NotFound, fmt::format("table '{}' not found", table)});
    }
    co_return co_await t->get(key);
}

Async< Result< uint64_t > > Database::write_single_entry(uint16_t table_id, OpType op, sisl::Blob const& key,
                                                         sisl::Blob const& value) {
    // Headers live on the coroutine frame — kept alive across the co_await below so the LogBlob views into
    // them stay valid until the journal / sink chain resolves.
    TxnRecordHeader rec_hdr{};
    rec_hdr.entry_count = 1;

    TxnEntryHeader entry_hdr{};
    entry_hdr.op_type = to_u8(op);
    entry_hdr.table_id = table_id;
    entry_hdr.key_size = to_u32(key.size());
    entry_hdr.value_size = to_u32(value.size());

    homestore::LogBlob record;
    record.append(sisl::IoBufSpan{r_cast< uint8_t const* >(&rec_hdr), to_u32(sizeof(rec_hdr)), false});
    record.append(sisl::IoBufSpan{r_cast< uint8_t const* >(&entry_hdr), to_u32(sizeof(entry_hdr)), false});
    if (key.size() > 0) {
        record.append(sisl::IoBufSpan{key.cbytes(), key.size(), false});
    }
    if (value.size() > 0) {
        record.append(sisl::IoBufSpan{value.cbytes(), value.size(), false});
    }
    co_return co_await journal_->write(record);
}

// ── CommitSink ───────────────────────────────────────────────────────────────────────────────────────────────────

Async< void > Database::on_commit(uint64_t lsn, homestore::LogBlob const& record_parts) {
    RecordCursor cur{record_parts};
    auto const* rec_hdr = cur.peek_struct< TxnRecordHeader >();
    HS_REL_ASSERT_EQ(rec_hdr->version, TxnRecordHeader::kVersion,
                     "Database::on_commit: TxnRecordHeader version mismatch (lsn={})", lsn);
    for (uint16_t i = 0; i < rec_hdr->entry_count; ++i) {
        auto const* entry_hdr = cur.peek_struct< TxnEntryHeader >();
        sisl::Blob const key = cur.take(entry_hdr->key_size);
        sisl::Blob const value = (entry_hdr->value_size > 0) ? cur.take(entry_hdr->value_size) : sisl::Blob{};

        auto* target = table_by_id(entry_hdr->table_id);
        HS_REL_ASSERT(target, "Database::on_commit: no table registered for id {} (lsn={})", entry_hdr->table_id,
                      lsn);
        auto commit_result = co_await target->commit(lsn, s_cast< OpType >(entry_hdr->op_type), key, value);
        HS_REL_ASSERT(commit_result.hasValue(), "Database::on_commit: table {} commit failed: {}",
                      entry_hdr->table_id, commit_result.hasValue() ? "" : commit_result.error().to_string());
    }
    co_return;
}

} // namespace homedb
