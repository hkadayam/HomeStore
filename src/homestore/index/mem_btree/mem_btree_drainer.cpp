#include "homestore/index/mem_btree/mem_btree_drainer.h"
#include "homestore/index/mem_btree/mem_btree.h"

#include <algorithm>

namespace homestore {

MemBtreeDrainer& MemBtreeDrainer::instance() {
    static MemBtreeDrainer s_instance;
    return s_instance;
}

MemBtreeDrainer::MemBtreeDrainer() {
    fs_.addFunction([this] { tick(); }, tick_interval_, "mem_btree_drain");
    fs_.start();
}

MemBtreeDrainer::~MemBtreeDrainer() {
    fs_.shutdown();
}

void MemBtreeDrainer::register_(MemBtree* bt) {
    std::lock_guard lk{m_};
    registered_.push_back(bt);
}

void MemBtreeDrainer::deregister(MemBtree* bt) {
    std::lock_guard lk{m_};
    registered_.erase(std::remove(registered_.begin(), registered_.end(), bt), registered_.end());
    bt->drain();
}

void MemBtreeDrainer::tick() {
    std::lock_guard lk{m_};
    for (auto* bt : registered_) {
        bt->drain();
    }
}

} // namespace homestore
