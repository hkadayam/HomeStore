#include <homestore/checkpoint/cp_mgr.hpp>
#include "index/cow_btree/cow_btree_cp.h"
#include "index/cow_btree/cow_btree_store.h"

namespace homestore {
COWBtreeCPCallbacks::COWBtreeCPCallbacks(COWBtreeStore* store) : m_bt_store{store} {}

std::unique_ptr< CPContext > COWBtreeCPCallbacks::on_switchover_cp(CP* cur_cp, CP* new_cp) {
    return std::make_unique< COWBtreeCPContext >(new_cp->id(), m_bt_store->parallel_map_flushers_count());
}

folly::Future< bool > COWBtreeCPCallbacks::cp_flush(CP* cp) {
    auto ctx = s_cast< COWBtreeCPContext* >(cp->context(cp_consumer_t::INDEX_SVC));
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

} // namespace homestore
