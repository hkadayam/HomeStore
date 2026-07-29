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

#include <boost/uuid/random_generator.hpp>

#include "hs_runtime_config.h"
#include "sisl/fds/buffer.h"

namespace homestore {
template < typename T >
std::string to_hex(T i) {
    return fmt::format("{0:x}", i);
}

class hs_utils {
    static size_t m_btree_mempool_size;

public:
    static uint8_t* iobuf_alloc(const size_t size, const sisl::Buftag tag, const size_t alignment);
    static void iobuf_free(uint8_t* const ptr, const sisl::Buftag tag);
    static void set_btree_mempool_size(const size_t size);
    static void iobuf_free(uint8_t* const ptr, const sisl::Buftag tag, const size_t size);
    static uint64_t aligned_size(const size_t size, const size_t alignment);
    static bool mod_aligned_sz(const size_t size_to_check, const size_t align_sz);
    static bool is_ptr_aligned(void* ptr, std::size_t alignment);
    static sisl::IoBufView create_byte_view(const uint64_t size, const bool is_aligned_needed, const sisl::Buftag tag,
                                            const size_t alignment);
    static sisl::IoBufSpan create_io_blob(const uint64_t size, const bool is_aligned_needed, const sisl::Buftag tag,
                                        const size_t alignment);
    static sisl::IoBufShared extract_byte_array(const sisl::IoBufView& b, const bool is_aligned_needed,
                                               const size_t alignment);
    static sisl::IoBufShared make_io_buf_shared(const uint64_t size, const bool is_aligned_needed, const sisl::Buftag tag,
                                            const size_t alignment);
    // Inlined here (rather than in homestore_utils.cpp, which is not compiled — it still uses the pre-port iomanager
    // API) so callers don't need that translation unit just for a random uuid.
    static uuid_t gen_random_uuid() { return boost::uuids::random_generator()(); }

    /**
     * @brief  given a DAG graph , build the partial order sequence.
     *
     * @return true if the DAG has a circle ,or false if not.
     */
    static bool topological_sort(std::unordered_map< std::string, std::vector< std::string > >& DAG,
                                 std::vector< std::string >& ordered_entries);
};

static bool wait_and_check(const std::function< bool() >& check_func, uint32_t timeout_ms,
                           uint32_t interval_ms = 100);

} // namespace homestore
