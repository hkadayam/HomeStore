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
 ***************************************************************************/
#pragma once

#include <cstdint>

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Compile-time configuration
//
// System-wide structural limits baked into the on-disk superblock layout.  Changing any of these requires a
// persistent-structure version bump, so they are compile-time constants — NOT runtime config (that is the
// ResourceLimits table in the .fbs) and NOT per-mount inputs (that is InputParams).  Dependency-free (std only)
// so the lowest layers in the stack (blkalloc, device) can include it directly.
//
// NOTE: the BlkId on-disk encoding (blk_num / nblks / chunk_num widths) is owned by blk.h — via its typedefs
// (blk_num_t, blk_count_t, chunk_num_t) + static_assert, and the derived max_addressable_chunks() /
// max_blks_per_chunk() / max_blks_per_blkid() helpers.  Do NOT restate those bit widths here.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

// Per-pdev chunk-id stride: a chunk's global physical id is (pdev_id * MAX_CHUNKS_IN_SYSTEM + slot).
constexpr uint32_t MAX_CHUNKS_IN_SYSTEM{65536};

// Maximum vdevs in the system.  Increasing this grows the vdev bitmap + info area in the superblock.
constexpr uint32_t MAX_VDEVS_IN_SYSTEM{1024};

// Minimum chunk size per device class.  Smaller chunks mean more chunks and a larger superblock area; the fast
// device floor is higher to cap chunk count (and thus superblock area) on the more expensive medium.
constexpr uint64_t MIN_CHUNK_SIZE_DATA_DEVICE{16ull * 1024 * 1024};
constexpr uint64_t MIN_CHUNK_SIZE_FAST_DEVICE{32ull * 1024 * 1024};

} // namespace homestore
