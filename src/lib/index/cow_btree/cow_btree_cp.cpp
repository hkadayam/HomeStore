#include <homestore/checkpoint/cp_mgr.hpp>
#include "index/cow_btree/cow_btree_cp.h"
#include "index/cow_btree/cow_btree_store.h"
#include "index/index_cp.h"

namespace homestore {
COWBtreeCPCallbacks::COWBtreeCPCallbacks(COWBtreeStore* store) : m_bt_store{store} {}

std::unique_ptr< CPContext > COWBtreeCPCallbacks::on_switchover_cp(CP* cur_cp, CP* new_cp) {
    return std::make_unique< COWBtreeCPContext >(new_cp, m_bt_store->parallel_map_flushers_count(),
                                                 m_bt_store->align_size());
}

folly::Future< bool > COWBtreeCPCallbacks::cp_flush(CP* cp) {
    auto ctx = IndexCPContext::store_context< COWBtreeCPContext >(cp, IndexStore::Type::COPY_ON_WRITE_BTREE);
    return m_bt_store->async_cp_flush(ctx);
}

void COWBtreeCPCallbacks::cp_cleanup(CP* cp) {}

int COWBtreeCPCallbacks::cp_progress_percent() { return 100; }

/////////////////////// COWBtreeCPContext section ///////////////////////////
bool COWBtreeCPContext::need_full_map_flush() const {
    // TODO: Fill with approp details to check if the resource manager reports that metablk is running out of space or
    // if we have accumulated more than enough journal entries for the map
    return false;
}

std::string COWBtreeCPContext::to_string() const {
    // TODO: Fill with approp details
    return std::string();
}

void COWBtreeCPContext::prepare_store_journal() {
    COWBtreeStore::Journal hdr_sb;
    hdr_sb.cp_id = id();
    hdr_sb.index_store_type = IndexStore::Type::COPY_ON_WRITE_BTREE;

    m_merged_journal_buf.append(sisl::blob{uintptr_cast(&hdr_sb), uint32_cast(sizeof(COWBtreeStore::Journal))});
    m_journal_header = r_cast< COWBtreeStore::Journal* >(m_merged_journal_buf.bytes());
}

void COWBtreeCPContext::append_btree_journal(sisl::io_blob_safe const& btree_journal_buf) {
    ++m_journal_header->num_btrees;
    m_journal_header->size += btree_journal_buf.size();
    m_merged_journal_buf.append(btree_journal_buf);
}

sisl::byte_view COWBtreeCPContext::store_journal() const { return m_merged_journal_buf.view(); }

} // namespace homestore
