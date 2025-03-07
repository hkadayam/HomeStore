#include <homestore/btree/btree_base.hpp>
#include <homestore/btree/btree_store.h>
#include <homestore/btree/detail/btree_node.hpp>
#include "common/homestore_assert.hpp"

namespace homestore {
BtreeBase::BtreeBase(BtreeConfig const& cfg, uuid_t uuid, uuid_t parent_uuid, uint32_t user_sb_size) :
        Index::Index{cfg.store_type() == IndexStore::Type::MEM_BTREE}, m_bt_cfg{cfg} {
    m_sb.create(sizeof(IndexSuperBlock));
    m_sb->uuid = uuid;
    m_sb->parent_uuid = parent_uuid;
    m_sb->user_sb_size = user_sb_size;
    m_sb->index_store_type = cfg.store_type();
    m_sb->ordinal = hs()->index_service().reserve_ordinal();

    m_store =
        std::static_pointer_cast< BtreeStore >(hs()->index_service().lookup_or_create_store(cfg.store_type(), {}));
    m_bt_private = std::move(m_store->on_btree_created(*this, false /* load_existing */));
    m_sb.write();
}

BtreeBase::BtreeBase(BtreeConfig const& cfg, superblk< IndexSuperBlock >&& sb) :
        Index::Index{cfg.store_type() == IndexStore::Type::MEM_BTREE}, m_bt_cfg{cfg} {
    HS_REL_ASSERT_EQ(cfg.store_type(), m_sb->index_store_type,
                     "Config requirement and super block differs in store_type");
    m_sb = std::move(sb);
    m_store =
        std::static_pointer_cast< BtreeStore >(hs()->index_service().lookup_or_create_store(cfg.store_type(), {}));
    m_bt_private = std::move(m_store->on_btree_created(*this, true /* load_existing*/));
}

uint32_t BtreeBase::node_size() const { return m_bt_cfg.node_size(); };
uint64_t BtreeBase::used_size() const { return m_store->used_size(*this); }
uint32_t BtreeBase::ordinal() const { return m_sb->ordinal; }
std::string BtreeBase::name() const { return m_bt_cfg.name(); }

///////////////////////////// BtreeCPGuard Section //////////////////////////////
BtreeCPGuard::BtreeCPGuard(BtreeBase& btree) :
        CPGuard{btree.is_ephemeral() ? nullptr : &(hs()->cp_mgr())}, m_btree{btree} {}

BtreeCPGuard::BtreeCPGuard(const BtreeCPGuard& other) {
    if (other.m_btree.is_ephemeral()) { return; }
    m_btree = other.m_btree;
    CPGuard::operator=(other);
}

BtreeCPGuard BtreeCPGuard::operator=(const BtreeCPGuard& other) {
    if (btree.is_ephemeral()) { return *this; }
    m_btree = other.m_btree;
    CPGuard::operator=(other);
    return *this;
}

CP& BtreeCPGuard::operator*() {
    // Ephemeral shouldn't be calling *, but just return an empty CP
    if (m_btree.is_ephemeral()) { return CP{&hs()->cp_mgr()}; }
    return *get();
}

CP* BtreeCPGuard::operator->() { return get(); }

CPContext* BtreeCPGuard::context() {
    return m_btree.is_ephemeral() ? nullptr : get()->context(cp_consumer_t::INDEX_SVC);
}

CP* BtreeCPGuard::get() {
    if (m_btree.is_ephemeral()) { return nullptr; }
    return CPGuard::get();
}

} // namespace homestore