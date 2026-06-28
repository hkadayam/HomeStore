#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>

#include <folly/concurrency/ConcurrentHashMap.h>

#include "sisl/fds/thread_vector.h"
#include "homestore/index/btree/btree_base.h"
#include "homestore/index/btree/detail/btree_node.h"

namespace homestore {

// Forward declaration — full definition in <homestore/index/btree/btree.h>.  Callers of MemBtree::create<K,V>() must
// have the Btree<K,V> template body visible (via btree.ipp or btree_test_helper.h) at the call site.
template < typename K, typename V >
class Btree;

// ─────────────────────────────────────────────────────────────────────────────
// MemNodeHandle — non-owning raw pointer to a NodeCore.
// MemBtree owns all node memory; this handle just points into it.
// sizeof(MemNodeHandle) must fit within Node::kStorageBytes.
// ─────────────────────────────────────────────────────────────────────────────
class MemNodeHandle final : public NodeHandle {
public:
    explicit MemNodeHandle(NodeCore* p) noexcept : ptr_{p} {}

    NodeCore* get() override { return ptr_; }
    bool valid() const override { return ptr_ != nullptr; }

    void move_to(void* dest) noexcept override {
        new (dest) MemNodeHandle(ptr_);
        ptr_ = nullptr;
    }

private:
    NodeCore* ptr_{nullptr};
};

static_assert(sizeof(MemNodeHandle) <= Node::kStorageBytes,
              "MemNodeHandle exceeds Node::kStorageBytes — increase kStorageBytes");

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MemBtree — in-memory UnderlyingBtree backend.
//
// Node ownership — split across three structures to keep create_node / remove_node lock-free on the hot path while
// deferring actual destruction to a single drainer thread (see MemBtreeDrainer):
//   - nodes_ : std::map<bnodeid_t, unique<NodeCore>>.  Owning registry, touched ONLY by the drainer (no lock needed).
//   - pending_creates_ : ThreadVector<NodeCore*>.  create_node() moves the nodecore; the drainer adopts it into
//                        nodes_ via unique_ptr ctor.  Ambient ownership between push and adoption.
//   - pending_removes_ : CIV<NodeCore*>.  remove_node() pushes the raw pointer here; the drainer later erases the
//                        matching entry from nodes_, destroying the unique_ptr.
//
// The bnodeid_t of a node is just reinterpret_cast(NodeCore*), so read_node() remains a zero-cost pointer reinterpret
// with no map lookup on the hot path.
//
// Synchronization: drain_sync_mtx_ is a shared_mutex — pushers take it shared (near-atomic cost, no contention among
// pushers since CIV is thread-local internally); the drainer takes it exclusive, which briefly excludes pushers while
// it walks & clears the CIVs.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class MemBtree : public UnderlyingBtree {
public:
    MemBtree();
    MemBtree(MemBtree const&) = delete;
    MemBtree& operator=(MemBtree const&) = delete;
    MemBtree(MemBtree&&) = delete;
    MemBtree& operator=(MemBtree&&) = delete;
    ~MemBtree() override;

    // Sync factory — allocates a MemBtree, wires it into a Btree<K,V>, and returns the tree.  Caller must include
    // <homestore/index/btree/btree.ipp> so the Btree<K,V> template body is visible at the call site.
    template < typename K, typename V >
    static shared< Btree< K, V > > create(BtreeConfig const& cfg);

    // ── UnderlyingBtree interface ────────────────────────────────────────────
    void bind_to(BtreeBase* base) override { base_btree_ = base; }

    Node create_node(bool is_leaf) override;
    BtreeResult< Node > read_node(bnodeid_t id, LockType lock_type) override;
    void write_node(Node const& /*node*/) override {}
    BtreeStatus prepare_for_write(Node const& /*node*/) override { return BtreeStatus::success; }
    void remove_node(Node const& node) override;
    void on_root_changed(Node const& /*root*/) override {}
    uint64_t space_occupied() const override;
    std::shared_ptr< uint8_t > allocate_node_buf() override {
        return std::shared_ptr< uint8_t >{new uint8_t[base_btree_->node_size()](), std::default_delete< uint8_t[] >{}};
    }

    // ── Overflow support (in-memory) ─────────────────────────────────────────
    BtreeStatus write_overflow(sisl::IoBufShared const& buf, BlkId& out_blkid) override;
    BtreeTask< BtreeStatus > read_overflow(BlkId const& blkid, sisl::IoBufShared& out_buf) const override;
    BtreeStatus delete_overflow(BlkId const& blkid) override;

    // Called by MemBtreeDrainer — applies pending ops to nodes_.
    void drain();

private:
    BtreeBase* base_btree_{nullptr};

    std::map< bnodeid_t, unique< NodeCore > > nodes_;
    sisl::ThreadVector< unique< NodeCore > > pending_creates_;
    sisl::ThreadVector< NodeCore* > pending_removes_;

    mutable folly::ConcurrentHashMap< uint64_t, sisl::IoBufShared > overflow_store_;
    std::atomic< uint64_t > overflow_next_id_{1};
};

// Template factory — defined inline to keep it visible wherever Btree<K,V> is.  Relies on the caller having already
// included btree.ipp (or btree_test_helper.h, which does).  Avoid #including btree.ipp here to keep mem_btree.h as
// light as the other UnderlyingBtree headers.
template < typename K, typename V >
shared< Btree< K, V > > MemBtree::create(BtreeConfig const& cfg) {
    auto underlying = std::make_shared< MemBtree >();
    return std::make_shared< Btree< K, V > >(cfg, std::move(underlying));
}

} // namespace homestore
