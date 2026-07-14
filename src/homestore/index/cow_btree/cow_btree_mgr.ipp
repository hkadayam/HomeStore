/*********************************************************************************
 * Copyright 2024-2026 Harihara Kadayam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *********************************************************************************/
#pragma once

#include <algorithm>
#include "common/async.h"
#include <cstring>

#include "homestore/index/cow_btree/cow_btree.h"
#include "homestore/index/cow_btree/cow_btree_mgr.h"
#include "homestore/index/btree/btree.h"
#include "homestore/index/btree/btree.ipp"
#include "homestore/meta/meta_blk.h"

namespace homestore {

/// Allocates a fresh COWBtreeSuperBlock-bearing MetaBlk under the manager-wide MetaClient, hands it to
/// COWBtree::create() (which fills in stream IDs and persists), then wraps the resulting UnderlyingBtree in a
/// Btree<K,V>. Empty root_node_id triggers Btree<K,V>'s fresh-boot path which calls create_root_node().
template < typename K, typename V >
Async< shared< Btree< K, V > > > COWBtreeManager::create_cow_btree(BtreeConfig const& cfg, shared< BlobDev > blob_dev,
                                                                   sisl::Blob const& user_sb) {
    auto const ordinal = ordinal_reserver_.reserve();

    auto const sb_size = sizeof(COWBtreeSuperBlock) + user_sb.size();
    auto mblk = co_await MetaBlkWrapper::create(meta_client_, cfg.name(), sb_size);

    auto& sb = *r_cast< COWBtreeSuperBlock* >(mblk.meta_blk().inline_data());
    sb = COWBtreeSuperBlock{};
    sb.ordinal = ordinal;
    sb.node_size = cfg.node_size();
    sb.set_btree_name(cfg.name());
    sb.user_sb_size = to_u32(user_sb.size());
    if (user_sb.size() > 0) {
        std::memcpy(sb.user_sb_data(), user_sb.cbytes(), user_sb.size());
    }

    auto cow_bt = co_await COWBtree::create(*this, std::move(blob_dev), std::move(mblk), node_cache_, overflow_cache_);
    auto btree = std::make_shared< Btree< K, V > >(cfg, std::move(cow_bt));

    track(btree);
    co_return btree;
}

/// Finds the matching MetaBlk in pending_btrees_ (populated by COWBtreeManager::load() at startup), reattaches it to
/// the manager's MetaClient, hands off to COWBtree::load() (opens streams from sb.*_stream_id and runs recovery), then
/// wraps in a Btree<K,V> seeded with the persisted root.
template < typename K, typename V >
Async< shared< Btree< K, V > > > COWBtreeManager::load_cow_btree(BtreeConfig const& cfg, shared< BlobDev > blob_dev,
                                                                 COWBtreeSuperBlock const& sb) {
    auto it = std::find_if(pending_btrees_.begin(), pending_btrees_.end(),
                           [&](PersistedBtreeInfo const& info) { return info.sb.ordinal == sb.ordinal; });
    HS_REL_ASSERT(it != pending_btrees_.end(), "load_cow_btree: no persisted MetaBlk for ordinal={}", sb.ordinal);

    auto mblk = MetaBlkWrapper::load(meta_client_, std::move(it->mblk));

    auto cow_bt = co_await COWBtree::load(*this, std::move(blob_dev), std::move(mblk), node_cache_, overflow_cache_);
    auto btree = std::make_shared< Btree< K, V > >(cfg, std::move(cow_bt), sb.root_node_id);

    track(btree);
    pending_btrees_.erase(it);
    co_return btree;
}

} // namespace homestore