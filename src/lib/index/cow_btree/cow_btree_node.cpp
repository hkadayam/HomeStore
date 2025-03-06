#include <homestore/btree/detail/btree_node.hpp>
#include "index/cow_btree/cow_btree_cp.h"
#include "index/cow_btree/cow_btree_node.h"
#include "index/cow_btree/cow_btree.h"
#include "common/homestore_utils.hpp"
#include "common/homestore_assert.hpp"

namespace homestore {

BtreeNode* COWBtreeNode::to_btree_node() { return r_cast< BtreeNode* >(uintptr_cast(this) + sizeof(COWBtreeNode)); }

COWBtreeNode::~COWBtreeNode() {
    if (m_prev_version_buf != nullptr) { hs_utils::iobuf_free(m_prev_version_buf, sisl::buftag::btree_node); }
}

bool COWBtreeNode::copy_buf_if_needed(COWBtree& bt, cp_id_t cur_cp_id) {
    BtreeNode* node = to_btree_node();

    // If the buffer for the current version was written as part of previous cp (exactly 1 behind requested cp), then we
    // need to check if previous cp is still in flushing phase. If so, we have to make a copy and use new version to
    // write. We preserve existing version until it is flushed.
    auto const mod_cp_id = node->get_modified_cp_id();
    if ((mod_cp_id == (cur_cp_id - 1)) && cp_mgr().is_cp_flushing(mod_cp_id)) {
        HS_DBG_ASSERT_EQ((void*)m_prev_version_buf, nullptr,
                         "We are copying the node={} to its prev version, but prev version={} has not been flushed "
                         "yet? or may be there is a race condition on getting write version of the node",
                         node->node_id(), node->to_string())

        // Do a deep copy and update the
        auto new_buf = hs_utils::iobuf_alloc(node->node_size(), sisl::buftag::btree_node, bt.align_size());
        std::memcpy(new_buf, node->get_phys_buf(), node->node_size());
        BtreeNode::set_modified_cp_id(new_buf, cur_cp_id);

        m_prev_version_buf = node->get_phys_buf();
        node->set_phys_buf(new_buf);
        return true;
    } else {
        node->set_modified_cp_id(cur_cp_id);
        return false;
    }
}

uint8_t* COWBtreeNode::get_flush_version_buf(cp_id_t cp_id) {
    uint8_t* buf;
    auto const mod_cp_id = to_btree_node()->get_modified_cp_id();
    if (mod_cp_id == cp_id) {
        buf = to_btree_node()->get_phys_buf();
    } else {
        // We have dirtied the buffer as part of next cp_id, get the previous version
        HS_REL_ASSERT_NE((void*)m_prev_version_buf, nullptr,
                         "Node={} was modified by next cp_id, but flush of cp_id={} couldn't locate previous version "
                         "of node buf. refresh_node() has not been called?",
                         to_btree_node()->to_string(), cp_id);
        buf = m_prev_version_buf;
    }

    // If node has been deleted after dirtying it in the same cp, ignore this buf from flushing
    return (BtreeNode::is_node_deleted(buf)) ? nullptr : buf;
}

void COWBtreeNode::reset_prev_version_buf(cp_id_t cp_id) {
    if (m_prev_version_buf != nullptr) {
        HS_DBG_ASSERT_EQ(BtreeNode::get_modified_cp_id(m_prev_version_buf), cp_id,
                         "We just flushed cp_id, but node={} prev version doesn't match", to_btree_node()->to_string());
        hs_utils::iobuf_free(m_prev_version_buf, sisl::buftag::btree_node);
        m_prev_version_buf = nullptr;
    }
}
} // namespace homestore