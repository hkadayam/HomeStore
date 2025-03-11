#pragma once

#include <array>
#include <homestore/index_service.hpp>
#include <homestore/btree/detail/btree_internal.hpp>
#include <homestore/checkpoint/cp_mgr.hpp>

namespace homestore {
class UnderlyingBtree {
public:
    virtual ~UnderlyingBtree() = default;

    // Get the node size of underlying btree. The return is expected to be non-zero
    virtual uint32_t node_size() const = 0;

    virtual BtreeNodePtr create_node(bool is_leaf, CPContext* context) = 0;
    virtual btree_status_t write_node(BtreeNodePtr const& node, CPContext* context) = 0;
    virtual btree_status_t read_node(bnodeid_t id, BtreeNodePtr& node) const = 0;
    virtual btree_status_t refresh_node(BtreeNodePtr const& node, bool for_read_modify_write,
                                        CPContext* context) const = 0;
    virtual void remove_node(BtreeNodePtr const& node, CPContext* context) = 0;
    virtual btree_status_t transact_nodes(const BtreeNodeList& new_nodes, const BtreeNodeList& removed_nodes,
                                          const BtreeNodePtr& left_child_node, const BtreeNodePtr& parent_node,
                                          CPContext* context) = 0;
    virtual btree_status_t on_root_changed(BtreeNodePtr const& root, CPContext* context) = 0;
    virtual uint64_t space_occupied() const = 0;
};

// Btree based implementations superblock area
struct BtreeSuperBlock {
    static constexpr size_t underlying_btree_sb_size =
        IndexSuperBlock::index_impl_sb_size - sizeof(bnodeid_t) - sizeof(uint32_t);

    bnodeid_t root_node{empty_bnodeid}; // Btree Root Node ID
    uint32_t node_size{0};              // Node size used for this btree
    std::array< uint8_t, underlying_btree_sb_size > underlying_btree_sb;
};

class BtreeBase;

struct BtreeRouteTracer {
    SCOPED_ENUM_DECL(Op, uint8_t);
    std::vector< bool > m_enabled_ops;
    std::vector< std::string > m_ops_routes;
    uint32_t m_max_buf_size_per_op; // Max size after which the buffer is rolled over
    bool m_log_if_rolled;
    mutable iomgr::FiberManagerLib::shared_mutex m_append_mtx;

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
    virtual ~BtreeBase() = default;

    UnderlyingBtree const* underlying_btree() const { return m_bt_private.get(); }
    UnderlyingBtree* underlying_btree() {
        return const_cast< UnderlyingBtree* >(s_cast< const BtreeBase* >(this)->underlying_btree());
    }

    superblk< IndexSuperBlock >& super_blk() {
        return const_cast< superblk< IndexSuperBlock >& >(s_cast< const Index* >(this)->super_blk());
    }

    virtual BtreeNode* init_node(uint8_t* node_buf, bnodeid_t id, bool init_buf, bool is_leaf, uint32_t ctx_size) = 0;
    virtual void setup_node_size(uint32_t node_size);
    virtual uint32_t node_size() const;
    uint64_t space_occupied() const override;
    uint32_t ordinal() const override;
    std::string name() const;
    BtreeRouteTracer& route_tracer();
    BtreeConfig const& bt_config() const { return m_bt_cfg; }
    [[nodiscard]] CPGuard bt_cp_guard();

protected:
    shared< BtreeStore > m_store;
    unique< UnderlyingBtree > m_bt_private;
    BtreeConfig m_bt_cfg;
    BtreeRouteTracer m_route_tracer;
};

struct BtreeVisualizeVariables {
    uint64_t parent;
    uint64_t midPoint;
    uint64_t index;
};

struct btree_locked_node_info {
    BtreeNode* node;
    Clock::time_point start_time;
    const char* fname;
    int line;

    void dump() const { LOGINFO("node locked by file: {}, line: {}", fname, line); }
};

struct BtreeThreadVariables {
    std::vector< btree_locked_node_info > wr_locked_nodes;
    std::vector< btree_locked_node_info > rd_locked_nodes;
    BtreeNodePtr force_split_node{nullptr};
};

struct BTREE_FLIPS {
    static constexpr uint32_t INDEX_PARENT_NON_ROOT = 1 << 0;
    static constexpr uint32_t INDEX_PARENT_ROOT = 1 << 1;
    static constexpr uint32_t INDEX_LEFT_SIBLING = 1 << 2;
    static constexpr uint32_t INDEX_RIGHT_SIBLING = 1 << 3;

    uint32_t flips;
    BTREE_FLIPS() : flips{0} {}
    std::string list() const {
        std::string str;
        if (flips & INDEX_PARENT_NON_ROOT) { str += "index_parent_non_root,"; }
        if (flips & INDEX_PARENT_ROOT) { str += "index_parent_root,"; }
        if (flips & INDEX_LEFT_SIBLING) { str += "index_left_sibling,"; }
        if (flips & INDEX_RIGHT_SIBLING) { str += "index_right_sibling,"; }
        return str;
    }
    void set_flip(uint32_t flip) { flips |= flip; }
    void set_flip(std::string flip) {
        if (flip == "index_parent_non_root") { set_flip(INDEX_PARENT_NON_ROOT); }
        if (flip == "index_parent_root") { set_flip(INDEX_PARENT_ROOT); }
        if (flip == "index_left_sibling") { set_flip(INDEX_LEFT_SIBLING); }
        if (flip == "index_right_sibling") { set_flip(INDEX_RIGHT_SIBLING); }
    }
};

} // namespace homestore