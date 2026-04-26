/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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

#include <mutex>
#include <shared_mutex>

#include <folly/SharedMutex.h>
#include <sisl/metrics/metrics_group_impl.h>
#include <sisl/metrics/metrics.h>

#include "bitset.h"
#include "common/defs.h"

namespace sisl {
class StreamTrackerMetrics : public MetricsGroup {
public:
    explicit StreamTrackerMetrics(const char* inst_name) : MetricsGroupWrapper("StreamTracker", inst_name) {
        REGISTER_COUNTER(stream_tracker_unsweeped_completions, "How many completions are unsweeped yet", "", {"", ""},
                         PublishAs::Gauge);

        REGISTER_GAUGE(stream_tracker_mem_size, "Total Memsize for stream tracker");
        REGISTER_GAUGE(stream_tracker_completed_upto, "Idx upto which stream tracker cursor is completed");

        register_me_to_farm();
    }
    ~StreamTrackerMetrics() { deregister_me_from_farm(); }
};

template < typename T, bool AutoTruncate = false, bool TrackCompletion = true >
class StreamTracker {
public:
    static constexpr size_t alloc_blk_size = 10000;
    static constexpr size_t compaction_threshold = alloc_blk_size / 2;
    static constexpr auto null_processor = []([[maybe_unused]] auto... x) -> bool { return true; };

    static_assert(std::is_trivially_copyable< T >::value, "Cannot use StreamTracker for non-trivally copyable classes");

    StreamTracker(const char* name = "StreamTracker", int64_t start_idx = -1) :
            slot_ref_idx_(start_idx + 1),
            slot_data_(static_cast< T* >(std::calloc(alloc_blk_size, sizeof(T)))),
            alloced_slots_(alloc_blk_size),
            active_slot_bits_(alloc_blk_size),
            comp_slot_bits_(TrackCompletion ? alloc_blk_size : 0),
            metrics_(name) {
        GAUGE_UPDATE(metrics_, stream_tracker_mem_size, (alloced_slots_ * sizeof(T)));
    }

    ~StreamTracker() {
        GAUGE_UPDATE(metrics_, stream_tracker_mem_size, 0);
        free(slot_data_);
    }

    void reinit(int64_t start_idx) { slot_ref_idx_ = start_idx; }

    template < class... Args >
    int64_t create_and_complete(int64_t idx, Args&&... args) {
        static_assert(TrackCompletion, "create_and_complete requires TrackCompletion=true");
        return do_update(idx, null_processor, true /* replace */, std::forward< Args >(args)...);
    }

    template < class... Args >
    int64_t create(int64_t idx, Args&&... args) {
        return do_update(
            idx, []([[maybe_unused]] T& data) { return false; }, true /* replace */, std::forward< Args >(args)...);
    }

    template < class... Args >
    int64_t update(int64_t idx, const auto& processor, Args&&... args) {
        return do_update(idx, processor, false /* replace */, std::forward< Args >(args)...);
    }

    void complete(int64_t start_idx, int64_t end_idx) {
        if constexpr (TrackCompletion) {
            std::shared_lock holder(lock_);
            auto start_bit = start_idx - slot_ref_idx_;
            comp_slot_bits_.set_bits(start_bit, end_idx - start_idx + 1);
        }
    }

    void rollback(int64_t new_end_idx) {
        std::shared_lock holder(lock_);
        if ((new_end_idx < slot_ref_idx_) || (new_end_idx >= (slot_ref_idx_ + int64_cast(active_slot_bits_.size())))) {
            throw std::out_of_range("Slot idx is not in range");
        }

        auto new_end_bit = new_end_idx - slot_ref_idx_;
        active_slot_bits_.reset_bits(new_end_bit + 1, active_slot_bits_.size() - new_end_bit - 1);
        if constexpr (TrackCompletion) {
            comp_slot_bits_.reset_bits(new_end_bit + 1, comp_slot_bits_.size() - new_end_bit - 1);
        }
    }

    T& at(int64_t idx) const {
        std::shared_lock holder(lock_);
        if (idx < slot_ref_idx_) {
            throw std::out_of_range("Slot idx is not in range");
        }

        size_t nbit = idx - slot_ref_idx_;
        if (!active_slot_bits_.get_bitval(nbit)) {
            throw std::out_of_range("Slot idx is not in range");
        }
        return *get_slot_data(nbit);
    }

