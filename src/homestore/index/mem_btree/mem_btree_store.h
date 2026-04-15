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
    bool valid() const override { return ptr_ != nullptr; }

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
    MemBtree() = default;

    void bind_to(BtreeBase* base) override { base_btree_ = base; }

    Node create_node(bool is_leaf) override;

    // Returns an unlocked Node for the NodeCore at the given id (pointer address).
    BtreeResult< Node > read_node(bnodeid_t id, LockType lock_type) const override;

    void write_node(const Node& node) override;
    BtreeStatus prepare_for_write(const Node& node) override;
    void remove_node(const Node& node) override;
    void on_root_changed(const Node& root) override;
    uint64_t space_occupied() const override { return 0; }

private:
    BtreeBase* base_btree_{nullptr};
};

} // namespace homestore
