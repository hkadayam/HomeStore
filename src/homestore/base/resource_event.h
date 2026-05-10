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
#include <string>

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ResourceEvent
//
// Emergency / exceptional resource signals from lower modules to ResourceManager (and any other interested
// subscriber).  Published via EventManager::publish<ResourceEvent>(...).  ResourceManager subscribes at start.
//
// These are RARE by design — the steady-state path (dirty counts, log size, etc.) is RM's poll loop.  Use a
// ResourceEvent only when a module hits a wall that needs immediate cross-module action.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct ResourceEvent {
    enum class Kind : uint8_t {
        DiskFullOnWrite,         // a write attempt failed because the device is out of space
        LogStreamSpaceExhausted, // log stream couldn't reserve the next chunk
        BlkAllocFailed,          // alloc returned nullopt; no slab can satisfy
        MemAllocFailed,          // soft OOM from a tracked allocator
    };

    Kind        kind;
    std::string source;      // module identifier for diagnostics, e.g. "BlobDev/foo"
    uint64_t    payload{0};  // event-specific context (requested bytes, vdev_id, etc.)
};

} // namespace homestore
