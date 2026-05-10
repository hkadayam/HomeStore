#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <folly/coro/Task.h>

#include "sisl/cache/cache.h"
#include "sisl/cache/two_q_evictor.h"

#include "common/defs.h"
#include "homestore/base/blk.h"
#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/index/btree/detail/btree_internal.h"
#include "sisl/fds/id_reserver.h"

#include "homestore/meta/meta_blk.h"
#include "homestore/meta/meta_client.h"

namespace homestore {
class BlobDev;
class BtreeBase;
template < typename K, typename V >
class Btree;
struct OverflowEntry;

// ──────────────────────────────────────── COWBtreeSuperBlock ─────────────────────────────────────────────────────────
// Per-btree metablk payload.  `btree_name` doubles as the BlobDev device name (one BlobDev per COWBtree).
#pragma pack(1)
struct COWBtreeSuperBlock {
    boost::uuids::uuid uuid{};
    boost::uuids::uuid parent_uuid{};
    uint32_t ordinal{0};
    uint32_t node_size{0};
    char btree_name[64]{};
    bnodeid_t root_node_id{empty_bnodeid};

    uint64_t node_stream_id{0};
    uint64_t overflow_stream_id{0};
    uint64_t incr_map_stream_id{0};
    uint64_t full_map_stream_ids[2]{0, 0};
    cp_id_t last_full_map_cp_id{-1};

    uint32_t user_sb_size{0};

    uint8_t* user_sb_data() { return r_cast< uint8_t* >(this) + sizeof(COWBtreeSuperBlock); }
    uint8_t const* user_sb_data() const { return r_cast< uint8_t const* >(this) + sizeof(COWBtreeSuperBlock); }

    void set_btree_name(std::string const& name) {
        std::memset(btree_name, 0, sizeof(btree_name));
        std::strncpy(btree_name, name.c_str(), sizeof(btree_name) - 1);
    }
};
#pragma pack()

// ──────────────────────────────────────── COWBtreeManager ───────────────────────────────────────────────────────────
class COWBtreeManager : public std::enable_shared_from_this< COWBtreeManager > {
public:
    using NodeCache = sisl::Cache< bnodeid_t, unique< NodeCore > >;
    using OverflowCache = sisl::Cache< BlkId, OverflowEntry >;

    // Called from HomeStore::do_start().  Registers with cp_mgr, reads persisted metablks from meta service, stashes
    // them for the upper layer to iterate.  Registered in Managers as cow_btree_mgr().
    // First-time boot: construct an empty manager, register with cp_mgr, install in Managers.
    static folly::coro::Task< void > create();

    // Recovery boot: same as create(), plus read all persisted COWBtree metablks and stash them for the upper layer
    // to iterate via list_persisted_btrees().
    static folly::coro::Task< void > load();

    void shutdown();

    std::vector< COWBtreeSuperBlock const* > list_persisted_btrees() const;

    template < typename K, typename V >
    folly::coro::Task< shared< Btree< K, V > > > create_cow_btree(BtreeConfig const& cfg, shared< BlobDev > blob_dev,
                                                                  sisl::Blob const& user_sb = {});

    template < typename K, typename V >
    folly::coro::Task< shared< Btree< K, V > > > load_cow_btree(BtreeConfig const& cfg, shared< BlobDev > blob_dev,
                                                                COWBtreeSuperBlock const& sb);

    folly::coro::Task< void > destroy_cow_btree(cshared< BtreeBase >& base);

    // ── Incremental map size accounting
    // Tracks total bytes currently held in the incr_map streams across all COWBtree instances.  COWBtree calls
    // incr_map_appended() after each successful incr_cp_flush() (delta = post-flush tail_offset - pre-flush) and
    // incr_map_truncated() after each full_cp_flush() truncates its incr stream (delta = pre-truncate tail_offset). The
    // per-CP "should we go full?" decision is computed once in the manager (see CPCallbacksImpl::cp_flush) and passed
    // to each btree as a suggestion via COWBtree::cp_flush(cp, suggest_incremental).
    void incr_map_appended(uint64_t bytes) noexcept {
        incr_map_total_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void incr_map_truncated(uint64_t bytes) noexcept {
        incr_map_total_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }
    uint64_t incr_map_total_bytes() const noexcept { return incr_map_total_bytes_.load(std::memory_order_relaxed); }

private:
    COWBtreeManager();

    // Computed once per CP by the manager — not exposed to btrees.  Returns true when accumulated incr_map bytes
    // exceed the configured percent of fast-dev capacity, indicating the next CP should do a full_map_flush.
    bool should_force_full_flush() const;

    class CPCallbacksImpl : public CPCallbacks {
    public:
        explicit CPCallbacksImpl(COWBtreeManager& mgr) : mgr_{mgr} {}
        void on_switchover_cp(CP* cur_cp, CP* new_cp) override;
        folly::coro::Task< bool > cp_flush(CP* cp) override;
        void cp_cleanup(CP* cp) override;
        int cp_progress_percent() override;

    private:
        COWBtreeManager& mgr_;
    };

    void track(cshared< BtreeBase >& bt);

    struct PersistedBtreeInfo {
        COWBtreeSuperBlock sb;
        MetaBlk mblk;
    };

    shared< sisl::TwoQEvictor > evictor_;
    shared< NodeCache > node_cache_;
    shared< OverflowCache > overflow_cache_;
    shared< MetaClient > meta_client_;

    std::mutex tracking_mtx_;
    std::vector< shared< BtreeBase > > tracked_btrees_;
    std::vector< PersistedBtreeInfo > pending_btrees_;
    sisl::IDReserver ordinal_reserver_;
    shared< CPCallbacksImpl > cp_callbacks_;

    // Sum of incr_map stream sizes across all live COWBtrees.  Compared against incr_map_max_bytes() each CP.
    std::atomic< uint64_t > incr_map_total_bytes_{0};
};

} // namespace homestore
