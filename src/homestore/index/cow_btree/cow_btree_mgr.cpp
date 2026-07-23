#include "common/async.h"

#include "sisl/flip/flip.h"

#include "homestore/index/cow_btree/cow_btree_mgr.h"
#include "homestore/index/cow_btree/cow_btree.h"
#include "homestore/base/hs_runtime_config.h" // HS_RUNTIME_CONFIG
#include "homestore/base/resource_mgr.h"
#include "homestore/blob/blob_dev.h"
#include "homestore/blob/blob_dev_mgr.h"
#include "homestore/device/device_manager.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/managers.h"

namespace homestore {

// Logical name registered with the MetaBlkManager.  All COWBtree per-instance MetaBlks live under this single client.
static constexpr char COW_BTREE_MGR_META_CLIENT_NAME[] = "cow_btree_mgr";

// CP flush rank for COWBtreeManager. See CPRank docs in cp_mgr.h for the layering scheme.
static constexpr uint32_t kCPRank_COWBtree = 10;

// ──────────────────────────────────────────────── Lifecycle ──────────────────────────────────────────────────────────

Async< void > COWBtreeManager::create() {
    auto mgr = shared< COWBtreeManager >(new COWBtreeManager());
    mgr->meta_client_ =
        std::make_shared< MetaClient >(co_await meta_mgr().register_client(COW_BTREE_MGR_META_CLIENT_NAME));
    Managers::init_cow_btree_mgr(std::move(mgr));
    co_return;
}

Async< void > COWBtreeManager::load() {
    auto mgr = shared< COWBtreeManager >(new COWBtreeManager());
    mgr->meta_client_ =
        std::make_shared< MetaClient >(co_await meta_mgr().register_client(COW_BTREE_MGR_META_CLIENT_NAME));

    // Walk every persisted MetaBlk under our client.  Each block holds one COWBtree's superblock (followed by an
    // optional user_sb).  Stash them in pending_btrees_ so the upper layer can iterate via list_persisted_btrees()
    // and call load_cow_btree<K,V>() for each one with the right K/V types.
    co_await mgr->meta_client_->for_each_recovered_block([&mgr](MetaBlk const& blk,
                                                                sisl::IoBufView data) -> Async< void > {
        HS_REL_ASSERT_GE(data.size(), sizeof(COWBtreeSuperBlock), "COWBtree metablk too small: {} bytes", data.size());
        PersistedBtreeInfo info;
        std::memcpy(&info.sb, data.bytes(), sizeof(COWBtreeSuperBlock));
        info.mblk = blk;
        mgr->ordinal_reserver_.reserve(info.sb.ordinal);
        mgr->pending_btrees_.push_back(std::move(info));
        co_return;
    });

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

COWBtreeManager::COWBtreeManager() : cp_callbacks_{std::make_shared< CPCallbacksImpl >(*this)} {
    // Single evictor shared by both caches — total cache budget is split across node and overflow data.
    // ResourceMgr owns the global cache budget (mem_cap * resource_limits.cache_size_percent).
    sisl::TwoQEvictor::Config ev_cfg;
    ev_cfg.max_size = resource_mgr().cache_size();
    ev_cfg.num_partitions = HS_RUNTIME_CONFIG(cache->num_evictor_partitions);
    ev_cfg.hot_pct = static_cast< float >(HS_RUNTIME_CONFIG(cache->hot_size_pct) / 100.0);
    ev_cfg.high_wm_pct = static_cast< float >(HS_RUNTIME_CONFIG(cache->high_watermark_pct) / 100.0);
    ev_cfg.low_wm_pct = static_cast< float >(HS_RUNTIME_CONFIG(cache->low_watermark_pct) / 100.0);
    evictor_ = std::make_shared< sisl::TwoQEvictor >(ev_cfg);

    // Derive num_buckets from total budget assuming ~4KB avg entry; entries_per_hash_bucket is the target ratio.
    auto const entries_per_bucket = HS_RUNTIME_CONFIG(cache->entries_per_hash_bucket);
    constexpr uint64_t avg_entry_size = 4096;
    auto const num_buckets =
        std::max< uint32_t >(1, to_u32(ev_cfg.max_size / avg_entry_size / std::max< uint32_t >(1, entries_per_bucket)));
    auto const ghost_capacity = HS_RUNTIME_CONFIG(cache->ghost_capacity_per_partition);

    NodeCache::Config node_cfg;
    node_cfg.num_buckets = num_buckets;
    node_cfg.ghost_capacity = ghost_capacity;
    node_cache_ = std::make_shared< NodeCache >(
        node_cfg, evictor_, [](unique< NodeCore > const& core) -> bnodeid_t { return core->node_id(); });

    OverflowCache::Config overflow_cfg;
    overflow_cfg.num_buckets = num_buckets;
    overflow_cfg.ghost_capacity = ghost_capacity;
    overflow_cache_ = std::make_shared< OverflowCache >(
        overflow_cfg, evictor_, [](OverflowEntry const& entry) -> BlkId { return entry.blkid; });

    cp_mgr().register_consumer("COWBtreeManager", cp_callbacks_, kCPRank_COWBtree);
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

Async< void > COWBtreeManager::destroy_cow_btree(cshared< BtreeBase >& base) {
    auto* cow_bt = COWBtree::cast_to(base.get());
    auto const ordinal = cow_bt->ordinal();

    // Tears down on-disk state (streams + per-btree metablk). Cache entries become stale and evict naturally.
    // The shared BlobDev is owned by BlobDevManager and is intentionally NOT destroyed here.
    co_await cow_bt->destroy();

    {
        std::lock_guard lk(tracking_mtx_);
        tracked_btrees_.erase(std::remove(tracked_btrees_.begin(), tracked_btrees_.end(), base), tracked_btrees_.end());
    }

    ordinal_reserver_.unreserve(ordinal);
    co_return;
}

bool COWBtreeManager::should_force_full_flush() const {
    // Test-only override: when the "force_full_map_flush" flip is set, the next CP is forced to be a full-map flush
    // regardless of the incr-map size threshold below.  Recovery tests use this to deterministically drive full vs
    // incremental flush sequences.
    if (flip::Flip::instance().test_flip("force_full_map_flush")) {
        return true;
    }

    auto cap = device_mgr().total_capacity_by_type(HSDevType::Fast);
    if (cap == 0) {
        cap = device_mgr().total_capacity_by_type(HSDevType::Data);
    }

    if (cap == 0) {
        return false;
    }

    auto const limit_bytes = to_u64(to_double(cap) * HS_RUNTIME_CONFIG(btree->cow_incr_map_max_size_pct) / 100.0);
    return incr_map_total_bytes_.load(std::memory_order_relaxed) >= limit_bytes;
}

// ──────────────────────────────────────────── CP callbacks ───────────────────────────────────────────────────────────

void COWBtreeManager::CPCallbacksImpl::on_switchover_cp(CP* cur_cp, CP* new_cp) {
}

Async< bool > COWBtreeManager::CPCallbacksImpl::cp_flush(CP* cp) {
    std::vector< shared< BtreeBase > > btrees;
    {
        std::lock_guard lk(mgr_.tracking_mtx_);
        btrees = mgr_.tracked_btrees_;
    }

    // Decide once for this CP whether to suggest incremental or full flush.  The threshold check (incr_map total
    // bytes vs % of fast-dev capacity) is the same for every btree, so computing it per-btree would be wasteful.
    // Each btree may still override the suggestion based on its own state.
    bool const suggest_incremental = !mgr_.should_force_full_flush();

    // Flush all btrees in parallel.  Each COWBtree::cp_flush() internally batches dirty nodes into stream WriteUnits
    // and issues IO as each unit fills — so even heavily-skewed btrees (one with 100K dirty nodes, another with 10)
    // stream out writes incrementally rather than blocking until the end.
    std::vector< Async< void > > tasks;
    tasks.reserve(btrees.size());
    for (auto const& bt : btrees) {
        tasks.push_back(COWBtree::cast_to(bt.get())->cp_flush(cp, suggest_incremental));
    }
    co_await folly::coro::collectAllRange(std::move(tasks));
    co_return true;
}

void COWBtreeManager::CPCallbacksImpl::cp_cleanup(CP*) {
}

int COWBtreeManager::CPCallbacksImpl::cp_progress_percent() {
    return 100;
}

} // namespace homestore
