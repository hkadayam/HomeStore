#pragma once
#include <homestore/checkpoint/cp.hpp>
#include <homestore/btree/detail/btree_internal.hpp>

namespace homestore {
class COWBtreeCPContext;
class COWBtree;
class BtreeNode;

class COWBtreeNodeBuffer {
public:
    COWBtreeNodeBuffer(uint8_t* buf, bool allocated = false) : m_buf{buf}, m_allocated{allocated} {}
    ~COWBtreeNodeBuffer();
    COWBtreeNodeBuffer(COWBtreeNodeBuffer const&) = delete;
    COWBtreeNodeBuffer& operator=(COWBtreeNodeBuffer const&) = delete;
    COWBtreeNodeBuffer(COWBtreeNodeBuffer&& other) {
        m_buf = other.m_buf;
        m_allocated = other.m_allocated;
        other.m_buf = nullptr;
        other.m_allocated = false;
    }
    COWBtreeNodeBuffer& operator=(COWBtreeNodeBuffer&& other) {
        m_buf = other.m_buf;
        m_allocated = other.m_allocated;
        other.m_buf = nullptr;
        other.m_allocated = false;
        return *this;
    }
    uint8_t* bytes() { return m_buf; }

private:
    uint8_t* m_buf;
    bool m_allocated;
};

struct COWBtreeNode {
public:
    std::atomic< uint8_t* > m_copied_version_buf{nullptr};
    static COWBtreeNode* construct(BtreeNodePtr const& node);
    static void destruct(BtreeNode* node);
    static COWBtreeNode* convert(BtreeNodePtr const& node);

private:
    COWBtreeNode() = default;
    ~COWBtreeNode();

public:
    bool copy_buf_if_needed(COWBtree const& bt, cp_id_t cp_id);
    COWBtreeNodeBuffer get_flush_version_buf(cp_id_t cp_id);
    BtreeNode* to_btree_node();
};

} // namespace homestore