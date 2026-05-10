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
#pragma once

#include <atomic>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <folly/coro/Task.h>
#include "sisl/fds/buffer.h"

#include "common/defs.h"

#include "homestore/meta/meta_blk.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/managers.h"
#include "homestore/meta/meta_client.h"

namespace homestore {

/// Atomic counter used to auto-generate unique names when the caller passes "".
inline std::atomic< uint64_t > g_module_counter{0};

// ──────────────────────────────────────────────────────────────────────────────
// ModuleMetaBlk<T>
//
// A typed, single-block metadata wrapper. Analogous to the previous superblk<T> pattern but built on top of the new
// MetaClient/MetaBlk layer.
//
// # Type requirements for T
//   - std::is_trivially_copyable<T>   (safe to memcpy)
//   - std::is_standard_layout<T>      (predictable field offsets)
//   - Default-constructible           (used on fresh creation)
//
// # Concurrency model
//   ModuleMetaBlk<T> itself is NOT thread-safe. The caller must provide external synchronisation (e.g.
//   folly::coro::Mutex) if concurrent access is needed. The underlying MetaClient has its own internal locking that
//   only protects its chain bookkeeping.
//
// # Usage
//   struct alignas(8) MyConfig { uint32_t version; uint64_t flags; };
//
//   // Create or recover (auto-detects based on whether the slot exists):
//   auto cfg = co_await ModuleMetaBlk<MyConfig>::open("my_module");
//
//   // Modify in-place:
//   cfg->version = 2;
//   cfg->flags  |= 0x1;
//
//   // Persist:
//   co_await cfg.write();
// ──────────────────────────────────────────────────────────────────────────────
template < typename T >
class ModuleMetaBlk {
    static_assert(std::is_trivially_copyable_v< T >, "ModuleMetaBlk<T>: T must be trivially copyable");
    static_assert(std::is_standard_layout_v< T >, "ModuleMetaBlk<T>: T must have standard layout");

public:
    // ── Factory ──────────────────────────────────────────────────────────────

    /// Create or recover a ModuleMetaBlk.
    ///
    /// - If `name` is empty, an auto-generated unique name is used.
    /// - If `size` is nullopt, sizeof(T) is used as the buffer size.
    /// - On first call (no existing data): T is default-constructed in the buffer; the block is allocated but NOT
    ///   written until write() is called.
    /// - On recovery (existing data found): the recovered ByteArray is used directly; the T it contains is accessible
    ///   immediately.
    static folly::coro::Task< ModuleMetaBlk< T > > open(std::string name, std::optional< size_t > size = std::nullopt) {
        if (name.empty()) {
            name = "meta_blk_" + std::to_string(g_module_counter.fetch_add(1, std::memory_order_relaxed));
        }
        const size_t buf_sz = size.value_or(sizeof(T));

        // Register client through the global MetaBlkManager.
        MetaClient client = co_await meta_mgr().register_client(name);

        const size_t n_blks = co_await client.num_meta_blks();

        if (n_blks > 0) {
            co_return co_await load_existing(std::move(client), name, buf_sz);
        } else {
            co_return co_await create_new(std::move(client), name, buf_sz);
        }
    }

    // ── Typed data access ─────────────────────────────────────────────────────

    /// Direct pointer to T within the buffer (read-only).
    const T* get() const { return reinterpret_cast< const T* >(buffer_->cbytes()); }

    /// Direct pointer to T within the buffer (mutable). Modifications are in-place; call write() to persist.
    T* get() { return reinterpret_cast< T* >(buffer_->bytes()); }

    /// Operator overloads for ergonomic field access (cfg->field instead of cfg.get()->field).
    T* operator->() { return get(); }
    const T* operator->() const { return get(); }
    T& operator*() { return *get(); }
    const T& operator*() const { return *get(); }

    // ── Persistence ───────────────────────────────────────────────────────────

    /// Persist the current contents of T to disk.
    folly::coro::Task< void > write() {
        co_await client_.write_meta_blk(meta_blk_, buffer_);
        is_persisted_ = true;
    }

    /// Remove this module's metadata from disk. After destroy() the object must not be used for further writes.
    folly::coro::Task< void > destroy() {
        if (is_persisted_) {
            co_await client_.remove_meta_blk(meta_blk_);
            is_persisted_ = false;
        }
    }

    // ── Metadata ──────────────────────────────────────────────────────────────

    std::string_view name() const { return name_; }
    size_t size() const { return buffer_->size(); }

    // ── Resize ────────────────────────────────────────────────────────────────

    /// Reallocate the buffer to new_size and reinitialise T with T{}.
    /// The caller must re-populate fields and call write() afterwards.
    T& resize(size_t new_size) {
        buffer_ = sisl::make_byte_array(to_u32(new_size));
        T default_val{};
        std::memcpy(buffer_->bytes(), &default_val, sizeof(T));
        return *get();
    }

    // ── Move-only ─────────────────────────────────────────────────────────────
    ModuleMetaBlk() = default;
    ModuleMetaBlk(ModuleMetaBlk&&) = default;
    ModuleMetaBlk& operator=(ModuleMetaBlk&&) = default;
    ModuleMetaBlk(const ModuleMetaBlk&) = delete;
    ModuleMetaBlk& operator=(const ModuleMetaBlk&) = delete;

private:
    MetaClient client_;
    MetaBlk meta_blk_;
    sisl::ByteArray buffer_;
    std::string name_;
    bool is_persisted_{false};

private:
    // ── Private factory helpers ───────────────────────────────────────────────
    static folly::coro::Task< ModuleMetaBlk< T > > load_existing(MetaClient client, std::string name,
                                                                 size_t /*buf_sz*/) {
        ModuleMetaBlk< T > m;
        bool found = false;

        co_await client.for_each_recovered_block(
            [&m, &found](const MetaBlk& blk, const sisl::ByteView& data) -> folly::coro::Task< void > {
                if (!found) {
                    if (data.size() < sizeof(T)) {
                        throw std::runtime_error{"ModuleMetaBlk::load_existing: recovered data too small"};
                    }
                    m.meta_blk_ = blk;
                    m.buffer_ = data.extract();
                    m.is_persisted_ = true;
                    found = true;
                }
                co_return;
            });

        if (!found) {
            throw std::runtime_error{"ModuleMetaBlk::load_existing: expected a recovered block but found none"};
        }

        m.name_ = std::move(name);
        m.client_ = std::move(client);
        co_return m;
    }

    static folly::coro::Task< ModuleMetaBlk< T > > create_new(MetaClient client, std::string name, size_t buf_sz) {
        ModuleMetaBlk< T > m;
        m.buffer_ = sisl::make_byte_array(to_u32(buf_sz));

        // Placement-initialise T with its default value.
        T default_val{};
        std::memcpy(m.buffer_->bytes(), &default_val, sizeof(T));

        // Pre-allocate the MetaBlk (not written to disk until write() is called).
        m.meta_blk_ = co_await client.create_meta_blk(name, buf_sz);
        m.name_ = std::move(name);
        m.client_ = std::move(client);
        m.is_persisted_ = false;
        co_return m;
    }
};

} // namespace homestore
