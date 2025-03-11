#include <string_view>
#include <string>
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

    auto bt_sb = new (m_sb.get()->underlying_index_sb.data()) BtreeSuperBlock();
    m_store =
        std::static_pointer_cast< BtreeStore >(hs()->index_service().lookup_or_create_store(cfg.store_type(), {}));
    m_bt_private = std::move(m_store->on_btree_created(*this, false /* load_existing */));

    if (m_bt_cfg.node_size() == 0) {
        // Caller is requesting to use underlying btree's node size
        m_bt_cfg.m_node_size = m_bt_private->node_size();
    }
    m_bt_cfg.finalize(sizeof(BtreeNode::PersistentHeader));
    bt_sb->node_size = m_bt_cfg.node_size();

    m_sb.write();
}

BtreeBase::BtreeBase(BtreeConfig const& cfg, superblk< IndexSuperBlock >&& sb) :
        Index::Index{cfg.store_type() == IndexStore::Type::MEM_BTREE}, m_bt_cfg{cfg} {
    HS_REL_ASSERT_EQ(cfg.store_type(), m_sb->index_store_type,
                     "Config requirement and super block differs in store_type");
    m_sb = std::move(sb);
    m_store =
        std::static_pointer_cast< BtreeStore >(hs()->index_service().lookup_or_create_store(cfg.store_type(), {}));
    m_bt_cfg.finalize(sizeof(BtreeNode::PersistentHeader));
    m_bt_private = std::move(m_store->on_btree_created(*this, true /* load_existing*/));
}

uint32_t BtreeBase::node_size() const { return m_bt_cfg.node_size(); };
uint64_t BtreeBase::space_occupied() const { return m_bt_private->space_occupied(); }
uint32_t BtreeBase::ordinal() const { return m_sb->ordinal; }
std::string BtreeBase::name() const { return m_bt_cfg.name(); }
BtreeRouteTracer& BtreeBase::route_tracer() { return m_route_tracer; }
[[nodiscard]] CPGuard BtreeBase::bt_cp_guard() { return CPGuard{is_ephemeral() ? nullptr : &(cp_mgr())}; }

BtreeRouteTracer::BtreeRouteTracer(uint32_t buf_size_per_op, bool log_if_rolled) :
        m_max_buf_size_per_op{buf_size_per_op}, m_log_if_rolled{log_if_rolled} {
    m_enabled_ops.reserve(enum_count< BtreeRouteTracer::Op >());
    m_ops_routes.reserve(enum_count< BtreeRouteTracer::Op >());

    for (uint32_t i{0}; i < enum_count< BtreeRouteTracer::Op >(); ++i) {
        m_enabled_ops.push_back(false);
    }
}

void BtreeRouteTracer::append_to(Op op, std::string const& route_str) {
    std::string& cur_buf = m_ops_routes[uint32_cast(op)];
    if (!m_enabled_ops[uint32_cast(op)]) { return; }

    std::unique_lock< iomgr::FiberManagerLib::shared_mutex > lock{m_append_mtx};
    while (cur_buf.size() + route_str.size() > m_max_buf_size_per_op) {
        size_t head_pos = cur_buf.find("Route size=");
        size_t next_pos = cur_buf.find("Route size=", head_pos + 1);
        if (m_log_if_rolled) {
            // TODO: We need to change this to btree specific log.
            LOGINFOMOD(btree, "Btree Route Trace: {}", std::string_view(cur_buf).substr(head_pos, next_pos));
        }

        if (next_pos == std::string::npos) {
            cur_buf.clear();
            break;
        } else {
            cur_buf.erase(0, next_pos);
        }
    }
    cur_buf.append(route_str);
}

std::string BtreeRouteTracer::get(Op op) const {
    m_append_mtx.lock_shared();
    std::shared_lock< iomgr::FiberManagerLib::shared_mutex > lock{m_append_mtx};
    auto const ret = m_ops_routes[uint32_cast(op)];
    m_append_mtx.unlock_shared();
    return ret;
}

std::vector< std::string > BtreeRouteTracer::get_all() const {
    m_append_mtx.lock_shared();
    auto const ret = m_ops_routes;
    m_append_mtx.unlock_shared();
    return ret;
}

#if 0
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
#endif

} // namespace homestore