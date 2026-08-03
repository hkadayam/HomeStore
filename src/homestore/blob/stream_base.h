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

#include "common/async.h"
#include "sisl/fds/concurrent_insert_set.h"
#include "sisl/fds/rcu.h"

#include "homestore/base/homestore_decl.h" // shared<>, unique<>
#include "homestore/checkpoint/cp.h"       // cp_id_t
#include "homestore/checkpoint/cp_mgr.h"   // CPManager::max_concurent_cps

#include "homestore/meta/meta_blk.h" // MetaBlk

namespace homestore {

class Chunk;
class MetaClient;
class VirtualDev;
enum class ChunkToShrink : uint8_t;

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// StreamBase
//
// Owns an ordered list of Chunks for one stream (1 stream : N chunks). Hot-path reads (chunk lookup, offset resolution)
// are truly lock-free via sisl::Rcu::data writes (expand, truncate, destroy) are serialised by a folly::coro::Mutex so
// they can co_await VDev I/O safely.
//
// Concurrency model:
//   - chunks() — lock-free read under RCU.  Do NOT hold the returned Rcu::access_ptr across a co_await (RCU readers
//     must be short-lived).
//   - expand_to() / truncate_before() / destroy() — coroutines that acquire expand_mutex_ before mutating chunk list.
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class StreamBase {
protected:
    // ── Create / load constructors ────────────────────────────────────────────
    // Protected: StreamBase is intended to be subclassed; construct via a derived type.

    /// On fresh creation pass no mblks (default empty).  On recovery, chunks are extracted from VDev by chunk_id,
    /// sorted by vdev_order, and each MetaBlk is moved into chunk_mblks_.
    using ChunkMblkMap = std::unordered_map< uint32_t, std::pair< MetaBlk, sisl::IoBufView > >;
    StreamBase(uint64_t stream_id, const shared< VirtualDev >& vdev, MetaClient& meta_client, std::string dev_name,
               uint64_t chunk_size, uint32_t stream_blk_size = 0, ChunkMblkMap&& mblks = {});

public:
    StreamBase(const StreamBase&) = delete;
    StreamBase& operator=(const StreamBase&) = delete;
    StreamBase(StreamBase&&) = delete;
    StreamBase& operator=(StreamBase&&) = delete;
    virtual ~StreamBase() = default;

    // ── Chunk-list read (lock-free) ───────────────────────────────────────────

    /// Returns an RCU accessor wrapping the current chunk vector. The accessor holds a folly::rcu_reader guard —
    /// release it (let it go out of scope) before the next co_await.
    sisl::Rcu::access_ptr< std::vector< shared< Chunk > > > chunks() const;

    /// Number of chunks currently in this stream (lock-free snapshot).
    size_t num_chunks() const;

    /// Physical device bytes the stream currently occupies: the chunks it still holds.  truncate/shrink release
    /// chunks fully below the head, so this shrinks on truncation — it is the stream's live footprint, not its
    /// logical (tail_offset) size.
    uint64_t footprint_bytes() const { return num_chunks() * chunk_size_; }

    // ── Chunk-list mutations (serialised coroutines) ──────────────────────────

    /// Ensure at least n+1 chunks exist, expanding via vdev.expand() as needed.
    Async< void > expand_to(size_t nchunks);

    /// Release chunks [0 .. n) from the stream via vdev.shrink(). Remaining chunks keep their relative order; indices
    /// shift down by n.
    Async< void > truncate_before(size_t nchunks);

    /// Release all chunks via vdev.shrink() and leave the list empty.  Virtual so a subclass with its own per-stream
    /// metadata (e.g. AppendByteStream's sb MetaBlk) can remove that too, otherwise it would be orphaned on recovery.
    virtual Async< void > destroy();

    // ── Accessors ─────────────────────────────────────────────────────────────
    uint64_t stream_id() const { return stream_id_; }
    uint64_t chunk_size() const { return chunk_size_; }
    uint32_t block_size() const { return blk_size_; }

    /// Convert a byte count to the number of stream blocks required to cover it (round-up).  Saves callers
    /// from open-coding the `(size + blk_size - 1) / blk_size` dance every time they need to size an alloc.
    blk_count_t size_to_nblks(uint32_t size) const {
        return static_cast< blk_count_t >((size + blk_size_ - 1) / blk_size_);
    }

    uint32_t blk_multiplier() const { return blk_multiplier_; }
    VirtualDev& vdev() const { return *vdev_; }
    const std::string& dev_name() const { return dev_name_; }

    // ── Subclass hook ────────────────────────────────────────────────────────
    /// Returns the stream type name used in per-chunk MetaBlk names, e.g. "rawblk", "appendblk", "appendbyte".
    virtual std::string_view stream_type_name() const = 0;

    // ── Per-CP dirty chunk tracking ─────────────────────────────────────────
    // Base FlushSession tracks which chunks were dirtied during a CP epoch via ConcurrentInsertSet — lock-free
    // per-thread insert, deduped gather at flush time. An atomic dirty flag provides a fast O(1) check so cp_flush can
    // skip the gather entirely when nothing was dirtied. Subclasses can extend (e.g. RawBlkStream::CPSession adds write
    // buffers). Indexed by cp_id % CPManager::max_concurent_cps (double-buffered).
    struct FlushSessionBase {
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

protected:
    // Single MetaClient owned by BlobDevManager; one MetaBlk per chunk stored in chunk_mblks_.
    MetaClient& meta_client_;
    folly::coro::Mutex mblk_mutex_;
    std::unordered_map< uint32_t, MetaBlk > chunk_mblks_;

    /// Allocate a MetaBlk for the given chunk and store it in chunk_mblks_. Called automatically by expand_to() for
    /// each newly added chunk.  Subclasses that don't use per-chunk mblks (e.g. AppendByteStream/LogStream which
    /// maintain a single per-stream mblk carrying the chunk list) can override this to update their own mblk.
    virtual Async< void > init_chunk_mblk(const shared< Chunk >& chunk);

    /// Remove the MetaBlk for the given chunk_id (mirror of init_chunk_mblk).  Called automatically by
    /// truncate_before() and destroy() for every released chunk.  Subclasses can override alongside init_chunk_mblk.
    virtual Async< void > remove_chunk_mblk(uint32_t chunk_id);

    /// Install a pre-loaded chunk list on the stream.  Sorts by vdev_order and atomically installs into chunks_.
    /// Used by load paths where chunks are recovered by looking up chunk_ids from the subclass's stream sb rather
    /// than via the per-chunk MetaBlk ChunkMblkMap.  Must be called during single-threaded load, before any
    /// concurrent access starts.
    void install_chunks(std::vector< shared< Chunk > > chunks);

private:
    uint64_t stream_id_;
    shared< VirtualDev > vdev_;
    std::string dev_name_;
    uint64_t chunk_size_;
    uint32_t blk_size_;       // stream's effective block size (may be a multiple of vdev's block size)
    uint32_t blk_multiplier_; // blk_size_ / vdev_->block_size()

    // RCU-protected chunk list. Readers take an rcu_reader guard (~2-5 ns). Writers call make_and_exchange() under
    // expand_mutex_ which invokes folly::synchronize_rcu() to wait for any in-flight readers.
    sisl::Rcu::data< std::vector< shared< Chunk > > > chunks_;

    // Serialises all mutations. folly::coro::Mutex is safe to hold across co_await; std::mutex is not.
    folly::coro::Mutex expand_mutex_;
};

} // namespace homestore
