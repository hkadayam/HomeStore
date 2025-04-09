#pragma once
#include <homestore/checkpoint/cp.hpp>
#include <homestore/btree/detail/btree_internal.hpp>

namespace homestore {
class COWBtreeCPContext;
class COWBtree;

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
    // Is the buffer for the node is exclusive contained in the node or shared
    std::atomic< bool > m_is_buf_exclusive{true};

    struct Buffer {
        BtreeNodePtr node;
        uint8_t* buf;

        Buffer(BtreeNodePtr n, uint8_t* b) : node{std::move(n)}, buf{b} {}
        Buffer& operator=(Buffer&& other) {
            node = std::move(other.node);
            buf = other.buf;
            other.buf = nullptr;
            return *this;
        }
        uint8_t* bytes() { return buf; }
    };

    static COWBtreeNode* construct(BtreeNodePtr const& node);
    static void destruct(BtreeNode* node);
    static COWBtreeNode* convert(BtreeNodePtr const& node);
    static Buffer prepare_flush_buf(BtreeNode node, cp_id_t cur_cp_id);

private:
    COWBtreeNode() = default;
    ~COWBtreeNode() = default;

public:
    uint8_t* share_buf();
    void release_buf(uint8_t* buf);
    BtreeNode* to_btree_node();
};

} // namespace homestore