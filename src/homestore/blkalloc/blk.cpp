/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
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
#include <bit>
#include <homestore/blk.h>
#include "base/homestore_assert.hpp"

namespace homestore {
BlkId::BlkId(uint64_t id_int) {
    s = std::bit_cast< serialized >(id_int);
}

BlkId::BlkId(blk_num_t blk_num, blk_count_t nblks, chunk_num_t chunk_num) : s{blk_num, nblks, chunk_num} {
}

uint64_t BlkId::to_integer() const {
    return std::bit_cast< uint64_t >(s);
}

sisl::Blob BlkId::serialize() const {
    return sisl::Blob{r_cast< uint8_t const* >(&s), sizeof(serialized)};
}

uint32_t BlkId::serialized_size() const {
    return sizeof(BlkId);
}
uint32_t BlkId::expected_serialized_size() {
    return sizeof(BlkId);
}

void BlkId::deserialize(sisl::Blob const& b, bool copy) {
    const serialized* other = r_cast< serialized const* >(b.cbytes());
    s = *other;
}

void BlkId::invalidate() {
    s.nblks_ = 0;
}

bool BlkId::is_valid() const {
    return (blk_count() > 0);
}

std::pair< BlkId, BlkId > BlkId::split(blk_count_t count) const {
    BlkId lb{blk_num(), count, chunk_num()};
    BlkId rb{blk_num() + count, (blk_count_t)(blk_count() - count), chunk_num()};
    return std::pair(lb, rb);
}

std::string BlkId::to_string() const {
    return is_valid() ? fmt::format("blk#={} count={} chunk={}", blk_num(), blk_count(), chunk_num()) : "Invalid_Blkid";
}

int BlkId::compare(const BlkId& one, const BlkId& two) {
    if (one.chunk_num() < two.chunk_num()) {
        return -1;
    } else if (one.chunk_num() > two.chunk_num()) {
        return 1;
    }

    if (one.blk_num() < two.blk_num()) {
        return -1;
    } else if (one.blk_num() > two.blk_num()) {
        return 1;
    }

    if (one.blk_count() < two.blk_count()) {
        return -1;
    } else if (one.blk_count() > two.blk_count()) {
        return 1;
    }

    return 0;
}
} // namespace homestore
