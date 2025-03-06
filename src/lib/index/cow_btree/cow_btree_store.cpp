#include <memory>
#include "index/cow_btree/cow_btree_store.h"
#include "index/cow_btree/cow_btree_node.h"
#include "index/cow_btree/cow_btree.h"
#include "index/cow_btree/cow_btree_cp.h"
#include "device/virtual_dev.hpp"

#ifdef _PRERELEASE
#include "common/crash_simulator.hpp"
#endif

namespace homestore {

static COWBtreeNode* to_cow_btree_node(BtreeNodePtr const& n) {
    return r_cast< COWBtreeNode* >(uintptr_cast(n.get()) - sizeof(COWBtreeNode));
}

static void cow_btree_node_constructor(BtreeNodePtr const& n) {
    new (uintptr_cast(n.get()) - sizeof(COWBtreeNode)) COWBtreeNode();
}

static void cow_btree_node_destructor(BtreeNode* n) {
    r_cast< COWBtreeNode* >(uintptr_cast(n) - sizeof(COWBtreeNode))->~COWBtreeNode();
}

static COWBtree* to_cow_btree(BtreeBase& btree) { return r_cast< COWBtree* >(btree.underlying_btree()); }
static COWBtree* to_cow_btree(Index* index) {
    return r_cast< COWBtree* >(s_cast< BtreeBase* >(index)->underlying_btree());
}

static std::vector< iomgr::io_fiber_t > start_flush_threads() {
    // Start WBCache flush threads
    struct Context {
        std::condition_variable cv;
        std::mutex mtx;
        uint32_t thread_cnt{0};
        std::vector< iomgr::io_fiber_t > cp_flush_fibers;
    };
    auto ctx = std::make_shared< Context >();

    auto const nthreads = HS_DYNAMIC_CONFIG(generic.btree_cp_flush_threads);
    for (uint32_t i{0}; i < nthreads; ++i) {
        iomanager.create_reactor("index_cp_flush" + std::to_string(i), iomgr::INTERRUPT_LOOP,
                                 HS_DYNAMIC_CONFIG(generic.btree_cp_flush_fibers_per_thread), [ctx](bool is_started) {
                                     if (is_started) {
                                         {
                                             auto fibers = iomanager.sync_io_capable_fibers();
                                             std::unique_lock< std::mutex > lk{ctx->mtx};
                                             ctx->cp_flush_fibers.insert(ctx->cp_flush_fibers.end(), fibers.begin(),
                                                                         fibers.end());
                                             ++(ctx->thread_cnt);
                                         }
                                         ctx->cv.notify_one();
                                     }
                                 });
    }

    {
        std::unique_lock< std::mutex > lk{ctx->mtx};
        ctx->cv.wait(lk, [ctx, nthreads] { return (ctx->thread_cnt == nthreads); });
    }
    return std::move(ctx->cp_flush_fibers);
}

COWBtreeStore::COWBtreeStore(shared< VirtualDev > vdev, std::vector< superblk< IndexStoreSuperBlock > > store_sbs,
                             shared< sisl::Evictor > evictor, uint32_t node_size) :
        m_cache{std::move(evictor), 100000, node_size,
                [](const BtreeNodePtr& node) -> bnodeid_t { return node->node_id(); },
                [](const sisl::CacheRecord& rec) -> bool {
                    const auto& hnode = (sisl::SingleEntryHashNode< BtreeNodePtr >&)rec;
                    return (hnode.m_value->m_refcount.test_le(1));
                }},
        m_vdev{std::move(vdev)},
        m_node_size{node_size},
        m_vdev_blks_per_node{node_size / m_vdev->block_size()} {
    if (store_sbs.size()) {
        // There can be multiple sbs, each sb containing a journal for a particular cp. We need to sort based on cp_id
        // and then split them as
        std::sort(store_sbs.begin(), store_sbs.end(), [](auto& lhs, auto& rhs) {
            return (r_cast< Journal* >(lhs.get())->cp_id < r_cast< Journal* >(rhs.get())->cp_id);
        });

        m_journals_by_cpid = std::move(store_sbs);
        for (auto& journal : m_journals_by_cpid) {
            load_journal(journal);
        }
    }
    m_cp_flush_fibers = std::move(start_flush_threads());
}

void COWBtreeStore::on_recovery_completed() {
    HS_DBG_ASSERT_EQ(m_journals_by_btree.size(), 0,
                     "Even after recovery is completed, there are some btree journals are yet to be loaded, perhaps "
                     "its index super block is missing?");

    // All btrees are loaded and recovery is completed. We can free up the journal buffers now. Note that we do not
    // free up the superblk itself which contains critical meta_cookie info to remove the journal record itself once we
    // do full map flush.
    for (auto& journal : m_journals_by_cpid) {
        journal.raw_buf().reset(); // This should free up the underlying byte_array only.
    }
}

unique< UnderlyingBtree > COWBtreeStore::on_btree_created(BtreeBase& btree, bool) {
    unique< COWBtree > cbtree;

    auto it = m_journals_by_btree.find(btree.ordinal());
    if (it == m_journals_by_btree.end()) {
        cbtree = std::make_unique< COWBtree >(&btree, m_vdev, std::vector< sisl::byte_view >{});
    } else {
        cbtree = std::make_unique< COWBtree >(&btree, m_vdev, std::move(it->second));
        m_journals_by_btree.erase(it); // We no longer need btree specific journal records after it is created.
    }
    return cbtree;
}

void COWBtreeStore::on_btree_destroyed(BtreeBase& bt) {
    to_cow_btree(bt)->on_btree_destroyed();
    hs()->index_service().remove_index_table_entry(bt.uuid());
}

BtreeNodePtr COWBtreeStore::create_node(BtreeBase& btree, bool is_leaf, void* context) {
    auto buf = hs_utils::iobuf_alloc(m_node_size, sisl::buftag::btree_node, m_vdev->align_size());
    auto n = BtreeNodePtr{btree.init_node(buf, to_cow_btree(btree)->generate_node_id(), true /* init_buf */, is_leaf,
                                          sizeof(COWBtreeNode))};
    new (uintptr_cast(n.get()) - sizeof(COWBtreeNode)) COWBtreeNode();

    // Add the node to the cache
    bool done = m_cache.insert(n);
    HS_REL_ASSERT_EQ(done, true, "Unable to add alloc'd node to cache, low memory or duplicate inserts?");

    return n;
}

btree_status_t COWBtreeStore::write_node(BtreeBase&, BtreeNodePtr const& node, void* context) {
    // All the required actions are performed during refresh_node with read_modify_write=true
    return btree_status_t::success;
}

btree_status_t COWBtreeStore::read_node(BtreeBase& btree, bnodeid_t node_id, BtreeNodePtr& node) {
retry:
    // Attempt to locate the node in the cache
    if (m_cache.get(node_id, node)) { return btree_status_t::success; }

    // Need to read from the blk, so check that in the map
    BlkId blkid = to_cow_btree(btree)->get_blkid_for_nodeid(node_id);
    if (!blkid.is_valid()) { return btree_status_t::not_found; }

    auto raw_buf = hs_utils::iobuf_alloc(m_node_size, sisl::buftag::btree_node, m_vdev->align_size());
    m_vdev->sync_read(r_cast< char* >(raw_buf), m_node_size, blkid);

    // Initialize the node
    node = BtreeNodePtr{btree.init_node(raw_buf, node_id, false /* init_buf*/, BtreeNode::identify_leaf_node(raw_buf),
                                        sizeof(COWBtreeNode))};
    cow_btree_node_constructor(node);

    // Add the node to the cache
    if (!m_cache.insert(node)) {
        // There is a race between 2 concurrent reads of same node, Re-read from cache again
        cow_btree_node_destructor(node.get());
        goto retry;
    }

    return btree_status_t::success;
}

btree_status_t COWBtreeStore::refresh_node(BtreeBase& bt, BtreeNodePtr const& node, bool for_read_modify_write,
                                           void* context) {
    if (context == nullptr || !for_read_modify_write) { return btree_status_t::success; }

    COWBtreeCPContext* cp_ctx = r_cast< COWBtreeCPContext* >(context);
    auto const mod_cp_id = node->get_modified_cp_id();
    auto const cur_cp_id = cp_ctx->id();
    if (mod_cp_id == cur_cp_id) {
        // For same cp, we don't need a copy, we can rewrite on the same buffer
        return btree_status_t::success;
    } else if (mod_cp_id > cur_cp_id) {
        return btree_status_t::cp_mismatch; // We are asked to provide the buffer of an older CP, which is not possible
    } else {
        COWBtree* cow_bt = to_cow_btree(bt);
        to_cow_btree_node(node)->copy_buf_if_needed(*cow_bt, cp_ctx->id());
        cow_bt->add_to_dirty_list(node, cp_ctx);
    }
    return btree_status_t::success;
}

void COWBtreeStore::remove_node(BtreeBase& bt, BtreeNodePtr const& node, void* context) {
    // Add the node id to dirty deleted list, which will be applied during the cp flush
    COWBtree* cow_bt = to_cow_btree(bt);

    COWBtreeCPContext* cp_ctx = r_cast< COWBtreeCPContext* >(context);
    cow_bt->add_to_remove_list(node->node_id(), cp_ctx);

    // Now we can remove the node from cache.
    BtreeNodePtr tmp;
    bool done = m_cache.remove(node->node_id(), tmp);
    HS_REL_ASSERT_EQ(done, true, "Race on cache removal of btree blkid?");
}

btree_status_t COWBtreeStore::transact_nodes(BtreeBase& bt, BtreeNodeList const& new_nodes,
                                             BtreeNodeList const& removed_nodes, BtreeNodePtr const& left_child_node,
                                             BtreeNodePtr const& parent_node, void* context) {
    for (const auto& node : new_nodes) {
        write_node(bt, node, context);
    }
    write_node(bt, left_child_node, context);
    write_node(bt, parent_node, context);

    for (const auto& node : removed_nodes) {
        remove_node(bt, node, context);
    }
    return btree_status_t::success;
}

btree_status_t COWBtreeStore::on_root_changed(BtreeBase& bt, BtreeNodePtr const& new_root, void* context) {
    // TODO: Need to update the metablk with correct root node
    to_cow_btree(bt)->on_root_changed(new_root, r_cast< COWBtreeCPContext* >(context));
    return btree_status_t::success;
}

void COWBtreeStore::on_node_freed(BtreeNode* node) { cow_btree_node_destructor(node); }

uint64_t COWBtreeStore::used_size(BtreeBase& btree) { return to_cow_btree(btree)->used_size(); }

folly::Future< bool > COWBtreeStore::async_cp_flush(COWBtreeCPContext* cp_ctx) {
    LOGTRACEMOD(btree, "Starting COWBtree CP Flush with cp context={}", cp_ctx->to_string());
    if (!cp_ctx->any_dirty_nodes()) {
        if (cp_ctx->id() == 0) {
            // For the first CP, we need to flush the journal buffer to the meta blk
            LOGINFO("First time boot cp, we shall flush the vdev to ensure all cp information is created");
            m_vdev->cp_flush(cp_ctx);
        } else {
            CP_PERIODIC_LOG(DEBUG, cp_ctx->id(), "Btree does not have any dirty buffers to flush");
        }
        return folly::makeFuture< bool >(true); // nothing to flush
    }

#ifdef _PRERELEASE
    if (hs()->crash_simulator().is_crashed()) {
        LOGINFOMOD(btree, "crash simulation is ongoing, so skip the cp flush");
        return folly::makeFuture< bool >(true);
    }
#endif

    // Get all the current btrees in the system.
    cp_ctx->m_all_btrees = std::move(hs()->index_service().get_all_index_tables());

    for (auto& fiber : m_cp_flush_fibers) {
        iomanager.run_on_forget(fiber, [this, cp_ctx]() {
            cp_ctx->m_flushing_fibers_count.increment(1);

            // Each thread will walk through all btrees created and alive at the point of CP flush and try to flush
            // their dirty nodes. We take this approach as against marking the dirtied btree seperately while dirtying
            // is that, we keep the code path of dirtying as waitfree as possible. It is more critical code path.
            // However, we pay the cost during the flushing by walking across all btrees and then check if they are
            // dirty. I feel this is much lower cost than doing in critical IO path.
            for (auto const& btree : cp_ctx->m_all_btrees) {
                COWBtree* cow_btree = to_cow_btree(btree.get());
                auto const [has_flushed, journal, is_sb_changed] = cow_btree->flush_nodes(cp_ctx);

                if (has_flushed) {
                    // This btree was dirtied in this cp, keep track of these btrees to persist their full map (if full
                    // map cp) or if superblk is changed.
                    // NOTE: We cannot persist superblk before persisting the journal that all btrees have been built.
                    // That is why we need to keep track of all btrees whose superblk has been changed and then write
                    // later.
                    std::unique_lock lg{cp_ctx->m_bt_list_mtx};
                    if (cp_ctx->need_full_map_flush()) {
                        cp_ctx->m_active_btree_list.emplace_back(cow_btree);
                    } else {
                        cp_ctx->m_merged_journal_buf.append(journal->m_base_buf);
                        if (is_sb_changed) { cp_ctx->m_active_btree_list.emplace_back(cow_btree); }
                    }
                }
            }

            if (cp_ctx->m_flushing_fibers_count.decrement_testz(1)) {
                // All dirty nodes from all btrees have been flushed, now we can flush the full map or journal
                // (depending on cp type) for each of the modified btree
                flush_map(cp_ctx);
            }
        });
    }
    return std::move(cp_ctx->get_future());
}

void COWBtreeStore::flush_map(COWBtreeCPContext* cp_ctx) {
    if (cp_ctx->need_full_map_flush()) {
        for (auto& fiber : m_cp_flush_fibers) {
            iomanager.run_on_forget(fiber, [this, cp_ctx]() {
                cp_ctx->m_flushing_fibers_count.increment(1);
                // Yes we access m_active_btree_list outside of lock, but we are sure that there is no one mutating this
                // btree list
                for (auto cow_btree : cp_ctx->m_active_btree_list) {
                    cow_btree->flush_map_and_sb(cp_ctx);
                }
                if (cp_ctx->m_flushing_fibers_count.decrement_testz(1)) {
                    // We just flushed the full bnode map of all btrees, we can remove all previous journal superblks
                    for (auto& journal : m_journals_by_cpid) {
                        journal.destroy();
                    }
                    cp_ctx->complete(true);
                }
            });
        }
    } else {
        auto sb = superblk< IndexStoreSuperBlock >{"index_store"};
        sb.load(cp_ctx->m_merged_journal_buf.view(), nullptr); // Load an empty meta_blk but with given buffer
        sb.write();                                     // Write the metablk
        sb.raw_buf().reset(); // after we wrote the superblk, we no longer need the merged journal buffer, free it
        m_journals_by_cpid.emplace_back(std::move(sb)); // Append to the end in the journal

        {
            std::unique_lock lg{cp_ctx->m_bt_list_mtx};
            for (auto cow_btree : cp_ctx->m_active_btree_list) {
                cow_btree->flush_sb(cp_ctx);
            }
        }
        cp_ctx->complete(true);
    }
}

void COWBtreeStore::load_journal(superblk< IndexStoreSuperBlock >& sb) {
    auto store_journal = r_cast< COWBtreeStore::Journal* >(sb.get());
    uint32_t cur_offset = sizeof(COWBtreeStore::Journal);

    for (uint32_t i{0}; i < store_journal->num_btrees; ++i) {
        COWBtree::Journal::Header* cur_bj = r_cast< COWBtree::Journal::Header* >(sb.get() + cur_offset);

        auto it = m_journals_by_btree.find(cur_bj->ordinal);
        if (it == m_journals_by_btree.end()) {
            bool happened;
            std::tie(it, happened) =
                m_journals_by_btree.insert(std::pair(cur_bj->ordinal, std::vector< sisl::byte_view >{}));
            HS_DBG_ASSERT(happened, "Insertion journal to journals list has failed for ordinal={}", cur_bj->ordinal);
        }
        it->second.emplace_back(sisl::byte_view{sb.raw_buf(), cur_offset, cur_bj->size});
        cur_offset += cur_bj->size;
    }
}

uint32_t COWBtreeStore::parallel_map_flushers_count() const {
    // We cannot have more parallel fibers flushing than max heads we can put in the btree superblk, because each fiber
    // will flush a portion of the full map and will have a head of the location chain.
    return std::min(uint32_cast(m_cp_flush_fibers.size()),
                    COWBtree::SuperBlock::max_map_heads(BtreeSuperBlock::underlying_btree_sb_size));
}
} // namespace homestore