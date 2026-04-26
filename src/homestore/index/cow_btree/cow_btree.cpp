#include <cstring>

#include <sisl/fds/buffer.h>

#include <homestore/index/btree/detail/btree_node.h>
#include <homestore/index/btree/btree_base.h>

#include "base/homestore_config.hpp"
#include "base/homestore_utils.hpp"
#include "blob/append_blk_stream.h"
#include "blob/append_byte_stream.h"
#include "blob/blob_dev.h"
#include "blob/raw_blk_stream.h"
#include "index/cow_btree/cow_btree.h"

namespace homestore {

using sisl::IOBuffer;

#define COWBT_LOG(level, msg, ...) SPECIFIC_BT_LOG(level, (*base_btree_), msg, ##__VA_ARGS__)
#define COWBT_CP_LOG(level, cp_id, msg, ...) SPECIFIC_BT_LOG(level, (*base_btree_), "[cp={}] " msg, cp_id, ##__VA_ARGS__)

// bnodeid_t layout: [ordinal(31 bits)][overflow_bit(1 bit)][node_number(32 bits)]
// Overflow bit is bit 32 (the lowest bit of the upper half).
static constexpr uint64_t btree_nodeid_bits = 32;
static constexpr uint64_t btree_nodeid_mask = (1ull << btree_nodeid_bits) - 1;
static constexpr uint64_t btree_overflow_bit = 1ull << btree_nodeid_bits; // bit 32
static constexpr uint64_t btree_ordinal_shift = btree_nodeid_bits + 1;    // bits 33-63

static inline COWBtree::CompactNodeId to_compact_nodeid(bnodeid_t node_id) {
    return to_u32(node_id & btree_nodeid_mask);
}

static inline bool is_overflow_node(bnodeid_t node_id) {
    return (node_id & btree_overflow_bit) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

COWBtreeSuperBlock const& COWBtree::super_blk() const {
    return *r_cast< COWBtreeSuperBlock const* >(mblk_.meta_blk().inline_data());
}

COWBtreeSuperBlock& COWBtree::mutable_super_blk() {
    return *r_cast< COWBtreeSuperBlock* >(mblk_.meta_blk().inline_data());
}

folly::coro::Task< shared< COWBtree > > COWBtree::create(COWBtreeManager& mgr, shared< BlobDev > blob_dev,
                                                         MetaBlkWrapper&& mblk, shared< NodeCache > node_cache,
                                                         shared< OverflowCache > overflow_cache) {
    auto node_s = co_await blob_dev->create_append_blk_stream(HS_DYNAMIC_CONFIG(btree->cow_node_chunk_size));
    auto overflow_s = co_await blob_dev->create_raw_blk_stream(HS_DYNAMIC_CONFIG(btree->cow_overflow_chunk_size));
    auto incr_map_s =
        co_await blob_dev->create_append_byte_stream(HS_DYNAMIC_CONFIG(btree->cow_incr_map_chunk_size), false);
    auto full_map_1 =
        co_await blob_dev->create_append_byte_stream(HS_DYNAMIC_CONFIG(btree->cow_full_map_chunk_size), false);
    auto full_map_2 =
        co_await blob_dev->create_append_byte_stream(HS_DYNAMIC_CONFIG(btree->cow_full_map_chunk_size), false);

    auto& sb = *r_cast< COWBtreeSuperBlock* >(mblk.meta_blk().inline_data());
    sb.node_stream_id = node_s->stream_id();
    sb.overflow_stream_id = overflow_s->stream_id();
    sb.incr_map_stream_id = incr_map_s->stream_id();
    sb.full_map_stream_ids[0] = full_map_1->stream_id();
    sb.full_map_stream_ids[1] = full_map_2->stream_id();
    sb.last_full_map_cp_id = -1;

    co_await mblk.write(to_cu8ptr(&sb), sizeof(COWBtreeSuperBlock));

    // Fresh streams — incr_map is empty, no accounting seed needed.
    co_return shared< COWBtree >(
        new COWBtree(mgr, std::move(blob_dev), std::move(mblk), std::move(node_cache), std::move(overflow_cache)));
}

folly::coro::Task< shared< COWBtree > > COWBtree::load(COWBtreeManager& mgr, shared< BlobDev > blob_dev,
                                                       MetaBlkWrapper&& mblk, shared< NodeCache > node_cache,
                                                       shared< OverflowCache > overflow_cache) {
    auto cow = shared< COWBtree >(
        new COWBtree(mgr, std::move(blob_dev), std::move(mblk), std::move(node_cache), std::move(overflow_cache)));
    co_await cow->recover();

    // Seed the manager-level incr_map accumulator with bytes already on disk so the threshold check is accurate
    // immediately after recovery.
    mgr.incr_map_appended(cow->incr_map_stream_->tail_offset());
    co_return cow;
}

COWBtree::COWBtree(COWBtreeManager& mgr, shared< BlobDev > blob_dev, MetaBlkWrapper&& mblk,
                   shared< NodeCache > node_cache, shared< OverflowCache > overflow_cache) :
        mgr_{mgr},
        blob_dev_{std::move(blob_dev)},
        node_cache_{std::move(node_cache)},
        overflow_cache_{std::move(overflow_cache)},
        nodeid_generator_{std::numeric_limits< uint32_t >::max()},
        mblk_{std::move(mblk)},
        btree_ordinal_{super_blk().ordinal},
        ordinal_shifted_{to_u64(super_blk().ordinal) << btree_ordinal_shift} {
    auto const& sb = super_blk();
    node_stream_ = blob_dev_->get_append_blk_stream(sb.node_stream_id);
    overflow_stream_ = blob_dev_->get_raw_blk_stream(sb.overflow_stream_id);
    incr_map_stream_ = blob_dev_->get_append_byte_stream(sb.incr_map_stream_id);
    incr_map_stream_->set_concurrent_safe(false);
    full_map_streams_[0] = blob_dev_->get_append_byte_stream(sb.full_map_stream_ids[0]);
    full_map_streams_[0]->set_concurrent_safe(false); // Streams are written during cp_flush and no concurrent cps
    full_map_streams_[1] = blob_dev_->get_append_byte_stream(sb.full_map_stream_ids[1]);
    full_map_streams_[1]->set_concurrent_safe(false);

    for (auto& session : cp_sessions_) {
        session = std::make_unique< CPSession >(*this);
    }
}

folly::coro::Task< void > COWBtree::destroy() {
    bnodeid_map_.clear();

    // Release all chunks held by each stream back to the underlying VDev. Cache entries (node_cache_,
    // overflow_cache_) become stale and will be evicted naturally — no explicit invalidation needed.
    co_await node_stream_->destroy();
    co_await overflow_stream_->destroy();
    co_await incr_map_stream_->destroy();
    co_await full_map_streams_[0]->destroy();
    co_await full_map_streams_[1]->destroy();

    // Drop the per-btree MetaBlk holding the COWBtreeSuperBlock.
    co_await mblk_.meta_client()->remove_meta_blk(mblk_.meta_blk());

    for (auto& session : cp_sessions_) {
        session->finish();
    }
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// UnderlyingBtree interface
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr< uint8_t > COWBtree::allocate_node_buf() {
    return sisl::AlignedSharedPtr< uint8_t, sisl::Buftag::btree_node >::make_sized(base_btree_->node_size(),
                                                                                   node_stream_->block_size());
}

Node COWBtree::create_node(bool is_leaf) {
    bnodeid_t const id = generate_node_id();

    auto buf = allocate_node_buf();
    auto core = base_btree_->construct_fresh_node(std::move(buf), id, is_leaf);

    auto cache_handle = node_cache_->insert(id, std::move(core), sisl::CacheHint::HOT);
    HS_REL_ASSERT(cache_handle, "create_node: cache insert failed for node_id={}", id);
    COWNodeHandle handle{std::move(cache_handle)};
    return Node::construct_new(handle, LockType::Write);
}

BtreeResult< Node > COWBtree::read_node(bnodeid_t id, LockType lock_type) {
    if (auto h = node_cache_->find(id); h) {
        COWNodeHandle handle{std::move(h)};
        CO_RETURN CO_AWAIT Node::construct_existing(handle, lock_type);
    }

    // Get the mapping of nodeid -> blkid
    BlkId const blkid = get_blkid_for_nodeid(id);
    HS_REL_ASSERT(blkid.is_valid(), "read_node: no BlkId found for node_id={}", id);

    // Read blkid from the stream
    IOBuffer io_buf{base_btree_->node_size(), node_stream_->block_size(), sisl::Buftag::btree_node};
    auto ec = CO_AWAIT node_stream_->read(io_buf, blkid);
    if (ec) {
        COWBT_LOG(ERROR, "read_node: stream read failed for node_id={} blkid={} ec={}", id, blkid.to_string(),
                  ec.message());
        CO_RETURN folly::makeUnexpected(BtreeStatus::node_read_failed);
    }

    auto core = base_btree_->construct_existing_node(io_buf.release_to_shared_ptr(), id);
    auto h = node_cache_->insert(id, std::move(core), sisl::CacheHint::COLD);
    if (!h) {
        // Somebody beat us in concurrently reading the blk, just read it from cache again
        h = node_cache_->find(id);
        HS_REL_ASSERT(h, "read_node: cache insert and find both failed for node_id={}", id);
    }

    COWNodeHandle handle{std::move(h)};
    auto node = CO_AWAIT Node::construct_existing(handle, lock_type);
    if (node.lock_type() == LockType::Write) {
        auto status = prepare_for_write(node);
        if (status != BtreeStatus::success) {
            CO_RETURN folly::makeUnexpected(status);
        }
    }
    CO_RETURN node;
}

void COWBtree::write_node(const Node& node) {
    HS_DBG_ASSERT_EQ(node.lock_type(), LockType::Write, "write_node called without write lock");
}

BtreeStatus COWBtree::prepare_for_write(const Node& node) {
    // Called under write lock just before mutating a node.
    // In COW semantics, every mutation produces a new version; we track the node as dirty
    // in the current CP session so flush_nodes() will append it to the AppendBlkStream.
    HS_DBG_ASSERT_EQ(node.lock_type(), LockType::Write, "prepare_for_write called without write lock");
    NodeCore& core = *node.operator->();

    CPGuard cpg = cp_mgr().cp_guard();
    auto const cur_cp_id = cpg->id();

    auto const mod_cp_id = core.get_modified_cp_id();
    if (mod_cp_id == cur_cp_id) {
        // Already marked dirty in this CP — reuse the same buffer.
        return BtreeStatus::success;
    }

    if (mod_cp_id > cur_cp_id) {
        // A newer cp has already modified this node, it can happen while this thread entered older CP and while its
        // processing cp switchover has happened and newer CP end up racing ahead and modified this node. We simply
        // should ask at the top of btree to just retry which will automatically enter into the new CP.
        return BtreeStatus::retry;
    }

    // If the buffer is still held by a previous CP's flush pipeline (use_count > 1), the flush thread could be reading
    // those bytes right now.  We must copy before the caller mutates.  After flush completes and FlushNodeEntry is
    // destroyed (in CPSession::finish()), the old shared_ptr ref drops and future prepare_for_write calls will see
    // use_count == 1 again — no unnecessary copies.
    auto buf = core.get_phys_buf();
    if (buf.use_count() > 1) {
        auto new_buf = sisl::AlignedSharedPtr< uint8_t, sisl::Buftag::btree_node >::make_sized(
            core.node_size(), node_stream_->block_size());
        std::memcpy(new_buf.get(), buf.get(), core.node_size());
        core.set_phys_buf(std::move(new_buf));
        buf = core.get_phys_buf();
    }

    // Pin the node in cache so the evictor can't reclaim it while dirty.
    auto handle = node_cache_->find(core.node_id());
    HS_REL_ASSERT(handle, "prepare_for_write: node_id={} not in cache", core.node_id());

    core.set_modified_cp_id(cur_cp_id);
    add_to_dirty_node_list(FlushNodeEntry{std::move(handle), buf}, cur_cp_id);
    return BtreeStatus::success;
}

void COWBtree::remove_node(const Node& node) {
    HS_DBG_ASSERT_EQ(node.lock_type(), LockType::Write, "remove_node called without write lock");
    auto const id = node->node_id();

    node_cache_->remove(id);
    add_to_remove_node_list(id);
}

void COWBtree::on_root_changed(const Node& root) {
    mutable_super_blk().root_node_id = root->node_id();

    CPGuard cpg = cp_mgr().cp_guard();
    cp_session(cpg->id())->new_root_id_.store(root->node_id());
}

uint64_t COWBtree::space_occupied() const {
    return bnodeid_map_.size() * base_btree_->node_size();
}

// ──────────────────────────────────────────────────────────────────────────────────────────────────
// Overflow support (RawBlkStream + overflow_cache_)
//
// write: alloc BlkId, copy data into IOBuffer, insert into cache + dirty list. No disk I/O.
// read:  cache hit → return immediately. Cache miss → co_await stream read, populate cache.
// delete: evict from cache, add BlkId to deleted list. Actual invalidate happens at flush time.
// ──────────────────────────────────────────────────────────────────────────────────────────────────

BtreeStatus COWBtree::write_overflow(const sisl::ByteArray& buf, BlkId& out_blkid) {
    auto const blk_sz = overflow_stream_->block_size();
    auto const nblks = s_cast< blk_count_t >((buf->size() + blk_sz - 1) / blk_sz);

    // Allocate blks in overflow stream
    auto status = overflow_stream_->alloc_blk(nblks, blk_alloc_hints{}, out_blkid);
    if (status != BlkAllocStatus::SUCCESS) {
        return BtreeStatus::space_not_avail;
    }

    // Insert into cache; the returned CacheHandle pins the entry so the evictor can't reclaim it while dirty.
    auto handle = overflow_cache_->insert(out_blkid, OverflowEntry{out_blkid, buf}, sisl::CacheHint::COLD);
    HS_REL_ASSERT(handle, "write_overflow: cache insert failed for blkid={}", out_blkid.to_string());
    add_to_dirty_overflow_list(std::move(handle));

    return BtreeStatus::success;
}

BtreeTask< BtreeStatus > COWBtree::read_overflow(const BlkId& blkid, sisl::ByteArray& out_buf) const {
    if (auto h = overflow_cache_->find(blkid); h) {
        out_buf = h.value().buf;
        CO_RETURN BtreeStatus::success;
    }

    auto const blk_sz = overflow_stream_->block_size();
    auto ba = sisl::make_byte_array(blkid.blk_count() * blk_sz, blk_sz, sisl::Buftag::btree_node);
    auto ec = CO_AWAIT overflow_stream_->read(*ba, blkid);
    if (ec) {
        CO_RETURN BtreeStatus::node_read_failed;
    }

    overflow_cache_->insert(blkid, OverflowEntry{blkid, ba}, sisl::CacheHint::COLD);
    out_buf = std::move(ba);
    CO_RETURN BtreeStatus::success;
}

BtreeStatus COWBtree::delete_overflow(const BlkId& blkid) {
    overflow_cache_->remove(blkid);
    add_to_remove_overflow_list(blkid);
    return BtreeStatus::success;
}

// ─────────────────────────────────────────────────────────────────────────────
// COWBtree-specific helpers
// ─────────────────────────────────────────────────────────────────────────────

bnodeid_t COWBtree::generate_node_id(bool is_overflow) {
    std::unique_lock lg{id_mtx_};
    return ordinal_shifted_ | (is_overflow ? btree_overflow_bit : 0) | nodeid_generator_.reserve();
}

void COWBtree::release_node_id(bnodeid_t nodeid) {
    std::unique_lock lg{id_mtx_};
    nodeid_generator_.unreserve(nodeid & btree_nodeid_mask);
}

BlkId COWBtree::get_blkid_for_nodeid(bnodeid_t nodeid) const {
    return bnodeid_map_.lookup(to_compact_nodeid(nodeid));
}

void COWBtree::add_to_dirty_node_list(FlushNodeEntry entry, cp_id_t cp_id) {
    cp_session(cp_id)->modified_nodes_.emplace_back(std::move(entry));
}

void COWBtree::add_to_remove_node_list(bnodeid_t node_id) {
    CPGuard cpg = cp_mgr().cp_guard();
    cp_session(cpg->id())->deleted_nodes_.push_back(to_compact_nodeid(node_id));
}

void COWBtree::add_to_dirty_overflow_list(sisl::CacheHandle< OverflowEntry >&& handle) {
    CPGuard cpg = cp_mgr().cp_guard();
    cp_session(cpg->id())->dirty_overflow_blks_.emplace_back(std::move(handle));
}

void COWBtree::add_to_remove_overflow_list(const BlkId& blkid) {
    CPGuard cpg = cp_mgr().cp_guard();
    cp_session(cpg->id())->deleted_overflow_blks_.push_back(blkid);
}

// ─────────────────────────────────────────────────────────────────────────────
//       CP and Flushes Section
// ─────────────────────────────────────────────────────────────────────────────

template < typename OnNodeFlushed >
folly::coro::Task< void > flush_dirty_nodes(COWBtree& bt, CP* cp, OnNodeFlushed&& on_flushed) {
    auto executor = co_await folly::coro::co_current_executor;
    std::vector< folly::SemiFuture< folly::Unit > > flush_futs;
    auto* session = bt.cp_session(cp->id());

    // Write all dirty nodes to node_stream_, update bnode_map, call on_node per entry.
    // We update bnode_map before confirming the write landed on disk — safe because FlushNodeEntry cache handles keep
    // nodes pinned so reads hit the cache.
    for (auto& node : session->modified_nodes_) {
        sisl::ByteArray buf = std::move(node.flush_buf);
        auto bid = bt.node_stream_->quick_append(cp, /*segment_id=*/0, buf);
        if (!bid) {
            flush_futs.push_back(bt.node_stream_->flush(cp).scheduleOn(executor).start());
            bid = co_await bt.node_stream_->append(cp, /*segment_id=*/0, std::move(buf));
        }

        auto const compact_id = to_compact_nodeid(node.node_id());
        auto const compact_blkid = COWBtree::CompactBlkId{*bid};
        bt.bnodeid_map_.update(compact_id, compact_blkid);

        on_flushed(compact_id, compact_blkid);
    }

    // Flush last WriteUnit to disk.
    flush_futs.push_back(bt.node_stream_->flush(cp).scheduleOn(executor).start());

    co_await folly::collectAll(std::move(flush_futs));
}

template < typename OnNodeDeleted >
folly::coro::Task< void > flush_deleted_nodes(COWBtree& bt, CP* cp, OnNodeDeleted&& on_deleted) {
    auto* session = bt.cp_session(cp->id());

    // Invalidate deleted nodes.
    for (auto compact_id : session->deleted_nodes_) {
        BlkId const old_blkid = bt.bnodeid_map_.lookup(compact_id);
        if (old_blkid.is_valid()) {
            bt.node_stream_->invalidate(cp, old_blkid);
            bt.bnodeid_map_.remove(compact_id);
            bt.nodeid_generator_.unreserve(compact_id);
        }
        on_deleted(compact_id);
    }
    co_return;
}

folly::coro::Task< void > flush_overflow_nodes(COWBtree& bt, CP* cp) {
    auto* session = bt.cp_session(cp->id());

    // Write dirty overflow blocks.
    for (auto const& handle : session->dirty_overflow_blks_) {
        auto const& ovf = handle.value();
        co_await bt.overflow_stream_->write(ovf.blkid, *ovf.buf, false);
        bt.overflow_stream_->commit_blk(cp, ovf.blkid);
    }

    // Invalidate deleted overflow blocks.
    for (auto const& blkid : session->deleted_overflow_blks_) {
        co_await bt.overflow_stream_->invalidate(cp, blkid);
    }
    co_return;
}

bool COWBtree::is_dirty(cp_id_t cp_id) const {
    auto const* session = cp_sessions_[cp_id % CPManager::max_concurent_cps].get();
    return (session->cp_id_ == cp_id) &&
        (session->modified_nodes_.size() > 0 || session->deleted_nodes_.size() > 0 ||
         session->dirty_overflow_blks_.size() > 0 || session->deleted_overflow_blks_.size() > 0);
}

folly::coro::Task< void > COWBtree::cp_flush(CP* cp, bool suggest_incremental) {
    // Manager pre-computes whether the global incr_map size threshold has been crossed and passes that as a
    // suggestion via suggest_incremental.  Currently we always honor the suggestion; a future btree-local override
    // (e.g. force full when this btree's last_full_map_cp_id is too far behind) would tweak `incr_flush` here.
    CPSession* session = cp_session(cp->id());
    bool const incr_flush = suggest_incremental;

    COWBT_CP_LOG(INFO, cp->id(), "CP flush starting, mode={} (suggested={})", incr_flush ? "incremental" : "full",
                 suggest_incremental ? "incremental" : "full");
    if (incr_flush) {
        co_await incr_cp_flush(cp);
    } else {
        co_await full_cp_flush(cp);
    }
    session->finish();
    COWBT_CP_LOG(INFO, cp->id(), "CP flush complete, map_size={}", bnodeid_map_.size());
    co_return;
}

folly::coro::Task< bool > COWBtree::incr_cp_flush(CP* cp) {
    auto* session = cp_session(cp->id());

    if (session->modified_nodes_.size() == 0 && session->deleted_nodes_.size() == 0 &&
        session->dirty_overflow_blks_.size() == 0 && session->deleted_overflow_blks_.size() == 0) {
        COWBT_CP_LOG(INFO, cp->id(), "Nothing to flush — no dirty/deleted nodes or overflow");
        co_return false;
    }

    auto const cur_tail_offset = incr_map_stream_->tail_offset();
    auto const new_root = session->new_root_id_.exchange(empty_bnodeid);
    auto const modified_count = session->modified_nodes_.size();
    auto const deleted_count = session->deleted_nodes_.size();

    COWBT_CP_LOG(INFO, cp->id(), "Incr flush started: modified={} deleted={} dirty_overflow={} deleted_overflow={}",
                 modified_count, deleted_count, session->dirty_overflow_blks_.size(),
                 session->deleted_overflow_blks_.size());

    // Snapshot the incr_map stream's pre-flush tail so we can report bytes appended to the manager (for the global
    // incr_map size threshold).
    auto const incr_map_pre_offset = incr_map_stream_->tail_offset();

    // Step 1 - Flush overflow blocks first — nodes may reference overflow blkids, so overflow must be on disk first.
    co_await flush_overflow_nodes(*this, cp);

    // Step 2 - Write IncrMapHeader.
    IncrMapHeader map_hdr{};
    map_hdr.cp_id = cp->id();
    map_hdr.num_updates = to_u32(modified_count);
    map_hdr.num_deletes = to_u32(deleted_count);
    map_hdr.new_root_nodeid = (new_root != empty_bnodeid) ? to_compact_nodeid(new_root) : EmptyCompactNodeId;
    incr_map_stream_->append(sisl::Blob{uintptr_cast(&map_hdr), sizeof(map_hdr)});

    // Journal record buffer — one IncrMapNodeRecord per contiguous blkid run.
    static constexpr uint32_t kMaxRecordBufSize = IncrMapNodeRecord::size(1024);
    alignas(8) uint8_t record_buf[kMaxRecordBufSize];
    IncrMapNodeRecord* cur_record = nullptr;
    uint32_t num_node_records = 0;
    uint32_t crc = 0;

    // Step 3 - Flush dirty nodes; the callback accumulates journal records.
    co_await flush_dirty_nodes(*this, cp, [&](CompactNodeId compact_id, CompactBlkId compact_blkid) {
        if (cur_record == nullptr) {
            cur_record = new (record_buf) IncrMapNodeRecord{};
            cur_record->base_blkid = compact_blkid;
            cur_record->n_nodes = 0;
        } else if (cur_record->base_blkid.chunk_num != compact_blkid.chunk_num ||
                   s_cast< blk_num_t >(cur_record->base_blkid.blk_num + cur_record->n_nodes) != compact_blkid.blk_num) {
            // Contiguity break — flush current record to incr_map_stream_.
            uint32_t const sz = cur_record->size();
            crc = crc32_ieee(crc, record_buf, sz);
            incr_map_stream_->append(sisl::Blob{record_buf, sz});
            ++num_node_records;

            cur_record = new (record_buf) IncrMapNodeRecord{};
            cur_record->base_blkid = compact_blkid;
            cur_record->n_nodes = 0;
        }
        cur_record->nodes[cur_record->n_nodes++] = compact_id;
    });

    // Flush last partial journal record.
    if (cur_record != nullptr && cur_record->n_nodes > 0) {
        uint32_t const sz = cur_record->size();
        crc = crc32_ieee(crc, record_buf, sz);
        incr_map_stream_->append(sisl::Blob{record_buf, sz});
        ++num_node_records;
    }

    // Step 4 - Write deleted node ids to journal.
    co_await flush_deleted_nodes(*this, cp, [&](CompactNodeId compact_id) {
        crc = crc32_ieee(crc, uintptr_cast(&compact_id), sizeof(CompactNodeId));
        incr_map_stream_->append(sisl::Blob{uintptr_cast(&compact_id), sizeof(CompactNodeId)});
    });

    // Step 5 - Write IncrMapFooter.
    IncrMapFooter map_footer{};
    map_footer.num_records = num_node_records;
    map_footer.checksum = crc;
    incr_map_stream_->append(sisl::Blob{uintptr_cast(&map_footer), sizeof(map_footer)});

    bnodeid_map_.updates_since_last_flush_.fetch_add(session->modified_nodes_.size() + session->deleted_nodes_.size());

    // Report bytes appended to the manager so the global incr_map size threshold is tracked accurately.
    auto const incr_map_appended = incr_map_stream_->tail_offset() - incr_map_pre_offset;
    mgr_.incr_map_appended(incr_map_appended);

    COWBT_CP_LOG(INFO, cp->id(),
                 "Incr flush complete: node_records={} updates={} deletes={} crc={:#x} incr_map_appended={}",
                 num_node_records, modified_count, deleted_count, crc, incr_map_appended);
    co_return true;
}

folly::coro::Task< void > COWBtree::full_cp_flush(CP* cp) {
    CPSession* session = cp_session(cp->id());
    auto const updates_since_last_flush = bnodeid_map_.updates_since_last_flush_.load();
    COWBT_CP_LOG(INFO, cp->id(),
                 "Full flush started: modified={} deleted={} dirty_overflow={} deleted_overflow={}, "
                 "map_changes_since_last_flush={}",
                 session->modified_nodes_.size(), session->deleted_nodes_.size(), session->dirty_overflow_blks_.size(),
                 session->deleted_overflow_blks_.size(), updates_since_last_flush);

    // Step 1 - Flush overflow first — nodes may reference overflow blkids.
    co_await flush_overflow_nodes(*this, cp);

    // Step 2 - Flush dirty nodes with a no-op callback (full map will capture everything).
    co_await flush_dirty_nodes(*this, cp, [](CompactNodeId, CompactBlkId) {});

    // Step 3 - Delete all nodes, no-op callback
    co_await flush_deleted_nodes(*this, cp, [](CompactNodeId) {});

    // Check if there are accumulated updates worth flushing.
    if (updates_since_last_flush == 0) {
        COWBT_CP_LOG(INFO, cp->id(), "Skipping full map flush — no changes since last flush");
        co_return;
    }

    auto const active_idx = active_full_map_.load(std::memory_order_acquire);
    auto& active_stream = *full_map_streams_[active_idx];
    auto& passive_stream = *full_map_streams_[active_idx ^ 1];

    // Serialize the entire bnodeid_map.  serialize() holds the shared lock while the callback runs — append() is
    // synchronous so the memcpy completes while entries_ is stable.
    bnodeid_map_.serialize(cp->id(), [&active_stream](sisl::Blob blob) { active_stream.append(blob); });
    co_await active_stream.flush();

    // Update and persist the superblk with this CP's id as the last full map cp.
    mutable_super_blk().last_full_map_cp_id = cp->id();
    co_await mblk_.write(to_cu8ptr(&super_blk()), sizeof(COWBtreeSuperBlock));

    // Swap: the passive stream is now stale; truncate up to its tail so all chunks get released (head==tail triggers
    // the fresh-start reset inside truncate).
    active_full_map_.store(active_idx ^ 1, std::memory_order_release);
    co_await passive_stream.truncate(passive_stream.tail_offset());

    // Truncate the incremental map stream — all deltas are covered by the full map.
    auto const incr_map_truncated_bytes = incr_map_stream_->tail_offset();
    co_await incr_map_stream_->truncate(incr_map_stream_->tail_offset());
    mgr_.incr_map_truncated(incr_map_truncated_bytes);

    bnodeid_map_.updates_since_last_flush_.store(0);
    COWBT_CP_LOG(INFO, cp->id(), "Full map flush complete: incr_map_truncated={}", incr_map_truncated_bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
//       Recovery Section
// ─────────────────────────────────────────────────────────────────────────────

folly::coro::Task< void > COWBtree::recover() {
    auto const cur_cp_id = cp_mgr().cp_guard()->id();
    auto const last_full_cp = super_blk().last_full_map_cp_id;

    COWBT_LOG(INFO, "Recovery started: cur_cp_id={} last_full_map_cp_id={} incr_stream_tail={}", cur_cp_id,
              last_full_cp, incr_map_stream_->tail_offset());

    // Recover full map cp
    co_await recover_full_map(cur_cp_id);

    // If there is no pending incremental cp, we are done recovering cow btree maps
    if (incr_map_stream_->tail_offset() == 0) {
        COWBT_LOG(INFO, "Recovery complete: no incremental journal entries to apply");
        co_return;
    }

    uint64_t offset = 0;
    uint32_t incr_cps_applied = 0;
    uint32_t incr_cps_skipped = 0;
    while (offset < incr_map_stream_->tail_offset()) {
        auto prev_offset = offset;
        auto new_offset = co_await recover_one_incr_cp(offset, last_full_cp, cur_cp_id);
        if (new_offset == 0) {
            break;
        }
        if (new_offset == prev_offset + sizeof(IncrMapHeader)) {
            ++incr_cps_skipped;
        } else {
            ++incr_cps_applied;
        }
        offset = new_offset;
    }

    COWBT_LOG(INFO, "Recovery complete: applied {} incr CPs, skipped {}, map_size={}", incr_cps_applied,
              incr_cps_skipped, bnodeid_map_.size());
}

folly::coro::Task< void > COWBtree::recover_full_map(cp_id_t cur_cp_id) {
    auto const last_full_cp = super_blk().last_full_map_cp_id;

    uint8_t recovered_idx = std::numeric_limits< uint8_t >::max();
    for (uint8_t idx = 0; idx < 2; ++idx) {
        auto& stream = *full_map_streams_[idx];
        if (stream.tail_offset() < sizeof(BNodeIdMapHeader)) {
            continue;
        }

        auto [ec, hdr_buf] = co_await stream.read(0, sizeof(BNodeIdMapHeader));
        if (ec) {
            continue;
        }

        auto const& hdr = *r_cast< BNodeIdMapHeader const* >(hdr_buf.bytes());
        if (hdr.magic != BNodeIdMapHeader::MAGIC) {
            continue;
        }

        if (hdr.cp_id >= cur_cp_id || hdr.cp_id != last_full_cp) {
            COWBT_LOG(INFO, "Truncating full_map stream[{}] with cp_id={} (last_full_cp={}, cur_cp_id={})", idx,
                      hdr.cp_id, last_full_cp, cur_cp_id);
            co_await stream.truncate(stream.tail_offset());
            continue;
        }

        auto const total_size = BNodeIdMap::serialized_size(hdr.num_entries);
        auto [ec2, full_buf] = co_await stream.read(0, total_size);
        HS_REL_ASSERT(!ec2, "recover_full_map: failed to read full map from stream {}", idx);
        bnodeid_map_.load_from(sisl::Blob{full_buf.bytes(), to_u32(total_size)});

        // TODO: serialize nodeid_generator_ alongside the map to avoid this O(n) scan.
        for (CompactNodeId nid = 0; nid < hdr.num_entries; ++nid) {
            if (bnodeid_map_.lookup(nid).is_valid()) {
                nodeid_generator_.reserve(nid);
            }
        }

        COWBT_LOG(INFO, "Recovered full map from stream[{}] cp_id={} entries={} live={}", idx, hdr.cp_id,
                  hdr.num_entries, hdr.live_count);
        recovered_idx = idx;
        break;
    }

    if (recovered_idx < 2) {
        active_full_map_.store(recovered_idx ^ 1, std::memory_order_relaxed);
        auto& stale = *full_map_streams_[recovered_idx ^ 1];
        if (stale.tail_offset() > 0) {
            co_await stale.truncate(stale.tail_offset());
        }
    }
}

folly::coro::Task< IOBuffer > COWBtree::read_from_incr_stream(uint64_t offset, size_t len) {
    if (offset + len > incr_map_stream_->tail_offset()) {
        HS_REL_ASSERT(false, "Expected atleast {} bytes after offset={}, but got eof, tail_offset={}", len, offset,
                      incr_map_stream_->tail_offset());
    }
    auto [ec, buf] = co_await incr_map_stream_->read(offset, len);
    HS_REL_ASSERT(!ec, "read_from_incr_stream: failed to read from incr_map_stream_ at offset={}", offset);
    co_return {std::move(buf)};
}

folly::coro::Task< uint64_t > COWBtree::recover_one_incr_cp(uint64_t offset, cp_id_t last_full_cp, cp_id_t cur_cp_id) {
    auto hdr_buf = co_await read_from_incr_stream(offset, sizeof(IncrMapHeader));
    auto const& hdr = *r_cast< IncrMapHeader const* >(hdr_buf.bytes());
    HS_REL_ASSERT_EQ(hdr.header_magic, IncrMapHeader::HEADER_MAGIC, "recover_one_incr_cp: invalid header magic");

    // Decide if we need the incr cp records to be processed.
    if (hdr.cp_id >= cur_cp_id) {
        COWBT_LOG(INFO, "Skipping incr journal at offset={} because it has incomplete cp_id={}, post restart cp_id={}",
                  offset, hdr.cp_id, cur_cp_id);
        co_return 0;
    }
    offset += sizeof(IncrMapHeader);

    // Skip anything that is already present in full_map_cp
    bool const skip = (hdr.cp_id <= last_full_cp);
    if (skip) {
        COWBT_LOG(INFO, "Skipping incr map records for cp_id={}, since full map cp_id={} has already been recovered",
                  hdr.cp_id, last_full_cp);
    }

    // Walk IncrMapNodeRecords.
    uint32_t updates_seen = 0;
    while (updates_seen < hdr.num_updates) {
        auto rbuf = co_await read_from_incr_stream(offset, sizeof(IncrMapNodeRecord));
        auto const* rec_hdr = r_cast< IncrMapNodeRecord const* >(rbuf.bytes());
        uint32_t const rec_size = rec_hdr->size();

        if (!skip) {
            auto full_rbuf = co_await read_from_incr_stream(offset, rec_size);
            auto const* rec = r_cast< IncrMapNodeRecord const* >(full_rbuf.bytes());
            for (uint16_t n = 0; n < rec->n_nodes; ++n) {
                bnodeid_map_.update(rec->nodes[n], CompactBlkId{rec->base_blkid.to_blkid(), n});
                nodeid_generator_.reserve(rec->nodes[n]);
            }
        }

        offset += rec_size;
        updates_seen += rec_hdr->n_nodes;
    }

    // Walk deletes.
    if (skip) {
        offset += hdr.num_deletes * sizeof(CompactNodeId);
    } else {
        for (uint32_t i = 0; i < hdr.num_deletes; ++i) {
            auto dbuf = co_await read_from_incr_stream(offset, sizeof(CompactNodeId));
            auto const del_id = *r_cast< CompactNodeId const* >(dbuf.bytes());
            bnodeid_map_.remove(del_id);
            nodeid_generator_.unreserve(del_id);
            offset += sizeof(CompactNodeId);
        }
    }

    // Validate footer.
    auto fbuf = co_await read_from_incr_stream(offset, sizeof(IncrMapFooter));
    auto const& footer = *r_cast< IncrMapFooter const* >(fbuf.bytes());
    HS_REL_ASSERT_EQ(footer.footer_magic, IncrMapFooter::FOOTER_MAGIC, "recover_one_incr_cp: invalid footer magic");
    offset += sizeof(IncrMapFooter);

    if (!skip) {
        if (hdr.new_root_nodeid != EmptyCompactNodeId) {
            mutable_super_blk().root_node_id = ordinal_shifted_ | hdr.new_root_nodeid;
        }
        COWBT_LOG(INFO, "Applied incr journal cp_id={}: updates={} deletes={} node_records={}{}", hdr.cp_id,
                  hdr.num_updates, hdr.num_deletes, footer.num_records,
                  (hdr.new_root_nodeid != EmptyCompactNodeId) ? fmt::format("new_root_id={}", hdr.new_root_nodeid)
                                                              : "");
    }

    co_return offset;
}

// ─────────────────────────────────────────────────────────────────────────────
// CPSession helpers
// ─────────────────────────────────────────────────────────────────────────────

COWBtree::CPSession* COWBtree::cp_session(cp_id_t cp_id) {
    auto* session = cp_sessions_[cp_id % CPManager::max_concurent_cps].get();
    if (sisl_unlikely(session->cp_id_ != cp_id)) {
        session->finish();
        session->cp_id_ = cp_id;
    }
    return session;
}

} // namespace homestore