    auto status(int64_t idx) const {
        struct {
            bool is_out_of_range = false;
            bool is_hole = false;
            bool is_active = false;
            bool is_completed = false;
        } ret;

        std::shared_lock holder(lock_);
        if (idx < slot_ref_idx_) {
            ret.is_out_of_range = true;
        } else {
            size_t nbit = idx - slot_ref_idx_;
            if constexpr (TrackCompletion) {
                if (comp_slot_bits_.get_bitval(nbit)) {
                    ret.is_completed = true;
                } else if (active_slot_bits_.get_bitval(nbit)) {
                    ret.is_active = true;
                } else {
                    ret.is_hole = true;
                }
            } else {
                if (active_slot_bits_.get_bitval(nbit)) {
                    ret.is_active = true;
                } else {
                    ret.is_hole = true;
                }
            }
        }
        return ret;
    }

    size_t truncate(int64_t idx) {
        std::unique_lock holder(lock_);
        auto upto_bit = idx - slot_ref_idx_ + 1;
        if (upto_bit <= 0) {
            return slot_ref_idx_ - 1;
        }
        return do_truncate(upto_bit);
    }

    size_t truncate() {
        if constexpr (AutoTruncate) {
            if (cmpltd_count_since_last_truncate_.load(std::memory_order_acquire) == 0) { return 0; }
        }

        std::unique_lock holder(lock_);
        if constexpr (TrackCompletion) {
            auto first_incomplete_bit = comp_slot_bits_.get_next_reset_bit(0);
            if (first_incomplete_bit == AtomicBitset::npos) {
                first_incomplete_bit = alloced_slots_;
            } else if (first_incomplete_bit == 0) {
                return slot_ref_idx_ - 1;
            }
            return do_truncate(first_incomplete_bit);
        } else {
            // Without completion tracking, truncate all active slots
            auto first_inactive_bit = active_slot_bits_.get_next_reset_bit(0);
            if (first_inactive_bit == AtomicBitset::npos) {
                first_inactive_bit = alloced_slots_;
            } else if (first_inactive_bit == 0) {
                return slot_ref_idx_ - 1;
            }
            return do_truncate(first_inactive_bit);
        }
    }

    size_t do_truncate(int64_t upto_bit) {
        if constexpr (TrackCompletion) { comp_slot_bits_.shrink_head(upto_bit); }
        active_slot_bits_.shrink_head(upto_bit);

        data_skip_count_ += upto_bit;
        alloced_slots_ -= upto_bit;
        if (data_skip_count_ > compaction_threshold) {
            std::memmove((void*)&slot_data_[0], (void*)&slot_data_[data_skip_count_], (sizeof(T) * alloced_slots_));
            data_skip_count_ = 0;
        }

        slot_ref_idx_ += upto_bit;
        COUNTER_DECREMENT(metrics_, stream_tracker_unsweeped_completions, upto_bit);
        return slot_ref_idx_ - 1;
    }

    void foreach_contiguous_completed(int64_t start_idx, const auto& cb) {
        static_assert(TrackCompletion, "foreach_contiguous_completed requires TrackCompletion=true");
        _foreach_contiguous(start_idx, true, cb);
    }
    void foreach_contiguous_active(int64_t start_idx, const auto& cb) { _foreach_contiguous(start_idx, false, cb); }
    void foreach_all_completed(int64_t start_idx, const auto& cb) {
        static_assert(TrackCompletion, "foreach_all_completed requires TrackCompletion=true");
        _foreach_all(start_idx, true, cb);
    }
    void foreach_all_active(int64_t start_idx, const auto& cb) { _foreach_all(start_idx, false, cb); }

    int64_t completed_upto(int64_t search_hint_idx = 0) const {
        static_assert(TrackCompletion, "completed_upto requires TrackCompletion=true");
        std::shared_lock holder(lock_);
        return _upto(true /* completed */, search_hint_idx);
    }

    int64_t active_upto(int64_t search_hint_idx = 0) const {
        std::shared_lock holder(lock_);
        return _upto(false /* completed */, search_hint_idx);
    }

