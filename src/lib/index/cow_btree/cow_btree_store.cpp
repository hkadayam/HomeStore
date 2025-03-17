#include <memory>
#include <homestore/btree/detail/btree_node.hpp>
#include "index/cow_btree/cow_btree_store.h"
#include "index/cow_btree/cow_btree_node.h"
#include "index/cow_btree/cow_btree.h"
#include "index/cow_btree/cow_btree_cp.h"
#include "index/index_cp.h"
#include "device/virtual_dev.hpp"

#ifdef _PRERELEASE
#include "common/crash_simulator.hpp"
#endif

namespace homestore {

static COWBtree* to_cow_btree(BtreeBase& btree) { return r_cast< COWBtree* >(btree.underlying_btree()); }

static COWBtree const* to_cow_btree(BtreeBase const& btree) {
    return r_cast< COWBtree const* >(btree.underlying_btree());
}

static COWBtree* to_cow_btree(Index* index) {
    return r_cast< COWBtree* >(s_cast< BtreeBase* >(index)->underlying_btree());
}

static COWBtree const* to_cow_btree(Index const* index) {
    return r_cast< COWBtree const* >(s_cast< BtreeBase const* >(index)->underlying_btree());
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

COWBtreeStore::COWBtreeStore(shared< VirtualDev > vdev, std::vector< superblk< IndexStoreSuperBlock > > store_sbs) :
        m_vdev{std::move(vdev)} {
    // Register ourselves to the IndexCPCallbacks
    r_cast< IndexCPCallbacks* >(cp_mgr().get_consumer(cp_consumer_t::INDEX_SVC))
        ->register_consumer(IndexStore::Type::COPY_ON_WRITE_BTREE, std::make_unique< COWBtreeCPCallbacks >(this));

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

uint32_t COWBtreeStore::max_node_size() const { return m_vdev->atomic_page_size(); }
uint32_t COWBtreeStore::align_size() const { return m_vdev->align_size(); }

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

unique< UnderlyingBtree > COWBtreeStore::on_btree_created(BtreeBase& btree, bool load_existing) {
    unique< COWBtree > cbtree;

    auto it = m_journals_by_btree.find(btree.ordinal());
    if (it == m_journals_by_btree.end()) {
        HS_DBG_ASSERT_EQ(load_existing, false, "Btree is asked to load, but its journal is missing");
        cbtree = std::make_unique< COWBtree >(btree, m_vdev, std::vector< sisl::byte_view >{}, load_existing);
    } else {
        HS_DBG_ASSERT_EQ(load_existing, true, "Btree is found, but we are asked to create a new one");
        cbtree = std::make_unique< COWBtree >(btree, m_vdev, std::move(it->second), load_existing);
        m_journals_by_btree.erase(it); // We no longer need btree specific journal records after it is created.
    }
    return cbtree;
}

void COWBtreeStore::on_btree_destroyed(BtreeBase& bt) {
    CPGuard cpg = cp_mgr().cp_guard();
    auto context = cpg->context(cp_consumer_t::INDEX_SVC);
    auto cp_ctx = IndexCPContext::convert< COWBtreeCPContext >(context, IndexStore::Type::COPY_ON_WRITE_BTREE);

    {
        std::unique_lock lg{cp_ctx->m_bt_list_mtx};
        cp_ctx->m_destroyed_btrees.emplace_back(bt.shared_from_this());
    }
}

void COWBtreeStore::on_node_freed(BtreeNode* node) { COWBtreeNode::destruct(node); }

class CPFlushGuard {
public:
    CPFlushGuard(COWBtreeCPContext* ctx, std::function< void(COWBtreeCPContext* cp_ctx) > done_cb) :
            m_cp_ctx{ctx}, m_done_cb{std::move(done_cb)} {
        ctx->m_flushing_fibers_count.increment(1);
    }

    ~CPFlushGuard() {
        if (m_cp_ctx->m_flushing_fibers_count.decrement_testz(1)) { m_done_cb(m_cp_ctx); }
    }

    CPFlushGuard(CPFlushGuard const& other) {
        m_cp_ctx = other.m_cp_ctx;
        m_done_cb = other.m_done_cb;
        m_cp_ctx->m_flushing_fibers_count.increment(1);
    }

    CPFlushGuard(CPFlushGuard&& other) = delete;

    CPFlushGuard operator=(CPFlushGuard const& other) {
        m_cp_ctx = other.m_cp_ctx;
        m_done_cb = other.m_done_cb;
        m_cp_ctx->m_flushing_fibers_count.increment(1);
        return *this;
    }

    CPFlushGuard operator=(CPFlushGuard&& other) = delete;

    COWBtreeCPContext* cp_ctx() { return m_cp_ctx; }

private:
    COWBtreeCPContext* m_cp_ctx;
    std::function< void(COWBtreeCPContext* ctx) > m_done_cb;
};

folly::Future< bool > COWBtreeStore::async_cp_flush(COWBtreeCPContext* cp_ctx) {
    LOGTRACEMOD(btree, "Starting COWBtree CP Flush with cp context={}", cp_ctx->to_string());
    if (!cp_ctx->any_dirty_nodes()) {
        if (cp_ctx->id() == 0) {
            // For the first CP, we need to flush the journal buffer to the meta blk
            // LOGINFO("First time boot cp, we shall flush the vdev to ensure all cp information is created");
            // m_vdev->cp_flush(cp_ctx);
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

    // Prepare the header for the journal to be written. The header details will be filled along the way while flushing
    cp_ctx->prepare_store_journal();

    // Get all the current btrees in the system.
    cp_ctx->m_all_btrees = std::move(hs()->index_service().get_all_index_tables());
    auto on_flush_nodes_done = [this](COWBtreeCPContext* cp_ctx) {
        // If there are any destroyed btrees as part of the CP, do the actual destroy now.
        for (auto& btree : cp_ctx->m_destroyed_btrees) {
            to_cow_btree(btree.get())->destroy();
        }

        // All dirty nodes from all btrees have been flushed, now we can flush the full map or journal
        // (depending on cp type) for each of the modified btree
        flush_map(cp_ctx);
    };

    CPFlushGuard fg{cp_ctx, on_flush_nodes_done};
    for (auto& fiber : m_cp_flush_fibers) {
        iomanager.run_on_forget(fiber, [fg]() mutable {
            // Each thread will walk through all btrees created and alive at the point of CP flush and try to flush
            // their dirty nodes. We take this approach as against marking the dirtied btree seperately while dirtying
            // is that, we keep the code path of dirtying as waitfree as possible. It is more critical code path.
            // However, we pay the cost during the flushing by walking across all btrees and then check if they are
            // dirty. I feel this is much lower cost than doing in critical IO path.
            auto cp_ctx = fg.cp_ctx();
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
                        cp_ctx->append_btree_journal(journal->m_base_buf);
                        if (is_sb_changed) { cp_ctx->m_active_btree_list.emplace_back(cow_btree); }
                    }
                }
            }
        });
    }

    return std::move(cp_ctx->get_future());
}

void COWBtreeStore::flush_map(COWBtreeCPContext* cp_ctx) {
    if (cp_ctx->need_full_map_flush()) {
        auto on_flush_map_done = [this](COWBtreeCPContext* cp_ctx) {
            // We just flushed the full bnode map of all btrees, we can remove all previous journal
            // superblks
            for (auto& journal : m_journals_by_cpid) {
                journal.destroy();
            }
            cp_ctx->complete(true);
        };

        CPFlushGuard fg{cp_ctx, on_flush_map_done};
        for (auto& fiber : m_cp_flush_fibers) {
            iomanager.run_on_forget(fiber, [fg]() mutable {
                auto cp_ctx = fg.cp_ctx();

                // Yes we access m_active_btree_list outside of lock, but we are sure that there is no one mutating this
                // btree list
                for (auto cow_btree : cp_ctx->m_active_btree_list) {
                    cow_btree->flush_map_and_sb(cp_ctx);
                }
            });
        }
    } else {
        auto sb = superblk< IndexStoreSuperBlock >{"index_store"};
        sb.load(cp_ctx->store_journal(), nullptr); // Load an empty meta_blk but with given buffer
        sb.write();                                // Write the metablk
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