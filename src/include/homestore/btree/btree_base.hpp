#pragma once

#include <array>
#include <homestore/index_service.hpp>
#include <homestore/btree/detail/btree_internal.hpp>
#include <homestore/checkpoint/cp_mgr.hpp>

namespace homestore {
class UnderlyingBtree {
public:
    virtual ~UnderlyingBtree() = default;
};

// Btree based implementations superblock area
struct BtreeSuperBlock {
    static constexpr size_t underlying_btree_sb_size = IndexSuperBlock::index_impl_sb_size - sizeof(bnodeid_t);

    bnodeid_t root_node{empty_bnodeid}; // Btree Root Node ID
    std::array< uint8_t, underlying_btree_sb_size > underlying_btree_sb;
};

class BtreeBase;
class BtreeCPGuard : public CPGuard {
public:
    BtreeCPGuard(BtreeBase& btree);
    ~BtreeCPGuard() = default;

    BtreeCPGuard(const BtreeCPGuard& other);
    BtreeCPGuard operator=(const BtreeCPGuard& other);

    CPContext* context();
    CP& operator*() override;
    CP* operator->() override;
    CP* get() override;

private:
    BtreeBase& m_btree;
};

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
    virtual uint32_t node_size() const;
    uint64_t used_size() const override;
    uint32_t ordinal() const override;
    std::string name() const;

    [[nodiscard]] BtreeCPGuard bt_cp_guard() { return BtreeCPGuard{*this}; }

protected:
    shared< BtreeStore > m_store;
    unique< UnderlyingBtree > m_bt_private;
    BtreeConfig m_bt_cfg;
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