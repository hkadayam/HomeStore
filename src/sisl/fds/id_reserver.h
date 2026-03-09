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

#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>

#include "bitset.h"
#include "utils.h"

namespace sisl {
class IDReserver {
public:
    IDReserver(uint32_t estimated_ids = 1024) : reserved_bits_(estimated_ids) { assert(estimated_ids != 0); }

    IDReserver(const sisl::ByteArray& b) : reserved_bits_(b) {}

    uint32_t reserve() {
        std::unique_lock lg(mutex_);
        size_t nbit = reserved_bits_.get_next_reset_bit(0);
        if (nbit == Bitset::npos) {
            const auto cur_size = reserved_bits_.size();
            assert(cur_size != 0);
            reserved_bits_.resize(cur_size * 2);
            nbit = cur_size;
        }
        reserved_bits_.set_bit(nbit);
        return nbit;
    }

    void reserve(uint32_t id) {
        std::unique_lock lg(mutex_);
        assert(!(reserved_bits_.get_bitval(id)));
        assert(id < reserved_bits_.size());
        reserved_bits_.set_bit(id);
    }

    void unreserve(uint32_t id) {
        std::unique_lock lg(mutex_);
        assert(id < reserved_bits_.size());
        reserved_bits_.reset_bit(id);
    }

    bool is_reserved(uint32_t id) {
        std::unique_lock lg(mutex_);
        return reserved_bits_.get_bitval(id);
    }

    sisl::ByteArray serialize() {
        std::unique_lock lg(mutex_);
        return reserved_bits_.serialize();
    }

    bool first_reserved_id(uint32_t& found_id) { return find_next_reserved_id(true, found_id); }
    bool next_reserved_id(uint32_t& last_found_id) { return find_next_reserved_id(false, last_found_id); }

private:
    bool find_next_reserved_id(bool first, uint32_t& last_found_id) {
        std::unique_lock lg(mutex_);
        size_t nbit = reserved_bits_.get_next_set_bit(first ? 0 : last_found_id + 1);
        if (nbit == Bitset::npos) return false;
        last_found_id = (uint32_t)nbit;
        return true;
    }

private:
    std::mutex mutex_;
    sisl::Bitset reserved_bits_;
};
} // namespace sisl
