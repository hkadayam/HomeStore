#pragma once

#include <vector>
#include <memory>

#include <homestore/index/btree/btree_store.h>
#include <homestore/index/btree/btree_base.h>

namespace homestore {

/// MemNodeHandle — non-owning raw pointer to a NodeCore.
/// MemBtree owns all node memory; this handle just points into it.
/// sizeof(MemNodeHandle) must fit within Node::kStorageBytes.
class MemNodeHandle final : public NodeHandle {
public:
    explicit MemNodeHandle(NodeCore* p) noexcept : ptr_{p} {}

    NodeCore* get() override { return ptr_; }
    bool      valid() const override { return ptr_ != nullptr; }

    void move_to(void* dest) noexcept override {
        new (dest) MemNodeHandle(ptr_);
        ptr_ = nullptr;
    }

private:
    NodeCore* ptr_{nullptr};

    static_assert(sizeof(MemNodeHandle) <= Node::kStorageBytes,
                  "MemNodeHandle exceeds Node::kStorageBytes — increase kStorageBytes");
};

class MemBtreeStore : public BtreeStore {
public:
    MemBtreeStore() = default;
    virtual ~MemBtreeStore() = default;

    void stop() override {}
    std::string store_type() const override { return "MEM_BTREE"; }
    void on_recovery_completed() override {}

    unique< UnderlyingBtree > create_underlying_btree(BtreeBase& btree, bool load_existing) override;
    folly::Future< folly::Unit > destroy_underlying_btree(BtreeBase&) override { return folly::makeFuture(); }
    bool is_fast_destroy_supported() const override { return true; }
    bool is_ephemeral() const override { return true; }
    uint32_t max_node_size() const override { return 4096u; }
};

class MemBtree : public UnderlyingBtree {
public:
    MemBtree(BtreeBase& btree);

    // Returns an unlocked Node wrapping a freshly-allocated NodeCore.
    Node create_node(bool is_leaf) override;

    // Returns an unlocked Node for the NodeCore at the given id (pointer address).
    Node read_node(bnodeid_t id) const override;

    btree_status_t write_node(Node const& node) override;
    btree_status_t prepare_for_write(Node const& node) override;
    void remove_node(Node const& node) override;
    NodeId load_root_node_id() override;
    btree_status_t on_root_changed(Node const& root) override;
    uint64_t       space_occupied() const override { return 0; }

private:
    BtreeBase& m_base_btree;
};

} // namespace homestore
