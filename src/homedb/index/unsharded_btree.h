#pragma once

#include <optional>
#include <utility>

#include <folly/Expected.h>

#include "common/async.h"
#include "common/defs.h"
#include "homestore/index/btree/btree.h"

namespace homedb {

// ── UnshardedBtree<K,V> ──────────────────────────────────────────────────────────────────────────────────────────
// Phase-1 concrete index backend: one homestore::Btree<K,V> on top of a caller-provided shared<Btree>.  The
// COWBtree beneath is created/loaded by HomeDB via cow_btree_mgr().create_cow_btree<K,V>(...) — HomeDB owns
// the lifecycle; this wrapper only exposes the ops surface.  When Sharded arrives in v2 an IBtreeIndex<K,V>
// interface will be introduced and both wrappers will implement it; Phase-1 skips the interface (single impl,
// virtual dispatch adds no value yet).

template < typename K, typename V >
class UnshardedBtree {
public:
    explicit UnshardedBtree(shared< homestore::Btree< K, V > > btree) : btree_{std::move(btree)} {}

    UnshardedBtree(UnshardedBtree const&) = delete;
    UnshardedBtree& operator=(UnshardedBtree const&) = delete;
    UnshardedBtree(UnshardedBtree&&) noexcept = default;
    UnshardedBtree& operator=(UnshardedBtree&&) noexcept = default;
    ~UnshardedBtree() = default;

    shared< homestore::Btree< K, V > > const& btree() const { return btree_; }

    // Wrapping thin coroutines that forward to homestore::Btree.  Return the raw BtreeResult so callers get
    // the concrete BtreeStatus for their own error mapping.
    Async< folly::Expected< homestore::PutStats, homestore::BtreeStatus > >
    put(K const& key, V const& value, homestore::BtreePutType put_type = homestore::BtreePutType::UPSERT) {
        co_return co_await btree_->put_one(key, value, put_type, /*existing_val=*/nullptr, /*filter=*/nullptr);
    }

    Async< folly::Expected< V, homestore::BtreeStatus > > get(K const& key) {
        co_return co_await btree_->get_one(key);
    }

    Async< folly::Expected< V, homestore::BtreeStatus > > remove(K const& key) {
        co_return co_await btree_->remove_one(key, /*filter=*/nullptr);
    }

private:
    shared< homestore::Btree< K, V > > btree_;
};

} // namespace homedb