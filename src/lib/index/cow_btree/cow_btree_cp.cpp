#include <stack>
#include <unordered_map>

#include <homestore/checkpoint/cp_mgr.hpp>
#include "index/index_cp.hpp"
#include "index/wb_cache.hpp"
#include "common/homestore_assert.hpp"

namespace homestore {
COWBtreeCPCallbacks::COWBtreeCPCallbacks(COWBtreeStore* store) : m_bt_store{store} {}

std::unique_ptr< CPContext > COWBtreeCPCallbacks::on_switchover_cp(CP* cur_cp, CP* new_cp) {
    return std::make_unique< COWBtreeCPContext >(new_cp->cp_id(), m_bt_store->parallel_flushers_count());
}

folly::Future< bool > COWBtreeCPCallbacks::cp_flush(CP* cp) {
    auto ctx = s_cast< COWBtreeCPContext* >(cp->context(cp_consumer_t::INDEX_SVC));
    return m_bt_store->async_cp_flush(ctx);
}

void COWBtreeCPCallbacks::cp_cleanup(CP* cp) {}

int COWBtreeCPCallbacks::cp_progress_percent() { return 100; }

/////////////////////// COWBtreeCPContext section ///////////////////////////
bool COWBtreeCPContext::need_full_map_flush() const {}

} // namespace homestore
