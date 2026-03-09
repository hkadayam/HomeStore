/*********************************************************************************
 *
 * Author/Developer(s): Harihara Kadayam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>

namespace sisl {

// Internal packed storage: counter (32-bit) | status (StatusType) in one 64-bit word.
#pragma pack(1)
template < typename StatusType, const StatusType DefaultVal >
struct StatusCounter {
    using CounterType = int32_t;
    using status_type = std::decay_t< StatusType >;
    static_assert(sizeof(CounterType) + sizeof(status_type) <= sizeof(uint64_t),
                  "Sizes of class must be contained in uint64_t");

    CounterType counter{0};
    status_type status{DefaultVal};

    StatusCounter(const CounterType cnt = 0, const status_type s = DefaultVal) : counter{cnt}, status{s} {}

    operator uint64_t() const { return to_integer(); }
    uint64_t to_integer() const {
        return static_cast< uint64_t >(counter) | (static_cast< uint64_t >(status) << (sizeof(CounterType) * 8));
    }
};
#pragma pack()

/*
 * AtomicStatusCounter
 *
 * Atomically maintains a 32-bit counter and a status (enum or small integer)
 * packed into a single 64-bit word.  All mutations go through a CAS loop so
 * reads and compound read-modify-write operations are always consistent.
 *
 * Counter range: [-2^31, 2^31); behaviour outside that range is undefined.
 */
template < typename StatusType, const StatusType DefaultVal >
struct AtomicStatusCounter {
    using Inner = StatusCounter< StatusType, DefaultVal >;
    using CounterType = typename Inner::CounterType;
    using status_type = typename Inner::status_type;

    std::atomic< Inner > val_;

    explicit AtomicStatusCounter(const CounterType counter = 0, const status_type status = DefaultVal) :
            val_{Inner{counter, status}} {}

    // ── Reads ─────────────────────────────────────────────────────────────────

    status_type get_status() const { return val_.load(std::memory_order_acquire).status; }
    CounterType count() const { return val_.load(std::memory_order_acquire).counter; }

    std::pair< status_type, CounterType > get_status_count() const {
        const auto v{val_.load(std::memory_order_acquire)};
        return {v.status, v.counter};
    }

    // ── Status mutations ──────────────────────────────────────────────────────

    void set_status(const status_type status) {
        update([status](Inner& v) { v.status = status; });
    }

    // Set status to new_status only if current status == exp_status.
    void xchng_status(const status_type exp_status, const status_type new_status) {
        update([exp_status, new_status](Inner& v) {
            if (v.status == exp_status) { v.status = new_status; }
        });
    }

    // ── Counter mutations ─────────────────────────────────────────────────────

    void increment(const CounterType count = 1) {
        update([count](Inner& v) { v.counter += count; });
    }

    void decrement(const CounterType count = 1) {
        update([count](Inner& v) { v.counter -= count; });
    }

    void set_counter(const CounterType count) {
        update([count](Inner& v) { v.counter = count; });
    }

    // Decrement and return true if the counter reached 0.
    bool decrement_testz(const CounterType count = 1) {
        return update([count](Inner& v) { v.counter -= count; }).counter == 0;
    }

    // ── Compound counter + status ─────────────────────────────────────────────

    // Decrement; if counter now == 0 and status == exp_status, set new_status.
    // Returns true if counter reached 0.
    bool dec_xchng_status_ifz(const status_type exp_status, const status_type new_status) {
        return update([exp_status, new_status](Inner& v) {
                   --v.counter;
                   if ((v.counter == 0) && (v.status == exp_status)) { v.status = new_status; }
               }).counter == 0;
    }

    // Decrement only if counter == 1 and status == exp_status; if so, set new_status.
    // Returns true if the decrement+swap actually happened (counter is now 0).
    bool dec_xchng_status_only_ifz(const status_type exp_status, const status_type new_status) {
        return update([exp_status, new_status](Inner& v) {
                   if ((v.counter == 1) && (v.status == exp_status)) {
                       --v.counter;
                       v.status = new_status;
                   }
               }).counter == 0;
    }

    // Increment only if current status == exp_status.  Returns true if it did.
    bool increment_if_status(const status_type exp_status) {
        return update([exp_status](Inner& v) {
                   if (v.status == exp_status) { ++v.counter; }
               }).status == exp_status;
    }

    // Decrement; return true if (counter == 0) && (status == exp_status).
    bool decrement_testz_and_test_status(const status_type exp_status) {
        const auto r{update([](Inner& v) { --v.counter; })};
        return (r.counter == 0) && (r.status == exp_status);
    }

    // Decrement; set status to new_status unconditionally; return true if counter == 0.
    bool dec_set_status_ifz(const status_type new_status) {
        return update([new_status](Inner& v) {
                   --v.counter;
                   v.status = new_status;
               }).counter == 0;
    }

    // ── Escape hatch — caller controls the update ─────────────────────────────

    // Modifier receives (counter&, status&).  Return false to abort the update.
    // Returns true if the modifier committed a change.
    bool set_atomic_value(const std::function< bool(CounterType&, status_type&) >& modifier) {
        Inner old_v, new_v;
        bool updated{true};
        do {
            old_v = val_.load(std::memory_order_acquire);
            new_v = old_v;
            if (!modifier(new_v.counter, new_v.status)) {
                updated = false;
                break;
            }
        } while (!val_.compare_exchange_weak(old_v, new_v, std::memory_order_acq_rel));
        return updated;
    }

private:
    Inner update(const std::function< void(Inner&) >& modifier) {
        Inner old_v, new_v;
        do {
            old_v = val_.load(std::memory_order_acquire);
            new_v = old_v;
            modifier(new_v);
        } while (!val_.compare_exchange_weak(old_v, new_v, std::memory_order_acq_rel));
        return new_v;
    }
};

} // namespace sisl
