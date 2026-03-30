#pragma once

// ReactorLocal<T> — per-reactor storage.
//
// Each reactor gets its own T, accessed by reactor_id as a plain array index.
// No locks, no atomics on the read path — only the owning reactor thread
// should read/write its own slot.

#include <cassert>
#include <functional>
#include <memory>
#include <vector>

#include "iomanager.h"

namespace homestore {

template < typename T >
class ReactorLocal {
public:
    // Construct with an initializer called once per reactor slot.
    explicit ReactorLocal(std::function< T() > init) {
        size_t n = iomgr().num_reactors();
        slots_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            slots_.emplace_back(std::make_unique< Slot >(init()));
        }
    }

    // Construct from a pre-built vector of values, one per reactor.
    explicit ReactorLocal(std::vector< T > values) {
        assert(values.size() == iomgr().num_reactors());
        slots_.reserve(values.size());
        for (auto& v : values) {
            slots_.emplace_back(std::make_unique< Slot >(std::move(v)));
        }
    }

    // Access the current reactor's value.
    // Must be called from a reactor thread.
    T& get() {
        size_t rid = iomgr().current_reactor_id();
        assert(rid < slots_.size());
        return slots_[rid]->value;
    }

    const T& get() const {
        size_t rid = iomgr().current_reactor_id();
        assert(rid < slots_.size());
        return slots_[rid]->value;
    }

    // Access a specific reactor's value.
    // Caller must ensure the target reactor is not concurrently accessing its slot.
    T& get_for_reactor(size_t reactor_id) {
        assert(reactor_id < slots_.size());
        return slots_[reactor_id]->value;
    }

    const T& get_for_reactor(size_t reactor_id) const {
        assert(reactor_id < slots_.size());
        return slots_[reactor_id]->value;
    }

    size_t num_reactors() const { return slots_.size(); }

private:
    // Each slot is heap-allocated to prevent false sharing between reactors.
    // alignas(64) ensures each slot occupies its own cache line(s).
    struct alignas(64) Slot {
        explicit Slot(T v) : value(std::move(v)) {}
        T value;
    };

    std::vector< std::unique_ptr< Slot > > slots_;
};

} // namespace homestore
