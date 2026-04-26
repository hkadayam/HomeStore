#pragma once

#include <chrono>
#include <mutex>
#include <vector>

#include <folly/executors/FunctionScheduler.h>

namespace homestore {

class MemBtree;

class MemBtreeDrainer {
public:
    static MemBtreeDrainer& instance();

    void register_(MemBtree* bt);
    void deregister(MemBtree* bt);

private:
    MemBtreeDrainer();
    ~MemBtreeDrainer();
    void tick();

    static constexpr std::chrono::milliseconds tick_interval_{10};

    folly::FunctionScheduler fs_;
    std::mutex m_;
    std::vector< MemBtree* > registered_;
};

} // namespace homestore