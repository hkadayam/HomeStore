#include "homedb/db_table/table.h"

#include <fmt/format.h>

namespace homedb {

static HomeDbError btree_error(homestore::BtreeStatus s, char const* op) {
    return HomeDbError{ErrorKind::Io, fmt::format("btree {} failed: status={}", op, enum_name(s))};
}

Table::Table(uint16_t table_id, std::string name, TableSpec spec,
             unique< UnshardedBtree< DbKey, DbValue > > index) :
        table_id_{table_id},
        name_{std::move(name)},
        spec_{std::move(spec)},
        index_{std::move(index)} {}

Async< Result< void > > Table::commit(uint64_t /*lsn*/, OpType op, sisl::Blob const& key, sisl::Blob const& value) {
    DbKey const dbk{key, spec_.key_spec.fixed_size()};
    if (op == OpType::Put) {
        DbValue const dbv{value, spec_.value_spec.fixed_size()};
        auto result = co_await index_->put(dbk, dbv);
        if (!result) {
            co_return folly::makeUnexpected(btree_error(result.error(), "put_one"));
        }
    } else {
        auto result = co_await index_->remove(dbk);
        if (!result && result.error() != homestore::BtreeStatus::key_not_found) {
            co_return folly::makeUnexpected(btree_error(result.error(), "remove_one"));
        }
    }
    co_return Result< void >{};
}

Async< Result< sisl::IoBufShared > > Table::get(sisl::Blob const& key) {
    if (auto r = spec_.key_spec.validate_bytes(key.size()); !r) {
        co_return folly::makeUnexpected(r.error());
    }
    DbKey const dbk{key, spec_.key_spec.fixed_size()};
    auto result = co_await index_->get(dbk);
    if (!result) {
        if (result.error() == homestore::BtreeStatus::key_not_found) {
            co_return sisl::IoBufShared{};
        }
        co_return folly::makeUnexpected(btree_error(result.error(), "get_one"));
    }
    co_return result.value().buf();
}

} // namespace homedb
