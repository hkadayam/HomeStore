/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once
#include <atomic>
#include <sisl/fds/concurrent_insert_vector.hpp>
#include <homestore/blk.h>
#include <homestore/checkpoint/cp_mgr.hpp>
#include <homestore/checkpoint/cp.hpp>
#include "device/virtual_dev.hpp"

namespace homestore {
class BtreeNode;

class COWBtreeStore;
class COWBtreeCPCallbacks : public CPCallbacks {
public:
    COWBtreeCPCallbacks(COWBtreeStore* store);
    virtual ~COWBtreeCPCallbacks() = default;

public:
    std::unique_ptr< CPContext > on_switchover_cp(CP* cur_cp, CP* new_cp) override;
    folly::Future< bool > cp_flush(CP* cp) override;
    void cp_cleanup(CP* cp) override;
    int cp_progress_percent() override;

private:
    COWBtreeStore* m_bt_store;
};

struct COWBtreeCPContext : public VDevCPContext {
public:
    sisl::atomic_counter< int64_t > m_dirty_node_count{0};
    sisl::atomic_counter< int64_t > m_removed_node_count{0};
    sisl::atomic_count< int64_t > m_flushing_fibers_count{0};
    uint32_t const m_parallel_flushers_count;

    iomgr::FiberManagerLib::shared_mutex m_bt_list_mtx;
    std::vector< shared< Index > > m_all_btrees;
    std::vector< COWBtree* > m_active_btree_list;
    sisl::buf_builder m_merged_journal_buf;

public:
    COWBtreeCPContext(CP* cp, uint32_t parallel_flushers_count) :
            VDevCPContext(cp), m_parallel_flushers_count{parallel_flushers_count} {}
    virtual ~COWBtreeCPContext() = default;
    bool need_full_map_flush() const;
    bool any_dirty_nodes() const { return (m_dirty_node_count.load() > 0) || (m_removed_node_count.load() > 0); }
};
} // namespace homestore
