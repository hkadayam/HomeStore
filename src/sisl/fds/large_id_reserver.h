#pragma once

#include <cassert>
#include <cstdint>
#include <limits>

#include <boost/icl/interval_set.hpp>

namespace sisl {

/// LargeIDReserver — reserves unique IDs from a large sparse domain using
/// boost::icl::interval_set.  Unlike IDReserver (which uses a dense Bitset),
/// this is efficient when the ID space is very large but only a small fraction
/// of IDs are active at any given time (e.g. btree node IDs).
///
/// Not thread-safe; callers must provide external synchronisation.
class LargeIDReserver {
private:
    using IntervalSet = boost::icl::interval_set< uint64_t >;
    using Interval = IntervalSet::interval_type;

    IntervalSet iset_;
    uint64_t max_;

public:
    explicit LargeIDReserver(uint64_t max_count) : max_{max_count} {}
    ~LargeIDReserver() = default;

    LargeIDReserver(LargeIDReserver const&) = delete;
    LargeIDReserver& operator=(LargeIDReserver const&) = delete;
    LargeIDReserver(LargeIDReserver&&) noexcept = default;
    LargeIDReserver& operator=(LargeIDReserver&&) noexcept = default;

    static constexpr uint64_t out_of_bounds = std::numeric_limits< uint64_t >::max();

    /// Reserve the next available ID.  Returns out_of_bounds if the space is exhausted.
    uint64_t reserve() {
        uint64_t id = find_next();
        if (id >= max_) {
            return out_of_bounds;
        }
        iset_.insert(Interval::right_open(id, id + 1));
        return id;
    }

    /// Explicitly reserve a specific ID.  Idempotent — safe to call on an already-reserved ID.
    void reserve(uint64_t id) { iset_.insert(Interval::right_open(id, id + 1)); }

    /// Release a previously reserved ID back into the free pool.
    void unreserve(uint64_t id) {
        assert(id < max_ && "Unreserving an id which was out of bounds");
        iset_.erase(Interval::right_open(id, id + 1));
    }

    /// Check whether an ID is currently reserved.
    bool is_reserved(uint64_t id) const { return (iset_.find(id) != iset_.end()); }

    /// Number of currently reserved IDs.
    uint64_t reserved_count() const { return boost::icl::length(iset_); }

    /// Maximum allowed ID count.
    uint64_t max_count() const { return max_; }

private:
    /// Find the lowest unreserved ID.  Scans from 0 upward, skipping over reserved intervals.
    uint64_t find_next() const {
        uint64_t next = 0;
        for (auto it = iset_.begin(); it != iset_.end(); ++it) {
            if (it->lower() != 0 && next < it->lower()) {
                break;
            }
            next = it->upper();
        }
        return next;
    }
};

} // namespace sisl
