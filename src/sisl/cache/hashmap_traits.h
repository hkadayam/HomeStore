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

namespace sisl {

// ───────────────────────────────────────────────── HashmapTraits<V> ──────────────────────────────────────────────────
// Customisation point that lets the hashmap call into V for refcount-aware behaviour without coupling the hashmap to
// any specific cache implementation.
//
// Default specialisation: V is NOT refcounted.  acquire / release are no-ops (the compiler inlines them away).
// is_unreferenced returns true so that erase_if_no_reference always succeeds for non-refcounted V.
//
// To make V participate in refcount semantics, specialise HashmapTraits<V> to provide:
//   - acquire(V&)         : increment refcount
//   - release(V&)         : decrement refcount
//   - is_unreferenced(V&) : true iff no live handles reference V (refcount == 0)
//
// See cache_node.h for the CacheNode<V> specialisation.  Cache-only concerns (size accounting) live in CacheTraits
// (also in cache_node.h), NOT here — keeping hashmap concerns separate from cache concerns.
template < typename V >
struct HashmapTraits {
    static constexpr bool refcounted = false;
    static void           acquire(V&) {}
    static void           release(V&) {}
    static bool           is_unreferenced(V const&) { return true; }
};

} // namespace sisl
