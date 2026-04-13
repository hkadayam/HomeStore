#include <folly/coro/Collect.h>

#include "index/cow_btree/cow_btree_mgr.h"
#include "index/cow_btree/cow_btree.h"
#include "blob/blob_dev.h"
#include "blob/blob_dev_mgr.h"
#include "managers.h"

namespace homestore {

// ──────────────────────────────────────────────── Lifecycle ──────────────────────────────────────────────────────────

folly::coro::Task< void > COWBtreeManager::create() {
    auto mgr = shared< COWBtreeManager >(new COWBtreeManager());
    Managers::init_cow_btree_mgr(std::move(mgr));
    co_return;
}

folly::coro::Task< void > COWBtreeManager::load() {
    auto mgr = shared< COWBtreeManager >(new COWBtreeManager());
    // TODO: read all persisted COWBtree metablks from the MetaClient and stash in pending_btrees_.

    Managers::init_cow_btree_mgr(std::move(mgr));
    co_return;
}

void COWBtreeManager::shutdown() {
    {
        std::lock_guard lk(tracking_mtx_);
        tracked_btrees_.clear();
    }
    pending_btrees_.clear();
}

// ────────────────────────────────────────────── Implementation ──────────────────────────────────────────────────────

COWBtreeManager::COWBtreeManager() : cp_callbacks_{std::make_unique< CPCallbacksImpl >(*this)} {
    // Single evictor shared by both caches — total cache budget is split across node and overflow data.
    sisl::TwoQEvictor::Config ev_cfg;
    ev_cfg.max_size = HS_DYNAMIC_CONFIG(btree->cache_size);
    evictor_ = std::make_shared< sisl::TwoQEvictor >(ev_cfg);

    node_cache_ = std::make_shared< NodeCache >(
        NodeCache::Config{}, evictor_,
        [](unique< NodeCore > const& core) -> bnodeid_t { return core->node_id(); });

    overflow_cache_ = std::make_shared< OverflowCache >(
        OverflowCache::Config{}, evictor_,
        [](OverflowEntry const& entry) -> BlkId { return entry.blkid; });

    cp_mgr().register_consumer(cp_consumer_t::INDEX_SVC, cp_callbacks_.get());
}

std::vector< COWBtreeSuperBlock const* > COWBtreeManager::list_persisted_btrees() const {
    std::vector< COWBtreeSuperBlock const* > result;
    result.reserve(pending_btrees_.size());
    for (auto const& info : pending_btrees_) {
        result.push_back(&info.sb);
    }
    return result;
}

void COWBtreeManager::track(cshared< BtreeBase >& bt) {
    std::lock_guard lk(tracking_mtx_);
    tracked_btrees_.push_back(bt);
}

folly::coro::Task< void > COWBtreeManager::destroy_cow_btree(cshared< BtreeBase >& base) {
    // TODO: remove from tracked_btrees_, remove metablk, destroy BlobDev + streams.
    co_return;
}

// ──────────────────────────────────────────── CP callbacks ───────────────────────────────────────────────────────────

void COWBtreeManager::CPCallbacksImpl::on_switchover_cp(CP* cur_cp, CP* new_cp) {
    std::lock_guard lk(mgr_.tracking_mtx_);
    for (auto const& bt : mgr_.tracked_btrees_) {
        COWBtree::cast_to(bt.get())->on_cp_switchover(cur_cp, new_cp);
    }
}

folly::coro::Task< bool > COWBtreeManager::CPCallbacksImpl::cp_flush(CP* cp) {
    std::vector< shared< BtreeBase > > btrees;
    {
        std::lock_guard lk(mgr_.tracking_mtx_);
        btrees = mgr_.tracked_btrees_;
    }

    // Flush all btrees in parallel.  Each COWBtree::cp_flush() internally batches dirty nodes into stream WriteUnits
    // and issues IO as each unit fills — so even heavily-skewed btrees (one with 100K dirty nodes, another with 10)
    // stream out writes incrementally rather than blocking until the end.
    std::vector< folly::coro::Task< bool > > tasks;
    tasks.reserve(btrees.size());
    for (auto const& bt : btrees) {
        tasks.push_back(COWBtree::cast_to(bt.get())->cp_flush(cp));
    }
    auto results = co_await folly::coro::collectAllRange(std::move(tasks));

    bool any_flushed = false;
    for (auto const& r : results) {
        any_flushed |= r;
    }
    co_return any_flushed;
}

void COWBtreeManager::CPCallbacksImpl::cp_cleanup(CP*) {}

int COWBtreeManager::CPCallbacksImpl::cp_progress_percent() { return 100; }

} // namespace homestore
