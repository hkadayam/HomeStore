#pragma once
#include <homestore/checkpoint/cp.hpp>

namespace homestore {
class COWBtreeCPContext;
class COWBtree;
class BtreeNode;

struct COWBtreeNode {
public:
    uint8_t* m_prev_version_buf{nullptr};

public:
    COWBtreeNode() = default;
    ~COWBtreeNode();

    bool copy_buf_if_needed(COWBtree& bt, cp_id_t cp_id);
    uint8_t* get_flush_version_buf(cp_id_t cp_id);
    void reset_prev_version_buf(cp_id_t cp_id);
    BtreeNode* to_btree_node();
};
} // namespace homestore