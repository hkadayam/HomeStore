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
#include <type_traits>

#include "sisl/fds/buffer.h"

#include "common/defs.h"
#include "common/homestore_assert.h"

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// LogBlob
//
// A bounded scatter-gather record passed to the LogStore / LogStream append APIs.  Up to kMaxParts IoBufSpan
// references are stored inline.  Bytes are NOT owned by LogBlob — IoBufSpan is a non-owning view; callers must
// keep the underlying memory alive until the next flush completes (same lifetime contract LogStream::append
// always had for its IoBufSpan argument).
//
// Trivially copyable so it can be embedded directly inside LogRecord and travel through StreamTracker.
//
// When a caller's source chain exceeds kMaxParts, the caller is responsible for coalescing upfront —
// allocating its own contiguous buffer (e.g., nuraft::buffer::alloc) and constructing a LogBlob from that
// single IoBufSpan.  LogBlob itself holds no allocation policy; can_build_trivially() answers the question.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct LogBlob {
    static constexpr uint8_t kMaxParts = 4;

    sisl::IoBufSpan parts[kMaxParts]{};
    uint8_t n_parts{0};

    LogBlob() = default;

    // Implicit single-IoBufSpan ctor — keeps existing LogStore callers (test_log_store et al.) working unchanged
    // when they pass a single IoBufSpan directly to quick_append / quick_write.
    LogBlob(sisl::IoBufSpan const& b) {
        parts[0] = b;
        n_parts = 1;
    }

    // Total bytes across all parts.
    size_t size() const {
        size_t t = 0;
        for (uint8_t i = 0; i < n_parts; ++i) {
            t += parts[i].size();
        }
        return t;
    }

    // True iff a chain of num_parts can be represented by LogBlob without coalescing.  Callers iterating a
    // multi-part source check this first: if true, just append() each part; if false, coalesce upfront into a
    // single contiguous buffer (caller-owned) and construct LogBlob from that.
    static constexpr bool can_build_trivially(size_t num_parts) { return num_parts <= kMaxParts; }

    // Append a single part to the LogBlob.  Asserts on overflow — caller must have checked
    // can_build_trivially() against the full chain size before starting to append.
    void append(sisl::IoBufSpan const& b) {
        HS_REL_ASSERT_LT(n_parts, kMaxParts, "LogBlob::append overflow; caller should have coalesced upfront");
        parts[n_parts++] = b;
    }
};
static_assert(std::is_trivially_copyable_v< LogBlob >,
              "LogBlob must be trivially copyable (StreamTracker requirement)");

} // namespace homestore
