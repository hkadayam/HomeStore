#pragma once

#include <atomic>
#include "common/async.h"
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include <folly/Expected.h>

#include "homestore/base/blk.h"
#include "sisl/fds/buffer.h"
#include "sisl/fds/enum.h"
#include "sisl/metrics/metrics.h"
#include "common/defs.h"
#include "homestore/index/btree/btree_async.h"
#include "homestore/index/btree/detail/btree_internal.h"
#include "homestore/index/btree/detail/btree_node.h"

namespace homestore {

template < typename T >
using BtreeResult = BtreeTask< folly::Expected< T, BtreeStatus > >;

class BtreeBase;

// ──────────────────────────────────────────── UnderlyingBtree ───────────────────────────────────────────────────────
class UnderlyingBtree {
public:
    virtual ~UnderlyingBtree() = default;

    virtual void bind_to(BtreeBase* base) = 0;
    virtual Node create_node(bool is_leaf) = 0;
    virtual BtreeResult< Node > read_node(bnodeid_t id, LockType lock_type) = 0;
    virtual void write_node(Node const& node) = 0;
    virtual BtreeStatus prepare_for_write(Node const& node) = 0;
    virtual void remove_node(Node const& node) = 0;
    virtual void on_root_changed(Node const& root) = 0;
    virtual uint64_t space_occupied() const = 0;

    // Allocate a raw node-sized buffer that matches this backend's alignment/tagging requirements.  Used by merge to
    // install a rollback-able working copy on an existing node's phys_node_buf_ without going through create_node()
    // (which would register a fresh NodeCore with the backend).
    virtual std::shared_ptr< uint8_t > allocate_node_buf() = 0;

    // Optional per-op guard (e.g. CPGuard in COWBtree).  MemBtree returns a no-op default.
    virtual OpGuard enter_op() { return OpGuard{}; }

    virtual BtreeStatus write_overflow(sisl::IoBufShared const& buf, BlkId& out_blkid) {
        return BtreeStatus::not_supported;
    }
    virtual BtreeTask< BtreeStatus > read_overflow(BlkId const& blkid, sisl::IoBufShared& out_buf) const {
        CO_RETURN BtreeStatus::not_supported;
    }
    virtual BtreeStatus delete_overflow(BlkId const& blkid) { return BtreeStatus::not_supported; }
};

// ──────────────────────────────────────────── BtreeRouteTracer ──────────────────────────────────────────────────────
struct BtreeRouteTracer {
    SCOPED_ENUM_DECL(Op, uint8_t);
    std::vector< bool > enabled_ops_;
    std::vector< std::string > ops_routes_;
    uint32_t max_buf_size_per_op_;
    bool log_if_rolled_;
    // Sync lock — BtreeRouteTracer is internal bookkeeping and never suspends, so it uses std::shared_mutex
    // directly rather than the mode-dependent BtreeSharedMutex (which resolves to folly::coro::SharedMutex in
    // async mode and has no sync lock()/lock_shared()).
    mutable folly::SharedMutex append_mtx_;

    BtreeRouteTracer(uint32_t buf_size_per_op = 1 * 1024 * 1024, bool log_if_buf_rolled = false);
    void enable(Op op) { enabled_ops_[uint32_cast(op)] = true; }
    void disable(Op op) { enabled_ops_[uint32_cast(op)] = false; }
    void enable_all() { enabled_ops_.assign(enabled_ops_.size(), true); }
    void disable_all() { enabled_ops_.assign(enabled_ops_.size(), false); }
    bool is_enabled_for(Op op) const { return enabled_ops_[uint32_cast(op)]; }

    void append_to(Op op, std::string const& route);
    std::string get(Op op) const;
    std::vector< std::string > get_all() const;
};

SCOPED_ENUM_DEF(BtreeRouteTracer, Op, uint8_t, PUT, GET, REMOVE, QUERY);

// ──────────────────────────────────────────── BtreeBase ─────────────────────────────────────────────────────────────
class BtreeBase {
public:
    BtreeBase(BtreeConfig const& cfg, shared< UnderlyingBtree > underlying);
    virtual ~BtreeBase();

    UnderlyingBtree const* underlying_btree() const { return underlying_.get(); }
    UnderlyingBtree* underlying_btree() {
        return const_cast< UnderlyingBtree* >(s_cast< const BtreeBase* >(this)->underlying_btree());
    }

    virtual unique< NodeCore > construct_fresh_node(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf) = 0;
    virtual unique< NodeCore > construct_existing_node(std::shared_ptr< uint8_t > buf, bnodeid_t id) = 0;

    uint32_t node_size() const;
    std::string name() const;
    BtreeRouteTracer& route_tracer();
    BtreeConfig const& bt_config() const { return bt_cfg_; }

    void write_node(Node const& node);
    Node create_leaf_node();
    Node create_interior_node();
    void remove_node(Node node);

protected:
    void create_root_node();
    BtreeResult< Node > get_child_node(const Node& parent_node, uint32_t index, LockType lock_type) const;
    BtreeTask< BtreeStatus > upgrade_node_locks(Node& parent_node, Node& child_node);

protected:
    shared< UnderlyingBtree > underlying_;
    bnodeid_t root_node_id_;

    BtreeConfig bt_cfg_;
    BtreeMetrics metrics_;
    BtreeRouteTracer route_tracer_;
    std::atomic< uint64_t > total_nodes_{0};
};

struct BtreeVisualizeVariables {
    uint64_t parent;
    uint64_t midPoint;
    uint64_t index;
};
} // namespace homestore
