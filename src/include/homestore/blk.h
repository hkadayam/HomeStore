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
#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>

#include <boost/icl/interval_map.hpp>
#include <folly/small_vector.h>
#include <sisl/fds/enum.h>
#include <sisl/fds/buffer.h>
#include <homestore/homestore_decl.hpp>

namespace homestore {

using chunk_num_t = uint16_t;
using blk_count_t = uint16_t;
using blk_num_t = uint32_t;
using blk_temp_t = uint16_t;
using allocator_id_t = chunk_num_t;

static constexpr size_t max_addressable_chunks() {
    return 1UL << (8 * sizeof(chunk_num_t));
}
static constexpr size_t max_blks_per_chunk() {
    return 1UL << (8 * sizeof(blk_num_t));
}
static constexpr size_t max_blks_per_blkid() {
    return (1UL << (8 * sizeof(blk_count_t))) - 1;
}

#pragma pack(1)
struct BlkId {
private:
    struct serialized {
        blk_num_t blk_num_;     // Block number which is unique within the chunk
        blk_count_t nblks_;     // Number of blocks+1 for this blkid, don't directly acccess this - use blk_count()
        chunk_num_t chunk_num_; // Chunk number - which is unique for the entire application

        serialized() : blk_num_{0}, nblks_{0}, chunk_num_{0} {}
        serialized(blk_num_t blk_num, blk_count_t nblks, chunk_num_t cnum) :
                blk_num_{blk_num}, nblks_{nblks}, chunk_num_{cnum} {}
    };
    static_assert(sizeof(serialized) == sizeof(uint64_t), "Expected serialized size to 64 bits");

    serialized s;

public:
    BlkId() = default;
    explicit BlkId(uint64_t id_int);
    BlkId(blk_num_t blk_num, blk_count_t nblks, chunk_num_t chunk_num);
    BlkId(BlkId const&) = default;
    BlkId& operator=(BlkId const&) = default;
    BlkId(BlkId&&) noexcept = default;
    BlkId& operator=(BlkId&&) noexcept = default;

    bool operator==(BlkId const& other) const { return (compare(*this, other) == 0); }
    bool operator>(BlkId const& other) const { return (compare(*this, other) > 0); }
    bool operator<(BlkId const& other) const { return (compare(*this, other) < 0); }

    blk_num_t blk_num() const { return s.blk_num_; }
    blk_count_t blk_count() const { return s.nblks_; }
    chunk_num_t chunk_num() const { return s.chunk_num_; }
    std::pair< BlkId, BlkId > split(blk_count_t count) const;

    void invalidate();
    uint64_t to_integer() const;
    sisl::Blob serialize() const;
    void deserialize(sisl::Blob const& b, bool copy);
    uint32_t serialized_size() const;
    std::string to_string() const;
    bool is_valid() const;
    static uint32_t expected_serialized_size();

    static int compare(BlkId const& one, BlkId const& two);
};
#pragma pack()

/// A small collection of BlkIds, stack-allocated for up to 4 pieces.
using BlkIds = folly::small_vector< BlkId, 4 >;

} // namespace homestore

///////////////////// hash function definitions /////////////////////
namespace std {
template <>
struct hash< homestore::BlkId > {
    size_t operator()(const homestore::BlkId& bid) const noexcept { return std::hash< uint64_t >()(bid.to_integer()); }
};

} // namespace std

///////////////////// formatting definitions /////////////////////
template <>
struct fmt::formatter< homestore::BlkId > : fmt::formatter< std::string > {
    auto format(const homestore::BlkId& a, format_context& ctx) const {
        return fmt::formatter< std::string >::format(a.to_string(), ctx);
    }
};

namespace boost {
template <>
struct hash< homestore::BlkId > {
    size_t operator()(const homestore::BlkId& bid) const noexcept { return std::hash< homestore::BlkId >()(bid); }
};
} // namespace boost

namespace homestore {
///////////////////// stream operation definitions /////////////////////
template < typename charT, typename traits, typename blkidT >
std::basic_ostream< charT, traits >& stream_op(std::basic_ostream< charT, traits >& outStream, blkidT const& blk) {
    // copy the stream formatting
    std::basic_ostringstream< charT, traits > outStringStream;
    outStringStream.copyfmt(outStream);

    // print the stream
    outStringStream << blk.to_string();
    outStream << outStringStream.str();

    return outStream;
}

template < typename charT, typename traits >
std::basic_ostream< charT, traits >& operator<<(std::basic_ostream< charT, traits >& outStream, BlkId const& blk) {
    return stream_op< charT, traits, BlkId >(outStream, blk);
}

///////////////////// Other common Blkd definitions /////////////////////
VENUM(BlkAllocStatus, uint32_t,
      BLK_ALLOC_NONE = 0,        // No Action taken
      SUCCESS = 1ul << 0,        // Success
      FAILED = 1ul << 1,         // Failed to alloc/free
      REQ_MORE = 1ul << 2,       // Indicate that we need more
      SPACE_FULL = 1ul << 3,     // Space is full
      INVALID_DEV = 1ul << 4,    // Invalid Device provided for alloc
      PARTIAL = 1ul << 5,        // In case of multiple blks, only partial is alloced/freed
      INVALID_THREAD = 1ul << 6, // Not possible to alloc in this thread
      INVALID_INPUT = 1ul << 7,  // Invalid input
      TOO_MANY_PIECES = 1ul << 8 // Allocation results in more pieces than passed on
);

struct blk_alloc_hints {
    blk_temp_t desired_temp{0};                               // Temperature hint for the device
    std::optional< uint32_t > reserved_blks{std::nullopt};    // Reserved blks in a chunk
    std::optional< uint32_t > pdev_id_hint{std::nullopt};     // which physical device to pick (hint if any)
    std::optional< chunk_num_t > chunk_id_hint{std::nullopt}; // any specific chunk id to pick for this allocation
    std::optional< BlkId > committed_blk_id{
        std::nullopt}; //  blk id indicates the blk was already allocated and committed, don't allocate and commit again
    std::optional< stream_id_t > stream_id_hint{std::nullopt}; // any specific stream to pick
    std::optional< uint64_t > application_hint{
        std::nullopt};                   // hints in uint64 what will be passed opaque to select_chunk
    bool can_look_for_other_chunk{true}; // If alloc on device not available can I pick other device
    bool is_contiguous{true};            // Should the entire allocation be one contiguous block
    bool partial_alloc_ok{false};        // ok to allocate only portion of nblks? Mutually exclusive with is_contiguous
    uint32_t min_blks_per_piece{1};      // blks allocated in a blkid should be atleast this size per entry
    uint32_t max_blks_per_piece{max_blks_per_blkid()};        // Number of blks on every entry
    std::optional< uint32_t > preferred_seg_id{std::nullopt}; // affinity hint for intra-chunk segment selection
};

} // namespace homestore
