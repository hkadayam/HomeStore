#include <homestore/btree/btree.hpp>
#include <homestore/btree/btree_store.h>

namespace homestore {
BtreeBase::BtreeBase(BtreeConfig const& cfg, uuid_t uuid, uuid_t parent_uuid, uint32_t user_sb_size) :
        Index::Index{cfg.store_type() == IndexStore::Type::MEM_BTREE}, m_bt_cfg{cfg} {
    m_sb.create(sizeof(IndexSuperBlock));
    m_sb->uuid = uuid;
    m_sb->parent_uuid = parent_uuid;
    m_sb->user_sb_size = user_sb_size;
    m_sb->index_store_type = cfg.store_type();
    m_sb->ordinal = hs()->index_service()->reserve_ordinal();

    m_store = hs()->index_service()->lookup_or_create_store(cfg.store_type(), {});
    m_bt_private = std::move(m_store->on_btree_created(*this, false /* load_existing */));
    m_sb.write();
}

BtreeBase::BtreeBase(BtreeConfig const& cfg, superblk< IndexSuperBlock >&& sb) :
        Index::Index{cfg.store_type() == IndexStore::Type::MEM_BTREE}, m_bt_cfg{cfg}, m_sb{std::move(sb)} {
    HS_REL_ASSERT_EQ(cfg.store_type(), m_sb->index_store_type,
                     "Config requirement and super block differs in store_type");
    m_store = hs()->index_service()->lookup_or_create_store(cfg.store_type(), {});
    m_bt_private = std::move(m_store->on_btree_created(this, true /* load_existing*/));
}

BtreeBase::~BtreeBase() {
    if (is_ephemeral()) { this->destroy(); }
}

StoreSpecificBtree* BtreeBase::store_specific_btree() { return m_bt_private.get(); }
uint32_t BtreeBase::node_size() const { return m_bt_cfg->node_size; };
uint32_t BtreeBase::ordinal() const { return m_sb->ordinal; };
uint64_t used_size() const { return m_store->used_size(*this); }

} // namespace homestore