    nlohmann::json get_status(const int verbosity) const {
        nlohmann::json js;
        js["start"] = slot_ref_idx_;
        if constexpr (TrackCompletion) { js["completed_upto"] = completed_upto(); }
        js["active_upto"] = active_upto();

        if (verbosity == 2) {
            js["alloced_count"] = alloced_slots_;
            if (AutoTruncate) {
                js["completed_since_last_truncate"] = cmpltd_count_since_last_truncate_.load(std::memory_order_relaxed);
            }
            js["truncate_frequency"] = truncate_on_count_;
            js["garbage_count"] = data_skip_count_;
        }
        return js;
    }

private:
    template < class... Args >
    int64_t do_update(int64_t idx, const auto& processor, bool replace, Args&&... args) {
        bool need_truncate = false;
        int64_t ret = 0;
        size_t nbit;

        do {
            lock_.lock_shared();

            if (idx < slot_ref_idx_) {
                ret = slot_ref_idx_ - 1;
                lock_.unlock_shared();
                return ret;
            }

            nbit = idx - slot_ref_idx_;
            if (nbit >= alloced_slots_) {
                lock_.unlock_shared();
                do_resize(nbit + 1);
            } else {
                break;
            }
        } while (true);

        T* data;
        if (replace || !active_slot_bits_.get_bitval(nbit)) {
            data = new ((void*)get_slot_data(nbit)) T(std::forward< Args >(args)...);
            active_slot_bits_.set_bit(nbit);
        } else {
            data = get_slot_data(nbit);
        }

        if (processor(*data)) {
            if constexpr (TrackCompletion) { comp_slot_bits_.set_bit(nbit); }
            if constexpr (AutoTruncate) {
                if (cmpltd_count_since_last_truncate_.fetch_add(1, std::memory_order_acq_rel) >= truncate_on_count_) {
                    need_truncate = true;
                }
            }
            COUNTER_INCREMENT(metrics_, stream_tracker_unsweeped_completions, 1);
        }
        ret = slot_ref_idx_ - 1;
        lock_.unlock_shared();

        if (need_truncate) {
            ret = truncate();
        }
        return ret;
    }

    void do_resize(size_t atleast_count) {
        std::unique_lock holder(lock_);
        if (atleast_count < alloced_slots_) {
            return;
        }

        auto new_count = std::max((alloced_slots_ * 2), atleast_count);
        auto new_slot_data = (T*)std::calloc(new_count, sizeof(T));
        if (new_slot_data == nullptr) {
            throw std::bad_alloc();
        }

        std::memmove((void*)&new_slot_data[0], (void*)&slot_data_[data_skip_count_], (sizeof(T) * alloced_slots_));
        free(slot_data_);
        slot_data_ = new_slot_data;
        alloced_slots_ = new_count;
        data_skip_count_ = 0;

        active_slot_bits_.resize(new_count);
        if constexpr (TrackCompletion) { comp_slot_bits_.resize(new_count); }

        GAUGE_UPDATE(metrics_, stream_tracker_mem_size, (alloced_slots_ * sizeof(T)));
    }

    int64_t _upto(bool completed, int64_t search_hint_idx) const {
        auto search_start_bit = std::max(to_i64(0), (search_hint_idx - slot_ref_idx_));
        auto first_incomplete_bit = completed ? comp_slot_bits_.get_next_reset_bit(search_start_bit)
                                              : active_slot_bits_.get_next_reset_bit(search_start_bit);
        if (first_incomplete_bit == AtomicBitset::npos) {
            return slot_ref_idx_ + alloced_slots_ - 1;
        } else {
            return slot_ref_idx_ + first_incomplete_bit - 1;
        }
    }

    void _foreach_contiguous(int64_t start_idx, bool completed_only, const auto& cb) {
        std::shared_lock holder(lock_);
        auto upto = _upto(completed_only, start_idx);
        for (auto idx = start_idx; idx <= upto; ++idx) {
            auto proceed = cb(idx, upto, *(get_slot_data(idx - slot_ref_idx_)));
            if (!proceed)
                break;
        }
    }

    void _foreach_all(int64_t start_idx, bool completed_only, const auto& cb) {
        std::shared_lock holder(lock_);
        auto search_bit = std::max(to_i64(0), (start_idx - slot_ref_idx_));
        do {
            search_bit = completed_only ? comp_slot_bits_.get_next_set_bit(search_bit)
                                        : active_slot_bits_.get_next_set_bit(search_bit);
            if (search_bit == AtomicBitset::npos) {
                break;
            }
            if (!cb(search_bit + slot_ref_idx_, *(get_slot_data(search_bit)))) {
                break;
            }
            ++search_bit;
        } while (true);
    }

    T* get_slot_data(int64_t nbit) const { return &(slot_data_[nbit + data_skip_count_]); }

private:
    // Hot on every append (read-only except on rare truncation/resize)
    int64_t slot_ref_idx_{0};
    T* slot_data_{nullptr};
    size_t data_skip_count_{0};
    size_t alloced_slots_{0};

    // Lock — separate from above since exclusive acquire (rare) modifies it
    mutable folly::SharedMutexWritePriority lock_;

    // Bitsets — accessed via pointer indirection (words are in separate allocation)
    sisl::AtomicBitset active_slot_bits_;
    sisl::AtomicBitset comp_slot_bits_; // size=0 when TrackCompletion=false

    // Cold: AutoTruncate-only, metrics
    std::atomic< size_t > cmpltd_count_since_last_truncate_{0};
    uint32_t truncate_on_count_{1000};
    StreamTrackerMetrics metrics_;
};
} // namespace sisl
