#pragma once
#include <homestore/checkpoint/cp.hpp>
#include <homestore/btree/detail/btree_internal.hpp>

namespace homestore {
class COWBtreeCPContext;
class COWBtree;

struct COWBtreeNode {
public:
    uint8_t* m_prev_version_buf{nullptr};
    static COWBtreeNode* construct(BtreeNodePtr const& node);
    static void destruct(BtreeNode* node);
    static COWBtreeNode* convert(BtreeNodePtr const& node);

private:
    COWBtreeNode() = default;
    ~COWBtreeNode();

public:
    bool copy_buf_if_needed(COWBtree const& bt, cp_id_t cp_id);
    uint8_t* get_flush_version_buf(cp_id_t cp_id);
    void reset_prev_version_buf(cp_id_t cp_id);
    BtreeNode* to_btree_node();
};
} // namespace homestore