#include "index/mem_btree/mem_btree.h"
#include <homestore/index/btree/detail/btree_node.h>
#include <homestore/index/btree/btree_base.h>

namespace homestore {

Node MemBtree::create_node(bool is_leaf) {
    // Allocate a plain shared_ptr-managed byte array of node_size and hand it to construct_fresh_node, which
    // placement-builds the correct variant (SimpleNode, VarObj/Value/Key, ...) into it.
    const uint32_t node_sz = base_btree_->node_size();
    std::shared_ptr< uint8_t > buf{new uint8_t[node_sz](), std::default_delete< uint8_t[] >{}};
    auto node = base_btree_->construct_fresh_node(std::move(buf), bnodeid_t{0}, is_leaf);
    NodeCore* core = node.get();

    // node_id is the raw pointer reinterpreted as 64-bit — lets read_node() be a pure reinterpret_cast with no map
    // lookup on the hot path.  The ConcurrentHashMap only exists so we can track ownership for remove_node() and
    // ensure every node is freed on MemBtree destruction.
    core->set_node_id(bnodeid_t{r_cast< std::uintptr_t >(core)});
    nodes_.insert(core, std::move(node));

    MemNodeHandle handle{core};
    return Node::construct(handle, LockType::Write);
}

BtreeResult< Node > MemBtree::read_node(bnodeid_t id, LockType lock_type) const {
    // Pure pointer reinterpret — no map lookup. Safe because MemBtree owns the NodeCore until remove_node()
    // erases it from nodes_, and by the btree invariants, we only ever read a node_id that is still reachable
    // from the tree root (hence still present in nodes_).
    MemNodeHandle handle{r_cast< NodeCore* >(id)};
    CO_RETURN CO_AWAIT Node::async_construct(handle, lock_type);
}

void MemBtree::remove_node(const Node& node) {
    // Erasing from the ConcurrentHashMap drops the owning unique_ptr, which deletes the NodeCore.
    nodes_.erase(node.operator->());
}

uint64_t MemBtree::space_occupied() const {
    return nodes_.size() * base_btree_->bt_config().node_size();
}

BtreeStatus MemBtree::write_overflow(sisl::ByteArray const& buf, BlkId& out_blkid) {
    auto id = overflow_next_id_.fetch_add(1, std::memory_order_relaxed);
    // Copy the buffer into a new ByteArray so the caller can free theirs.
    auto copy = sisl::make_byte_array(buf->size(), 0);
    std::memcpy(copy->bytes(), buf->cbytes(), buf->size());
    overflow_store_.insert(id, std::move(copy));
    out_blkid = BlkId{static_cast< blk_num_t >(id), 1 /* nblks */, 0 /* chunk */};
    return BtreeStatus::success;
}

BtreeTask< BtreeStatus > MemBtree::read_overflow(BlkId const& blkid, sisl::ByteArray& out_buf) const {
    auto it = overflow_store_.find(to_u64(blkid.blk_num()));
    if (it == overflow_store_.end()) {
        CO_RETURN BtreeStatus::node_read_failed;
    }
    out_buf = it->second;
    CO_RETURN BtreeStatus::success;
}

BtreeStatus MemBtree::delete_overflow(BlkId const& blkid) {
    overflow_store_.erase(to_u64(blkid.blk_num()));
    return BtreeStatus::success;
}

} // namespace homestore
