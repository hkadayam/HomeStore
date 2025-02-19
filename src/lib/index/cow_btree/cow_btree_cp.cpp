#include <stack>
#include <unordered_map>

#include <homestore/checkpoint/cp_mgr.hpp>
#include "index/index_cp.hpp"
#include "index/wb_cache.hpp"
#include "common/homestore_assert.hpp"

namespace homestore {
COWBtreeCPCallbacks::COWBtreeCPCallbacks(COWBtreeStore* store) : m_bt_store{store} {}

std::unique_ptr< CPContext > COWBtreeCPCallbacks::on_switchover_cp(CP* cur_cp, CP* new_cp) {
    return std::make_unique< COWBtreeCPContext >(new_cp);
}

folly::Future< bool > COWBtreeCPCallbacks::cp_flush(CP* cp) {
    auto ctx = s_cast< COWBtreeCPContext* >(cp->context(cp_consumer_t::INDEX_SVC));
    return m_bt_store->async_cp_flush(ctx);
}

void COWBtreeCPCallbacks::cp_cleanup(CP* cp) {}

int COWBtreeCPCallbacks::cp_progress_percent() { return 100; }

/////////////////////// IndexCPContext section ///////////////////////////
COWBtreeCPContext::COWBtreeCPContext(CP* cp) : VDevCPContext(cp) {}

void COWBtreeCPContext::add_to_txn_journal(uint32_t index_ordinal, const IndexBufferPtr& parent_buf,
                                           const IndexBufferPtr& left_child_buf, const IndexBufferPtrList& created_bufs,
                                           const IndexBufferPtrList& freed_bufs) {
    auto record_size = txn_record::size_for_num_ids(created_bufs.size() + freed_bufs.size() + (left_child_buf ? 1 : 0) +
                                                    (parent_buf ? 1 : 0));
    std::unique_lock< iomgr::FiberManagerLib::mutex > lg{m_txn_journal_mtx};
    if (m_txn_journal_buf.bytes() == nullptr) {
        m_txn_journal_buf =
            std::move(sisl::io_blob_safe{std::max(sizeof(txn_journal), 512ul), 512, sisl::buftag::metablk});
        txn_journal* tj = new (m_txn_journal_buf.bytes()) txn_journal();
        tj->cp_id = id();
    }

    txn_journal* tj = r_cast< txn_journal* >(m_txn_journal_buf.bytes());
    if (m_txn_journal_buf.size() < tj->size + record_size) {
        m_txn_journal_buf.buf_realloc(m_txn_journal_buf.size() + std::max(tj->size + record_size, 512u), 512,
                                      sisl::buftag::metablk);
        tj = r_cast< txn_journal* >(m_txn_journal_buf.bytes());
    }

    {
        auto rec = tj->append_record(index_ordinal);
        if (parent_buf) {
            rec->append(op_t::parent_inplace, parent_buf->blkid());
            if (parent_buf->is_meta_buf()) { rec->is_parent_meta = 0x1; }
        }
        if (left_child_buf && (left_child_buf != parent_buf)) {
            rec->append(op_t::child_inplace, left_child_buf->blkid());
        }
        for (auto const& buf : created_bufs) {
            rec->append(op_t::child_new, buf->blkid());
        }
        for (auto const& buf : freed_bufs) {
            rec->append(op_t::child_freed, buf->blkid());
        }
    }
}

void COWBtreeCPContext::add_to_dirty_list(BtreeNodePtr const& node) {
    m_dirty_node_list.push_back(node);
    m_dirty_buf_count.increment(1);
}

bool COWBtreeCPContext::any_dirty_buffers() const { return !m_dirty_buf_count.testz(); }

void COWBtreeCPContext::prepare_flush_iteration() { m_dirty_buf_it = m_dirty_buf_list.begin(); }

std::optional< BtreeNodePtr > COWBtreeCPContext::next_dirty() {
    if (m_dirty_buf_it == m_dirty_buf_list.end()) { return std::nullopt; }
    BtreeNodePtr ret = *m_dirty_buf_it;
    ++m_dirty_buf_it;
    return ret;
}

std::string COWBtreeCPContext::to_string() {
    std::string str{fmt::format("COWBtreeCPContext cpid={} dirty_buf_count={} dirty_buf_list_size={}\n", m_cp->id(),
                                m_dirty_buf_count.get(), m_dirty_buf_list.size())};

    m_dirty_buf_list.foreach_entry(
        [&str](BtreeNodePtr node) { fmt::format_to(std::back_inserter(str), "{}", node->to_compact_string()); });
    return str;
}

std::map< BlkId, IndexBufferPtr > IndexCPContext::recover(sisl::byte_view sb) {
    txn_journal const* tj = r_cast< txn_journal const* >(sb.bytes());
    if (tj->cp_id != id()) {
        // On clean shutdown, cp_id would be lesser than the current cp_id, in that case ignore this sb
        HS_DBG_ASSERT_LT(tj->cp_id, id(), "Persisted cp in wb txn journal is more than current cp");
        return {};
    }
    HS_DBG_ASSERT_GT(tj->num_txns, 0, "Invalid txn_journal, num_txns is zero");
    HS_DBG_ASSERT_GT(tj->size, 0, "Invalid txn_journal, size of records is zero");

    std::map< BlkId, IndexBufferPtr > buf_map;
    uint8_t const* cur_ptr = r_cast< uint8_t const* >(tj) + sizeof(txn_journal);

    for (uint32_t t{0}; t < tj->num_txns; ++t) {
        txn_record const* rec = r_cast< txn_record const* >(cur_ptr);
        HS_DBG_ASSERT_GT(rec->total_ids(), 0, "Invalid txn_record, has no ids in it");

        process_txn_record(rec, buf_map);
        cur_ptr += rec->size();
    }

    return buf_map;
}

void IndexCPContext::process_txn_record(txn_record const* rec, std::map< BlkId, IndexBufferPtr >& buf_map) {
    auto cpg = cp_mgr().cp_guard();

    auto const rec_to_buf = [&buf_map, &cpg](txn_record const* rec, bool is_meta, BlkId const& bid,
                                             IndexBufferPtr const& up_buf) -> IndexBufferPtr {
        IndexBufferPtr buf;
        auto it = buf_map.find(bid);
        if (it == buf_map.end()) {
            if (is_meta) {
                superblk< index_table_sb > tmp_sb;
                buf = std::make_shared< MetaIndexBuffer >(tmp_sb);
            } else {
                buf = std::make_shared< IndexBuffer >(nullptr, bid);
            }

            [[maybe_unused]] auto [it2, happened] = buf_map.insert(std::make_pair(bid, buf));
            DEBUG_ASSERT(happened, "buf_map insert failed");

            buf->m_dirtied_cp_id = cpg->id();
            buf->m_index_ordinal = rec->index_ordinal;
        } else {
            buf = it->second;
        }

        if (up_buf) {
            DEBUG_ASSERT(((buf->m_up_buffer == nullptr) || (buf->m_up_buffer == up_buf)), "Inconsistent up buffer");
            auto real_up_buf = (up_buf->m_created_cp_id == cpg->id()) ? up_buf->m_up_buffer : up_buf;

#ifndef NDEBUG
            //  if (!is_sibling_link || (buf->m_up_buffer == real_up_buf)) { return buf;}
            //  Already linked with same buf or its not a sibling link to override
            bool found{false};
            for (auto const& dbuf : real_up_buf->m_down_buffers) {
                if (dbuf.lock() == buf) {
                    found = true;
                    break;
                }
            }
            if (found) { return buf; }
            real_up_buf->m_down_buffers.emplace_back(buf);
#endif

            if (buf->m_up_buffer != real_up_buf) {
                real_up_buf->m_wait_for_down_buffers.increment(1);
                buf->m_up_buffer = real_up_buf;
            }
        }
        return buf;
    };

    uint32_t cur_idx = 0;
    IndexBufferPtr parent_buf{nullptr};
    if (rec->has_inplace_parent) { parent_buf = rec_to_buf(rec, rec->is_parent_meta, rec->blk_id(cur_idx++), nullptr); }

    IndexBufferPtr inplace_child_buf{nullptr};
    if (rec->has_inplace_child) {
        inplace_child_buf = rec_to_buf(rec, false /* is_meta */, rec->blk_id(cur_idx++), parent_buf);
    }

    for (uint8_t idx{0}; idx < rec->num_new_ids; ++idx) {
        auto new_buf = rec_to_buf(rec, false /* is_meta */, rec->blk_id(cur_idx++),
                                  inplace_child_buf ? inplace_child_buf : parent_buf);
        new_buf->m_created_cp_id = cpg->id();
    }

    for (uint8_t idx{0}; idx < rec->num_freed_ids; ++idx) {
        auto freed_buf = rec_to_buf(rec, false /* is_meta */, rec->blk_id(cur_idx++),
                                    inplace_child_buf ? inplace_child_buf : parent_buf);
        freed_buf->m_node_freed = true;
    }
}

void IndexCPContext::txn_journal::log_records() const { LOGINFO("{}", to_string()); }

std::string IndexCPContext::txn_journal::to_string() const {
    std::string str = fmt::format("cp_id={}, num_txns={}, size={}", cp_id, num_txns, size);
    uint8_t const* cur_ptr = r_cast< uint8_t const* >(this) + sizeof(txn_journal);
    for (uint32_t t{0}; t < num_txns; ++t) {
        txn_record const* rec = r_cast< txn_record const* >(cur_ptr);
        fmt::format_to(std::back_inserter(str), "\n  {}: {}", t, rec->to_string());
        cur_ptr += rec->size();
    }
    return str;
}

std::string IndexCPContext::txn_record::to_string() const {
    auto add_to_string = [this](std::string& str, uint8_t& idx, uint8_t id_count) {
        if (id_count == 0) {
            fmt::format_to(std::back_inserter(str), "empty]");
        } else {
            for (uint8_t i{0}; i < id_count; ++i, ++idx) {
                fmt::format_to(std::back_inserter(str), "[chunk={}, blk={}],", ids[idx].second, ids[idx].first);
            }
            fmt::format_to(std::back_inserter(str), "]");
        }
    };

    std::string str = fmt::format("ordinal={}, parent=[{}], in_place_child=[{}]", index_ordinal, parent_id_string(),
                                  child_id_string(), num_new_ids, num_freed_ids);

    uint8_t idx = (has_inplace_parent == 0x1) ? 1 : 0 + (has_inplace_child == 0x1) ? 1 : 0;
    fmt::format_to(std::back_inserter(str), ", new_ids=[");
    add_to_string(str, idx, num_new_ids);

    fmt::format_to(std::back_inserter(str), ", freed_ids=[");
    add_to_string(str, idx, num_freed_ids);
    return str;
}
} // namespace homestore
