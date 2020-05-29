/*
 * fixed_blk_allocator.cpp
 *
 *  Created on: Aug 09, 2016
 *      Author: hkadayam
 */
#include "blk_allocator.h"
#include <cassert>

using namespace std;

namespace homestore {

FixedBlkAllocator::FixedBlkAllocator(BlkAllocConfig& cfg, bool init, uint32_t id) :
        BlkAllocator(cfg, id), m_init(init) {
    m_blk_nodes = new __fixed_blk_node[cfg.get_total_blks()];

    if (m_init) {
        inited();
    }
}

FixedBlkAllocator::~FixedBlkAllocator() {
    delete[](m_blk_nodes);
}

BlkAllocStatus FixedBlkAllocator::alloc(BlkId& in_bid) {
    BlkAllocPortion* portion = blknum_to_portion(in_bid.get_id());
    portion->lock();
    assert(in_bid.get_nblks() == 1);
    if (get_alloced_bm()->is_bits_set(in_bid.get_id(), in_bid.get_nblks())) {
        /* XXX: We need to have better status */
        portion->unlock();
        return BLK_ALLOC_FAILED;
    }
    get_alloced_bm()->set_bits(in_bid.get_id(), in_bid.get_nblks());
    portion->unlock();
    return BLK_ALLOC_SUCCESS;
}

void FixedBlkAllocator::inited() {

    m_first_blk_id = BLKID32_INVALID;
    /* create the blkid chain */
    uint32_t prev_blkid = BLKID32_INVALID;
    for (uint32_t i = 0; i < (uint32_t)m_cfg.get_total_blks(); i++) {
#ifndef NDEBUG
        m_blk_nodes[i].this_blk_id = i;
#endif
        BlkAllocPortion* portion = blknum_to_portion(i);
        portion->lock();
        if (get_alloced_bm()->is_bits_set(i, 1)) {
            portion->unlock();
            continue;
        }
        portion->unlock();
        if (m_first_blk_id == BLKID32_INVALID) { m_first_blk_id = i; }
        if (prev_blkid != BLKID32_INVALID) {
            m_blk_nodes[prev_blkid].next_blk = i;
        }
        prev_blkid = i;
    }
    assert(prev_blkid != BLKID32_INVALID);
    m_blk_nodes[prev_blkid].next_blk = BLKID32_INVALID;

    assert(m_first_blk_id != BLKID32_INVALID);
    __top_blk tp(0, m_first_blk_id);
    m_top_blk_id.store(tp.to_integer());
#ifndef NDEBUG
    m_nfree_blks.store(m_cfg.get_total_blks(), std::memory_order_relaxed);
#endif
    m_init = true;
}

bool FixedBlkAllocator::is_blk_alloced(BlkId& b) {
    /* We need to take lock so we can check in non debug builds */
    if (!m_init) { return true; }
#ifndef NDEBUG
    BlkAllocPortion* portion = blknum_to_portion(b.get_id());
    portion->lock();
    bool status = get_alloced_bm()->is_bits_set(b.get_id(), b.get_nblks());
    portion->unlock();
    return status;
#else
    return true;
#endif
}

BlkAllocStatus FixedBlkAllocator::alloc(uint8_t nblks, const blk_alloc_hints& hints, std::vector< BlkId >& out_blkid) {
    BlkId blkid;
    /* TODO:If it is more then 1 then we need to make sure that we never allocate across the portions */
    assert(nblks == 1);

#ifdef _PRERELEASE
    if (homestore_flip->test_flip("fixed_blkalloc_no_blks", nblks)) {
        return BLK_ALLOC_SPACEFULL;
    }
#endif
    if (alloc(nblks, hints, &blkid) == BLK_ALLOC_SUCCESS) {
        out_blkid.push_back(blkid);
        return BLK_ALLOC_SUCCESS;
    }
    /* We don't support the vector of blkids in fixed blk allocator */
    return BLK_ALLOC_SPACEFULL;
}

BlkAllocStatus FixedBlkAllocator::alloc(uint8_t nblks, const blk_alloc_hints& hints, BlkId* out_blkid, bool best_fit) {
    uint64_t prev_val;
    uint64_t cur_val;
    uint32_t id;

    assert(nblks == 1);
    assert(m_init);
    do {
        prev_val = m_top_blk_id.load();
        __top_blk tp(prev_val);

        // Get the __top_blk blk and replace the __top_blk blk id with next id
        id = tp.get_top_blk_id();
        if (id == BLKID32_INVALID) {
            return BLK_ALLOC_SPACEFULL;
        }

        __fixed_blk_node blknode = m_blk_nodes[id];

        tp.set_top_blk_id(blknode.next_blk);
        tp.set_gen(tp.get_gen() + 1);
        cur_val = tp.to_integer();

    } while (!(m_top_blk_id.compare_exchange_weak(prev_val, cur_val)));

    out_blkid->set(id, 1, 0);

#ifndef NDEBUG
    m_nfree_blks.fetch_sub(1, std::memory_order_relaxed);
#endif
    BlkAllocPortion* portion = blknum_to_portion(out_blkid->get_id());
    portion->lock();
    get_alloced_bm()->set_bits(out_blkid->get_id(), out_blkid->get_nblks());
    portion->unlock();
    return BLK_ALLOC_SUCCESS;
}

void FixedBlkAllocator::free(const BlkId& b, bool set_in_use, bool set_cache) {
    assert(b.get_nblks() == 1);

    if (set_cache) { free_blk((uint32_t)b.get_id()); }
#ifndef NDEBUG
    m_nfree_blks.fetch_add(1, std::memory_order_relaxed);
#endif
    if (set_in_use) {
        BlkAllocPortion* portion = blknum_to_portion(b.get_id());
        portion->lock();
        get_alloced_bm()->reset_bits(b.get_id(), b.get_nblks());
        portion->unlock();
    }
}

void FixedBlkAllocator::free_blk(uint32_t id) {
    uint64_t prev_val;
    uint64_t cur_val;
    __fixed_blk_node* blknode = &m_blk_nodes[id];

    do {
        prev_val = m_top_blk_id.load();
        __top_blk tp(prev_val);

        blknode->next_blk = tp.get_top_blk_id();
        ;

        tp.set_gen(tp.get_gen() + 1);
        tp.set_top_blk_id(id);
        cur_val = tp.to_integer();
    } while (!(m_top_blk_id.compare_exchange_weak(prev_val, cur_val)));
}

std::string FixedBlkAllocator::to_string() const {
    ostringstream oss;
#ifndef NDEBUG
    oss << "Total free blks = " << m_nfree_blks.load(std::memory_order_relaxed) << " ";
#endif
    oss << "m_top_blk_id=" << m_top_blk_id << "\n";
    return oss.str();
}
} // namespace homestore
