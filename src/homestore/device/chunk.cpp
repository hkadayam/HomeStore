/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

#include <fmt/format.h>

#include "device/chunk.h"
#include "device/physical_dev.h"

namespace homestore {

// Mirrors Rust's Chunk::new(chunk_info, chunk_slot, pdev).
Chunk::Chunk(ChunkInfo info, uint32_t chunk_slot, std::shared_ptr< PhysicalDev > pdev) :
        chunk_info_{std::move(info)}, chunk_slot_{chunk_slot}, pdev_{std::move(pdev)} {}

// Mirrors Rust's Chunk::to_string().
// Format: "Chunk[id={}, slot={}, offset={}, size={}]"
std::string Chunk::to_string() const {
    // Copy packed fields to local vars first to avoid UB from unaligned reads
    // (chunk_info_ is #pragma pack(1)).
    const uint32_t id     = chunk_info_.chunk_id;
    const uint64_t offset = chunk_info_.chunk_start_offset;
    const uint64_t sz     = chunk_info_.chunk_size;

    return fmt::format("Chunk[id={}, slot={}, offset={}, size={}]",
                       id, chunk_slot_, offset, sz);
}

} // namespace homestore
