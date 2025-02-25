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
    sisl::atomic_counter< int64_t > dirty_node_count_{0};
    sisl::atomic_counter< int64_t > removed_node_count_{0};
    std::unique_ptr< COWBtreeJournal > journal_;

public:
    COWBtreeCPContext(CP* cp) : VDevCPContext(cp) {}
    virtual ~COWBtreeCPContext() = default;
};

struct COWBtreeCPContext : public VDevCPContext {
public:
#pragma pack(1)
    using compact_blkid_t = std::pair< blk_num_t, chunk_num_t >;
    enum class op_t : uint8_t { child_new, child_freed, parent_inplace, child_inplace };
    struct txn_record {
        uint8_t has_inplace_parent : 1; // Do we have parent_id in the list of ids. It will be first
        uint8_t has_inplace_child : 1;  // Do we have child_id in the list of ids. It will be second
        uint8_t is_parent_meta : 1;     // Is the parent buffer a meta buffer
        uint8_t reserved1 : 5;
        uint8_t num_new_ids;
        uint8_t num_freed_ids;
        uint8_t reserved{0};
        uint32_t index_ordinal;
        compact_blkid_t ids[1]; // C++ std probhits 0 size array

        txn_record(uint32_t ordinal) :
                has_inplace_parent{0x0},
                has_inplace_child{0x0},
                is_parent_meta{0x0},
                num_new_ids{0},
                num_freed_ids{0},
                index_ordinal{ordinal} {}

        uint32_t total_ids() const {
            return (num_new_ids + num_freed_ids + ((has_inplace_parent == 0x1) ? 1 : 0) +
                    ((has_inplace_child == 0x1) ? 1 : 0));
        }

        uint32_t size() const { return sizeof(txn_record) + (total_ids() - 1) * sizeof(compact_blkid_t); }
        static uint32_t size_for_num_ids(uint8_t n) { return sizeof(txn_record) + (n - 1) * sizeof(compact_blkid_t); }

        uint32_t next_slot() const {
            return ((has_inplace_parent == 0x1) ? 1 : 0) + ((has_inplace_child == 0x1) ? 1 : 0) + num_new_ids +
                num_freed_ids;
        }

        void append(op_t op, BlkId const& blk) {
            auto const compact_blk = std::make_pair(blk.blk_num(), blk.chunk_num());
            auto const slot = next_slot();
            if (op == op_t::parent_inplace) {
                DEBUG_ASSERT(has_inplace_parent == 0x0, "Duplicate inplace parent in same txn record");
                DEBUG_ASSERT((has_inplace_child == 0x0) && (num_new_ids == 0) && (num_freed_ids == 0),
                             "Ordering of append is not correct");
                has_inplace_parent = 0x1;
            } else if (op == op_t::child_inplace) {
                DEBUG_ASSERT(has_inplace_child == 0x0, "Duplicate inplace child in same txn record");
                has_inplace_child = 0x1;
            } else if (op == op_t::child_new) {
                DEBUG_ASSERT_LT(num_new_ids, 0xff, "Too many new ids in txn record");
                ++num_new_ids;
            } else if (op == op_t::child_freed) {
                DEBUG_ASSERT_LT(num_freed_ids, 0xff, "Too many freed ids in txn record");
                ++num_freed_ids;
            } else {
                DEBUG_ASSERT(false, "Invalid op type");
            }
            ids[slot] = compact_blk;
        }

        BlkId blk_id(uint8_t idx) const {
            DEBUG_ASSERT_LT(idx, total_ids(), "Index out of bounds");
            return BlkId{ids[idx].first, (blk_count_t)1u, ids[idx].second};
        }

        std::string parent_id_string() const {
            return (has_inplace_parent == 0x1) ? fmt::format("chunk={}, blk={}", ids[0].second, ids[0].first) : "empty";
        }

        std::string child_id_string() const {
            auto const idx = (has_inplace_parent == 0x1) ? 1 : 0;
            return (has_inplace_child == 0x1) ? fmt::format("chunk={}, blk={}", ids[idx].second, ids[idx].first)
                                              : "empty";
        }

        std::string to_string() const;
    };

    struct txn_journal {
        cp_id_t cp_id;
        uint32_t num_txns{0};
        uint32_t size{sizeof(txn_journal)}; // Total size including this header

        struct append_guard {
            txn_journal* m_journal;
            txn_record* m_rec;
            append_guard(txn_journal* journal, uint32_t ordinal) : m_journal{journal} {
                m_rec = new (uintptr_cast(m_journal) + m_journal->size) txn_record(ordinal);
            }
            ~append_guard() { m_journal->size += m_rec->size(); }
            txn_record* operator->() { return m_rec; }
            txn_record& operator*() { return *m_rec; }
        };

        // Followed by index_txns records
        append_guard append_record(uint32_t ordinal) {
            ++num_txns;
            return append_guard(this, ordinal);
        }

        std::string to_string() const;
        void log_records() const;
    };
#pragma pack()

public:
    sisl::ConcurrentInsertVector< BtreeNodePtr > m_dirty_buf_list;
    sisl::atomic_counter< int64_t > m_dirty_buf_count{0};
    std::mutex m_flush_buffer_mtx;
    sisl::ConcurrentInsertVector< BtreeNodePtr >::iterator m_dirty_buf_it;

    iomgr::FiberManagerLib::mutex m_txn_journal_mtx;
    sisl::io_blob_safe m_txn_journal_buf;

public:
    COWBtreeCPContext(CP* cp);
    virtual ~COWBtreeCPContext() = default;

    sisl::io_blob_safe const& journal_buf() const { return m_txn_journal_buf; }

    void add_to_dirty_list(BtreeNodePtr const& node);
    bool any_dirty_buffers() const;
    void prepare_flush_iteration();
    std::optional< IndexBufferPtr > next_dirty();
    uint32_t next_n_dirty(uint32_t count, std::vector< BtreeNodePtr >& out_vec);
    std::string to_string();

private:
};

} // namespace homestore
