#pragma once

#include <cstdint>
#include <memory>

#include <folly/concurrency/ConcurrentHashMap.h>

#include "homestore/index/btree/btree_base.h"
#include "homestore/index/btree/detail/btree_node.h"

namespace homestore {

// Forward declaration — full definition in <homestore/index/btree/btree.h>.  Callers of MemBtree::create<K,V>() must
// have the Btree<K,V> template body visible (via btree.ipp or btree_test_helper.hpp) at the call site.
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

// ─────────────────────────────────────────────────────────────────────────────
// MemBtree — in-memory UnderlyingBtree backend.
//
// Node ownership: each allocated NodeCore is heap-owned by a unique_ptr stored in a folly::ConcurrentHashMap keyed by
// the raw NodeCore*.  The bnodeid_t of a node is just reinterpret_cast(NodeCore*), so read_node() is a zero-cost
// pointer reinterpret — no hashtable lookup on the hot path.  The map exists purely so:
//   - MemBtree destruction frees every node the tree ever allocated.
//   - remove_node() can drop the owning unique_ptr.
// This mirrors the Rust memdb design (pointer-as-id) while remaining safe in C++ without GC.
// ─────────────────────────────────────────────────────────────────────────────
class MemBtree : public UnderlyingBtree {
public:
    MemBtree() = default;
    MemBtree(MemBtree const&) = delete;
    MemBtree& operator=(MemBtree const&) = delete;
    MemBtree(MemBtree&&) = delete;
    MemBtree& operator=(MemBtree&&) = delete;
    ~MemBtree() override = default; // nodes_ unique_ptrs free all NodeCores automatically

    // Sync factory — allocates a MemBtree, wires it into a Btree<K,V>, and returns the tree.  Caller must include
    // <homestore/index/btree/btree.ipp> so the Btree<K,V> template body is visible at the call site.
    template < typename K, typename V >
    static shared< Btree< K, V > > create(BtreeConfig const& cfg);

    // ── UnderlyingBtree interface ────────────────────────────────────────────
    void bind_to(BtreeBase* base) override { base_btree_ = base; }

    Node create_node(bool is_leaf) override;
    BtreeResult< Node > read_node(bnodeid_t id, LockType lock_type) const override;
    void write_node(Node const& /*node*/) override {}
    BtreeStatus prepare_for_write(Node const& /*node*/) override { return BtreeStatus::success; }
    void remove_node(Node const& node) override;
    void on_root_changed(Node const& /*root*/) override {}
    uint64_t space_occupied() const override;

    // ── Overflow support (in-memory) ─────────────────────────────────────────
    BtreeStatus write_overflow(sisl::ByteArray const& buf, BlkId& out_blkid) override;
    BtreeTask< BtreeStatus > read_overflow(BlkId const& blkid, sisl::ByteArray& out_buf) const override;
    BtreeStatus delete_overflow(BlkId const& blkid) override;

private:
    BtreeBase* base_btree_{nullptr};

    // Ownership map: unique_ptr owns each live NodeCore.  Keyed by raw NodeCore* (= bnodeid_t after cast).
    // ConcurrentHashMap permits concurrent insert/erase without a mutex.
    folly::ConcurrentHashMap< NodeCore*, unique< NodeCore > > nodes_;

    // In-memory overflow storage.  BlkId is synthesized from a monotonic counter; the ByteArray is heap-owned.
    mutable folly::ConcurrentHashMap< uint64_t, sisl::ByteArray > overflow_store_;
    std::atomic< uint64_t > overflow_next_id_{1};
};

// Template factory — defined inline to keep it visible wherever Btree<K,V> is.  Relies on the caller having already
// included btree.ipp (or btree_test_helper.hpp, which does).  Avoid #including btree.ipp here to keep mem_btree.h as
// light as the other UnderlyingBtree headers.
template < typename K, typename V >
shared< Btree< K, V > > MemBtree::create(BtreeConfig const& cfg) {
    auto underlying = std::make_shared< MemBtree >();
    return std::make_shared< Btree< K, V > >(cfg, std::move(underlying));
}

} // namespace homestore
