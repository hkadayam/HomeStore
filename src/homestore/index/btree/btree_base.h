#pragma once

#include <array>
#include <homestore/index_service.hpp>
#include <homestore/index/btree/detail/btree_internal.h>
#include <homestore/checkpoint/cp_mgr.h>
#include <homestore/index/btree/detail/btree_node.h>
#include <homestore/index/btree/btree_async.h>

namespace homestore {

/// UnderlyingBtree — storage-backend interface.
///
/// create_node / read_node return an unlocked Node (lock_type == None).
/// BtreeBase::read_node(id, LockType) acquires the lock after fetching the Node.
///
/// write_node / refresh_node / remove_node / on_root_changed
/// receive const-ref Nodes; the backend may call node->get() to reach NodeCore.
class UnderlyingBtree {
public:
    virtual ~UnderlyingBtree() = default;

    // Returns an Node core backed by the underlying storage.
    virtual Node create_node(bool is_leaf) = 0;

    // Returns an unlocked Node for an existing on-disk/in-memory node.
    virtual Node read_node(bnodeid_t id) const = 0;

    virtual btree_status_t write_node(Node const& node) = 0;
    virtual btree_status_t prepare_for_write(Node const& node) = 0;
    virtual void remove_node(Node const& node) = 0;
    virtual NodeId load_root_node_id() = 0;
    virtual btree_status_t on_root_changed(Node const& root) = 0;
    virtual uint64_t space_occupied() const = 0;
};

// Btree based implementations superblock area
struct BtreeSuperBlock {
    static constexpr size_t underlying_btree_sb_size =
        IndexSuperBlock::index_impl_sb_size - sizeof(bnodeid_t) - sizeof(uint64_t) - sizeof(uint32_t);

    bnodeid_t root_node_id{empty_bnodeid}; // Btree Root Node ID
    uint64_t root_link_version{0};
    uint32_t node_size{0}; // Node size used for this btree
    std::array< uint8_t, underlying_btree_sb_size > underlying_btree_sb;
};

class BtreeBase;

struct BtreeRouteTracer {
    SCOPED_ENUM_DECL(Op, uint8_t);
    std::vector< bool > m_enabled_ops;
    std::vector< std::string > m_ops_routes;
    uint32_t m_max_buf_size_per_op; // Max size after which the buffer is rolled over
    bool m_log_if_rolled;
    mutable BtreeMutex m_append_mtx;

    BtreeRouteTracer(uint32_t buf_size_per_op = 1 * 1024 * 1024, bool log_if_buf_rolled = false);
    void enable(Op op) { m_enabled_ops[uint32_cast(op)] = true; }
    void disable(Op op) { m_enabled_ops[uint32_cast(op)] = false; }
    void enable_all() { m_enabled_ops.assign(m_enabled_ops.size(), true); }
    void disable_all() { m_enabled_ops.assign(m_enabled_ops.size(), false); }
    bool is_enabled_for(Op op) const { return m_enabled_ops[uint32_cast(op)]; }

    void append_to(Op op, std::string const& route);
    std::string get(Op op) const;
    std::vector< std::string > get_all() const;
};

SCOPED_ENUM_DEF(BtreeRouteTracer, Op, uint8_t, PUT, GET, REMOVE, QUERY);

class BtreeStore;

class BtreeBase : public Index {
public:
    BtreeBase(BtreeConfig const& cfg, uuid_t uuid = uuid_t{}, uuid_t parent_uuid = uuid_t{}, uint32_t user_sb_size = 0);
    BtreeBase(BtreeConfig const& cfg, superblk< IndexSuperBlock >&& sb);
    virtual ~BtreeBase();

    UnderlyingBtree const* underlying_btree() const { return m_bt_private.get(); }
    UnderlyingBtree* underlying_btree() {
        return const_cast< UnderlyingBtree* >(s_cast< const BtreeBase* >(this)->underlying_btree());
    }

    BtreeSuperBlock const& bt_super_blk() const {
        return *(r_cast< BtreeSuperBlock const* >(super_blk()->underlying_index_sb.data()));
    }
    BtreeSuperBlock& bt_super_blk() {
        return const_cast< BtreeSuperBlock& >(s_cast< const BtreeBase* >(this)->bt_super_blk());
    }

    /// Allocate a fresh concrete NodeCore (e.g. SimpleNode<K,V>, VariantNode<K,V>) via placement-new.
    /// Called by the backend's create_node/read_node implementations.
    virtual NodeCore* alloc_node_core(bnodeid_t id, bool is_leaf) const = 0;
    virtual NodeCore* load_node_core(uint8_t* node_buf, bnodeid_t id) const = 0;

    uint64_t space_occupied() const override;
    uint32_t ordinal() const override;

    virtual uint32_t node_size() const;
    std::string name() const;
    BtreeRouteTracer& route_tracer();
    BtreeConfig const& bt_config() const { return m_bt_cfg; }
    [[nodiscard]] CPGuard bt_cp_guard();

public:
    virtual btree_status_t write_node(Node const& node);

    virtual Node create_leaf_node();
    virtual Node create_interior_node();

    // Takes Node by value: unlocks first, then removes from backing store.
    // After this call the Node is in a disarmed (lock_type==NONE) state; the
    // destructor on function-exit releases only the backend reference.
    virtual void remove_node(Node node);

protected:
    virtual btree_status_t create_root_node();
    virtual Node clone_temp_node(NodeCore const& node) = 0;

    // Reads and locks a node.  Returns {success, locked_Node}; on failure
    // the returned Node is invalid (valid() == false).
    virtual BtreeTask< std::pair< btree_status_t, Node > > read_node(NodeId id, LockType lock_type) const;

    virtual BtreeTask< std::pair< btree_status_t, Node > >
    get_child_node(Node const& parent_node, uint32_t index, NodeId& child_nodeid, LockType lock_type) const;

    // Upgrade READ → WRITE lock on parent and child atomically.
    // Updates node.lock_type() on success.
    virtual BtreeTask< btree_status_t > upgrade_node_locks(Node& parent_node, Node& child_node);
    virtual BtreeTask< btree_status_t > upgrade_node_lock(Node& node);

    // Acquires the requested lock; sets node.lock_type() on success.
    virtual BtreeTask< btree_status_t > lock_node(Node& node, LockType type) const;

    // Releases the lock currently held by node and resets node.lock_type() to NONE.
    // Use only inside upgrade sequences — normal unlock is handled by Node's destructor.
    virtual void unlock_node(Node& node) const;

protected:
    shared< BtreeStore > m_store;
    unique< UnderlyingBtree > m_underlying;
    NodeId m_root_node_info;

    BtreeConfig m_bt_cfg;
    BtreeMetrics m_metrics;
    BtreeRouteTracer m_route_tracer;
    std::atomic< uint64_t > m_total_nodes{0};
};

struct BtreeVisualizeVariables {
    uint64_t parent;
    uint64_t midPoint;
    uint64_t index;
};
} // namespace homestore
