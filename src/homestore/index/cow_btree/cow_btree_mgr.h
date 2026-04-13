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
#include <homestore/blk.h>
#include <homestore/checkpoint/cp_mgr.h>
#include <homestore/index/btree/detail/btree_internal.h>

#include "iomanager/drive_interface.hpp" // IOBuffer
#include "meta/meta_blk.h"

namespace homestore {
class BtreeBase;
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
    folly::coro::Task< shared< BtreeBase > > create_cow_btree(BtreeConfig const& cfg, shared< BlobDev > blob_dev,
                                                              sisl::Blob const& user_sb = {});

    template < typename K, typename V >
    shared< BtreeBase > load_cow_btree(BtreeConfig const& cfg, COWBtreeSuperBlock const& sb);

    folly::coro::Task< void > destroy_cow_btree(cshared< BtreeBase >& base);

private:
    COWBtreeManager();

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

    std::mutex tracking_mtx_;
    std::vector< shared< BtreeBase > > tracked_btrees_;
    std::vector< PersistedBtreeInfo > pending_btrees_;
    std::atomic< uint32_t > next_ordinal_{0};
    unique< CPCallbacksImpl > cp_callbacks_;
    uint32_t num_incremental_flushes_{0};
};

} // namespace homestore
