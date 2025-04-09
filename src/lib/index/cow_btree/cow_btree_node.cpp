#include <homestore/btree/detail/btree_node.hpp>
#include "index/cow_btree/cow_btree_cp.h"
#include "index/cow_btree/cow_btree_node.h"
#include "index/cow_btree/cow_btree.h"
#include "common/homestore_utils.hpp"
#include "common/homestore_assert.hpp"

namespace homestore {
BtreeNode* COWBtreeNode::to_btree_node() { return r_cast< BtreeNode* >(uintptr_cast(this) + sizeof(COWBtreeNode)); }

COWBtreeNode* COWBtreeNode::construct(BtreeNodePtr const& node) {
    return new (uintptr_cast(node.get()) - sizeof(COWBtreeNode)) COWBtreeNode();
}

void COWBtreeNode::destruct(BtreeNode* node) {
    HS_DBG_ASSERT_EQ(m_is_buf_exclusive.load(), true,
                     "A destructing node shouldn't be sharing the buffer with cp, but here it is shared.",
                     fmt::ptr(node->get_phys_buf()));

    // Release the node buffer
    hs_utils::iobuf_free(node->get_phys_buf(), sisl::buftag::btree_node);

    // Release the entire BtreeNode covering structure
    uint8_t* ptr = uintptr_cast(node) - sizeof(COWBtreeNode);
    r_cast< COWBtreeNode* >(ptr)->~COWBtreeNode();
    delete[] ptr;
}

COWBtreeNode* COWBtreeNode::convert(BtreeNodePtr const& n) {
    return r_cast< COWBtreeNode* >(uintptr_cast(n.get()) - sizeof(COWBtreeNode));
}

COWBtreeNode::Buffer COWBtreeNode::prepare_flush_buf(BtreeNode node, cp_id_t cur_cp_id) {
    COWBtreeNode* cow_node = convert(node);
    uint8_t* ret_buf = cow_node->try_share_buf();

    // If the buffer for the current version was written as part of previous cp (exactly 1 behind requested cp), then we
    // need to check if previous cp is still in flushing the but. If so, we have to make a copy and use new version to
    // write. We preserve existing version until it is flushed.
    auto const node_cp_id = node->get_modified_cp_id();
    if (node_cp_id == (cur_cp_id - 1)) {
        // We tried to see if the buffer can be shared. If it possible that the flush of previous cp is completed and
        // the flushing thread released the buffer. In that case it is safe to share it.
        if (ret_buf == nullptr) {
            // We couldn't share the buffer, so we need to make a copy of it.
            ret_buf = hs_utils::iobuf_alloc(node->node_size(), sisl::buftag::btree_node, node->align_size());
            std::memcpy(ret_buf, node->get_phys_buf(), node->node_size());
        }
    } else {
        HS_DBG_ASSERT_NE(ret_buf, (void*)nullptr,
                         "Node={} was modified by earlier cp_id, but we couldn't share the buffer.", node->to_string());
    }
    node->set_modified_cp_id(cur_cp_id);
    return Buffer{std::move(node), ret_buf};
}

uint8_t* COWBtreeNode::try_share_buf() {
    uint8_t* buf = to_btree_node()->get_phys_node_buf();

    bool expected = true;
    if (!m_is_buf_exclusive.compare_exchange_strong(expected, false)) {
        // The buffer is already shared, so we can't share it again.
        return nullptr;
    }
    return buf;
}

void COWBtreeNode::release_buf(uint8_t* buf) {
    auto is_exclusive = m_is_buf_exclusive.exchange(true);
    if (is_exclusive == true) {
        // Looks like the earlier buffer which was shared to us has been released and a new copy was used. So we
        // need to free the buffer
        hs_utils::iobuf_free(buf, sisl::buftag::btree_node);
    } else {
        HS_DBG_ASSERT_EQ(buf, to_btree_node()->get_phys_node_buf(), "Buffer is not same as the one we shared. buf={}",
                         fmt::ptr(buf));
    }
}

bool COWBtreeNode::copy_buf_if_needed(COWBtree const& bt, cp_id_t cur_cp_id) {
    BtreeNode* node = to_btree_node();
    bool copied{false};

    // If the buffer for the current version was written as part of previous cp (exactly 1 behind requested cp), then we
    // need to check if previous cp is still in flushing phase. If so, we have to make a copy and use new version to
    // write. We preserve existing version until it is flushed.
    auto const node_cp_id = node->get_modified_cp_id();
    if ((node_cp_id == (cur_cp_id - 1)) && !cp_mgr().has_cp_flushed(node_cp_id)) {
        // Do a deep copy and update the
        auto new_buf = hs_utils::iobuf_alloc(node->node_size(), sisl::buftag::btree_node, bt.align_size());
        std::memcpy(new_buf, node->get_phys_buf(), node->node_size());

        auto old_buf = m_copied_version_buf.exchange(new_buf);
        if (old_buf != nullptr) {
            // There is a very rare scenario where copied_version_buf is not nullptr. Scenario is that dirtying thread
            // has come to this condition where existing node_cp_id is 1 less than dirtying cp_id, so it needs to copy
            // the buffer and copied them. However, the flushing thread called get_flush_version_buf() and before
            // dirtying created a copied version of the buffer, able to pull the existing buffer (which would not have
            // been modified, so it is ok), then the copied version created by dirtied thread would not have been freed,
            // so we are freeing them now.
            hs_utils::iobuf_free(old_buf, sisl::buftag::btree_node);
        }
        copied = true;
    }
    node->set_modified_cp_id(cur_cp_id);
    return copied;
}

COWBtreeNodeBuffer COWBtreeNode::get_flush_version_buf(cp_id_t cp_id) {
    // NOTE: We ignore any buf from flushing if has been deleted after dirtying it in the same cp. This could be a
    // common case,  example in the same cp, following things could happen:
    // 1. Remove entries as part of normal key delete, but it is not minimal enough to cause a rebalance/merge of nodes.
    // 2. Later in same cp, it removed more entries and ended up merging with its left node, in that case it will be
    // deleted.
    //
    // Under these situations (which is common), COWBtree can safely skip the 1st step write.

    uint8_t* buf = to_btree_node()->get_phys_buf();
    if (BtreeNode::get_modified_cp_id(buf) != cp_id) {
        // We have dirtied the buffer as part of next cp_id, while we are yet to flush the prev_cp_id, so we should find
        // the version to flush in its m_copied_version_buf
        HS_REL_ASSERT_NE((void*)m_copied_version_buf.load(), nullptr,
                         "Node={} was modified by next cp_id, but flush of cp_id={} couldn't locate previous version "
                         "of node buf. refresh_node() has not been called?",
                         to_btree_node()->to_string(), cp_id);
        COWBtreeNodeBuffer cbuf{m_copied_version_buf.exchange(nullptr), true /* alloced */};
        HS_DBG_ASSERT_EQ(
            BtreeNode::get_modified_cp_id(cbuf.bytes()), cp_id,
            "We got the copied version buffer for node_id={} but it was not modified by this flushing cp_id",
            to_btree_node()->node_id());

        return BtreeNode::is_node_deleted(cbuf.bytes()) ? COWBtreeNodeBuffer(nullptr, false /* alloced */)
                                                        : std::move(cbuf);
    } else {
        // This is most likely case. If we come here, we got the correct buffer to flush. Note that it is ok if during
        // this time makes a deep copy of the buffer and create new buffer for phys_buf and move this buf to
        // flush_node_buf because we already got the correct buffer.
        return COWBtreeNodeBuffer((BtreeNode::is_node_deleted(buf) ? nullptr : buf), false /* allocated */);
    }
}

COWBtreeNodeBuffer::~COWBtreeNodeBuffer() {
    if (m_allocated && (m_buf != nullptr)) {
        // Was allocated by the copy of buffer, so need to free them
        hs_utils::iobuf_free(m_buf, sisl::buftag::btree_node);
    }
}
} // namespace homestore