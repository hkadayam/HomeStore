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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>
#include <sisl/fds/concurrent_insert_set.h>
#include <sisl/fds/urcu_helper.h>

#include <homestore/homestore_decl.hpp> // shared<>, unique<>
#include "checkpoint/cp.h"              // cp_id_t
#include "checkpoint/cp_mgr.h"          // CPManager::max_concurent_cps

#include "meta/meta_blk.hpp" // MetaBlk

namespace homestore {

class Chunk;
class MetaClient;
class VirtualDev;
enum class ChunkToShrink : uint8_t;

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// StreamBase
//
// Owns an ordered list of Chunks for one stream (1 stream : N chunks). Hot-path reads (chunk lookup, offset resolution)
// are truly lock-free via sisl::urcu_data; writes (expand, truncate, destroy) are serialised by a folly::coro::Mutex so
// they can co_await VDev I/O safely.
//
// Concurrency model:
//   - chunks() — lock-free read under RCU.  Do NOT hold the returned _urcu_access_ptr across a co_await (RCU readers
//     must be short-lived).
//   - expand_to() / truncate_before() / destroy() — coroutines that acquire expand_mutex_ before mutating chunk list.
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class StreamBase {
protected:
    // ── Create / load constructors ────────────────────────────────────────────
    // Protected: StreamBase is intended to be subclassed; construct via a derived type.

    /// On fresh creation pass no mblks (default empty).  On recovery, chunks are extracted from VDev by chunk_id,
    /// sorted by creation_order, and each MetaBlk is moved into chunk_mblks_.
    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, IOBuffer > >;
    StreamBase(uint64_t stream_id, const shared< VirtualDev >& vdev, MetaClient& meta_client, std::string dev_name,
               uint64_t chunk_size, ChunkMblkMap&& mblks = {});

public:
    StreamBase(const StreamBase&) = delete;
    StreamBase& operator=(const StreamBase&) = delete;
    StreamBase(StreamBase&&) = delete;
    StreamBase& operator=(StreamBase&&) = delete;
    virtual ~StreamBase() = default;

    // ── Chunk-list read (lock-free) ───────────────────────────────────────────

    /// Returns an RCU accessor wrapping the current chunk vector. The accessor holds a folly::rcu_reader guard —
    /// release it (let it go out of scope) before the next co_await.
    sisl::_urcu_access_ptr< std::vector< shared< Chunk > > > chunks() const;

    /// Number of chunks currently in this stream (lock-free snapshot).
    size_t num_chunks() const;

    // ── Chunk-list mutations (serialised coroutines) ──────────────────────────

    /// Ensure at least n+1 chunks exist, expanding via vdev.expand() as needed.
    folly::coro::Task< void > expand_to(size_t nchunks);

    /// Release chunks [0 .. n) from the stream via vdev.shrink(). Remaining chunks keep their relative order; indices
    /// shift down by n.
    folly::coro::Task< void > truncate_before(size_t nchunks);

    /// Release all chunks via vdev.shrink() and leave the list empty.
    folly::coro::Task< void > destroy();

    // ── Accessors ─────────────────────────────────────────────────────────────
    uint64_t stream_id() const { return stream_id_; }
    uint64_t chunk_size() const { return chunk_size_; }
    uint32_t block_size() const { return blk_size_; }
    VirtualDev& vdev() const { return *vdev_; }
    const std::string& dev_name() const { return dev_name_; }

    // ── Subclass hook ────────────────────────────────────────────────────────
    /// Returns the stream type name used in per-chunk MetaBlk names, e.g. "rawblk", "appendblk", "appendbyte".
    virtual std::string_view stream_type_name() const = 0;

    // ── Per-CP dirty chunk tracking ─────────────────────────────────────────
    // Base CPSession tracks which chunks were dirtied during a CP epoch via ConcurrentInsertSet — lock-free per-thread
    // insert, deduped gather at flush time. An atomic dirty flag provides a fast O(1) check so cp_flush can skip the
    // gather entirely when nothing was dirtied. Subclasses can extend (e.g. RawBlkCPSession adds write buffers).
    // Indexed by cp_id % CPManager::max_concurent_cps (double-buffered).
    struct CPSession {
        std::atomic< bool > is_dirty{false};
        sisl::ConcurrentInsertSet< uint32_t > dirty_chunks;

        void mark_chunk_dirty(uint32_t chunk_id) {
            is_dirty.store(true, std::memory_order_relaxed);
            dirty_chunks.insert(chunk_id);
        }

        bool has_dirty_chunks() const { return is_dirty.load(std::memory_order_relaxed); }

        // Returns the deduped set of dirty chunk_ids and resets both the set and the dirty flag.
        std::unordered_set< uint32_t > gather_dirty_chunks() {
            is_dirty.store(false, std::memory_order_relaxed);
            return dirty_chunks.gather(/*clear_on_gather=*/true);
        }
    };

    CPSession& cp_session(cp_id_t cp_id) { return cp_session_[cp_id % CPManager::max_concurent_cps]; }

protected:
    CPSession cp_session_[CPManager::max_concurent_cps];
    // Single MetaClient owned by BlobDevManager; one MetaBlk per chunk stored in chunk_mblks_.
    MetaClient& meta_client_;
    folly::coro::Mutex mblk_mutex_;
    std::unordered_map< uint32_t, MetaBlk > chunk_mblks_;

private:
    /// Allocate a MetaBlk for the given chunk and store it in chunk_mblks_. Called automatically by expand_to() for
    /// each newly added chunk.
    folly::coro::Task< void > init_chunk_mblk(const shared< Chunk >& chunk);

    uint64_t stream_id_;
    shared< VirtualDev > vdev_;
    std::string dev_name_;
    uint64_t chunk_size_;
    uint32_t blk_size_;

    // RCU-protected chunk list. Readers take an rcu_reader guard (~2-5 ns). Writers call make_and_exchange() under
    // expand_mutex_ which invokes folly::synchronize_rcu() to wait for any in-flight readers.
    sisl::urcu_data< std::vector< shared< Chunk > > > chunks_;

    // Serialises all mutations. folly::coro::Mutex is safe to hold across co_await; std::mutex is not.
    folly::coro::Mutex expand_mutex_;
};

} // namespace homestore
