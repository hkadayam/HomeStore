#include <string>

#include <sisl/logging/logging.h>

#include "homestore/index/btree/btree_base.h"
#include "homestore/index/btree/detail/btree_node.h"
#include "common/homestore_assert.hpp"

namespace homestore {

// ──────────────────────────────────────────── BtreeBase ─────────────────────────────────────────────────────────────

BtreeBase::BtreeBase(BtreeConfig const& cfg, shared< UnderlyingBtree > underlying) :
        underlying_{std::move(underlying)}, bt_cfg_{cfg}, metrics_{cfg.name().c_str()} {
    underlying_->bind_to(this);
}

BtreeBase::~BtreeBase() = default;

uint32_t BtreeBase::node_size() const {
    return bt_cfg_.node_size();
}

std::string BtreeBase::name() const {
    return bt_cfg_.name();
}

BtreeRouteTracer& BtreeBase::route_tracer() {
    return route_tracer_;
}

void BtreeBase::create_root_node() {
    Node root = create_leaf_node();
    root->set_level(0u);
    write_node(root);

    root_node_id_ = root->node_id();
    underlying_->on_root_changed(root);
}

BtreeResult< Node > BtreeBase::get_child_node(const Node& parent_node, uint32_t parent_index, NodeLink& child_link,
                                              LockType lock_type) const {
    if (parent_index == parent_node->total_entries()) {
        if (!parent_node->has_valid_edge()) {
            BT_NODE_LOG_ASSERT(false, parent_node, "Child index {} does not have valid bnode_id", parent_index);
            CO_RETURN folly::makeUnexpected(BtreeStatus::not_found);
        }
        child_link = parent_node->get_edge_value();
    } else {
        BT_NODE_LOG_ASSERT_LT(parent_index, parent_node->total_entries(), parent_node);
        parent_node->get_nth_value(parent_index, &child_link, false /* copy */);
    }

    CO_RETURN CO_AWAIT(underlying_->read_node(child_link.bnode_id(), lock_type));
}

void BtreeBase::write_node(Node const& node) {
    COUNTER_INCREMENT_IF_ELSE(metrics_, node->is_leaf(), btree_leaf_node_writes, btree_int_node_writes, 1);
    HISTOGRAM_OBSERVE_IF_ELSE(metrics_, node->is_leaf(), btree_leaf_node_occupancy, btree_int_node_occupancy,
                              ((node_size() - node->available_size()) * 100) / node_size());
    underlying_->write_node(node);
}

/*
 * This function upgrades the parent node and child node locks from read lock to write lock and take required steps if
 * things have changed during the upgrade.
 *
 * Inputs:
 * parent_node - Parent Node to upgrade
 * child_node - Child Node to upgrade
 * child_cur_lock - Current child node which is held
 * context - Context to pass down
 *
 * Returns - If successfully able to upgrade both the nodes, return success, else return status of upgrade_node.
 * In case of not success, all nodes locks are released.
 *
 * NOTE: This function expects both the parent_node and child_node to be already locked. Parent node is
 * expected to be read locked and child node could be either read or write locked.
 */
BtreeTask< BtreeStatus > BtreeBase::upgrade_node_locks(Node& parent_node, Node& child_node) {
    auto const parent_gen = parent_node->node_gen();
    auto const child_gen = child_node->node_gen();

    child_node.release();
    parent_node.release();

    CO_AWAIT parent_node.acquire(LockType::Write);
    auto ret = underlying_->prepare_for_write(parent_node);
    if (ret != BtreeStatus::success) {
        parent_node.release();
        CO_RETURN ret;
    }

    CO_AWAIT child_node.acquire(LockType::Write);
    ret = underlying_->prepare_for_write(child_node);
    if (ret != BtreeStatus::success) {
        child_node.release();
        parent_node.release();
        CO_RETURN ret;
    }

    if (parent_node->is_node_deleted() || (parent_gen != parent_node->node_gen()) || child_node->is_node_deleted() ||
        (child_gen != child_node->node_gen())) {
        CO_RETURN BtreeStatus::retry;
    }


#if 0
#ifdef _PRERELEASE
    {
        int is_leaf = 0;

        if (child_node && child_node->is_leaf()) { is_leaf = 1; }
        if (iomgr_flip::instance()->test_flip("btree_upgrade_node_fail", is_leaf)) {
            unlock_node(my_node, cur_lock);
            cur_lock = locktype_t::NONE;
            if (child_node) {
                unlock_node(child_node, child_cur_lock);
                child_cur_lock = locktype_t::NONE;
            }
            ret = BtreeStatus::retry;
        }
    }
#endif
#endif

    CO_RETURN ret;
}

Node BtreeBase::create_leaf_node() {
    Node n = underlying_->create_node(/*is_leaf=*/true);
    COUNTER_INCREMENT(metrics_, btree_leaf_node_count, 1);
    ++total_nodes_;
    return n;
}

Node BtreeBase::create_interior_node() {
    Node n = underlying_->create_node(/*is_leaf=*/false);
    COUNTER_INCREMENT(metrics_, btree_int_node_count, 1);
    ++total_nodes_;
    return n;
}

void BtreeBase::remove_node(Node node) {
    COUNTER_DECREMENT_IF_ELSE(metrics_, node->is_leaf(), btree_leaf_node_count, btree_int_node_count, 1);
    if (node.lock_type() != LockType::Write) {
        BT_NODE_DBG_ASSERT(false, node, "We can't remove a node with read lock type right?");
        node->set_node_deleted();
    }
    --total_nodes_;
    underlying_->remove_node(node);
}

// ──────────────────────────────────────────── BtreeRouteTracer ──────────────────────────────────────────────────────

BtreeRouteTracer::BtreeRouteTracer(uint32_t buf_size_per_op, bool log_if_rolled) :
        max_buf_size_per_op_{buf_size_per_op}, log_if_rolled_{log_if_rolled} {
    auto const count = enum_count< BtreeRouteTracer::Op >();
    enabled_ops_.resize(count, false);
    ops_routes_.resize(count);
}

void BtreeRouteTracer::append_to(Op op, std::string const& route_str) {
    if (!enabled_ops_[uint32_cast(op)]) {
        return;
    }

    std::unique_lock lg{append_mtx_};
    std::string& cur_buf = ops_routes_[uint32_cast(op)];
    while (cur_buf.size() + route_str.size() > max_buf_size_per_op_) {
        size_t head_pos = cur_buf.find("Route size=");
        size_t next_pos = cur_buf.find("Route size=", head_pos + 1);
        if (log_if_rolled_) {
            LOGINFOMOD(btree, "Btree Route Trace: {}", std::string_view(cur_buf).substr(head_pos, next_pos));
        }
        if (next_pos == std::string::npos) {
            cur_buf.clear();
            break;
        } else {
            cur_buf.erase(0, next_pos);
        }
    }
    cur_buf.append(route_str);
}

std::string BtreeRouteTracer::get(Op op) const {
    std::shared_lock lg{append_mtx_};
    return ops_routes_[uint32_cast(op)];
}

std::vector< std::string > BtreeRouteTracer::get_all() const {
    std::shared_lock lg{append_mtx_};
    return ops_routes_;
}

} // namespace homestore
